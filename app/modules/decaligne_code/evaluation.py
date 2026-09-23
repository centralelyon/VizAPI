"""Durable semantic JSONL events. No evaluation data is stored in runtime SQLite.

POSIX file locks serialize workers (the supported deployment is Linux/Docker).
Bindings and locks live in evaluation/.sessions, separate from runtime state.
"""
from contextlib import contextmanager
from datetime import datetime, timezone
import fcntl
import json
import logging
import os
import re
import time
from fastapi import HTTPException
from .storage import data_root

logger = logging.getLogger(__name__)
IDENTIFIER = re.compile(r'^[A-Za-z0-9_-]{1,80}$')
UI_ACTIONS = {'session-start', 'style-change', 'style-select', 'mode-change',
              'edit-mode', 'side-change', 'snap-change', 'undo', 'redo'}
OPERATIONS = {'move-node', 'delete-node', 'merge-stations', 'split-station',
              'split-segment', 'add-station', 'add-route', 'delete-route',
              'restore', 'loom', 'upload'}


def validate_id(value):
    if not isinstance(value, str) or not IDENTIFIER.fullmatch(value):
        raise HTTPException(422, 'Invalid evaluation identifier')
    return value


def _session_root():
    root = data_root() / "evaluation" / ".sessions"
    root.mkdir(parents=True, exist_ok=True)
    return root


def _evaluation_root():
    root = data_root() / "evaluation"
    root.mkdir(parents=True, exist_ok=True)
    return root


def _binding_path(session):
    return _session_root() / f"{session}.json"


def _read_binding(session):
    path = _binding_path(session)

    if not path.exists():
        return None

    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError, KeyError) as exc:
        raise HTTPException(
            503,
            "Could not read evaluation session binding",
        ) from exc


def _next_evaluation_id():

    root = _session_root()

    counter_path = root / "counter"
    lock_path = root / "counter.lock"

    with lock_path.open("a+b") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)

        try:
            current = 0

            if counter_path.exists():
                text = counter_path.read_text(
                    encoding="utf-8"
                ).strip()

                if text:
                    current = int(text)

            next_id = current + 1

            temporary = root / "counter.tmp"

            with temporary.open(
                "w",
                encoding="utf-8",
            ) as file:
                file.write(str(next_id))
                file.flush()
                os.fsync(file.fileno())

            temporary.replace(counter_path)

            return f"{next_id:06d}"

        finally:
            fcntl.flock(lock, fcntl.LOCK_UN)


def bind(participant, session):
    validate_id(participant)
    validate_id(session)

    evaluation_id = _next_evaluation_id()

    binding = {
        "participantId": participant,
        "evaluationId": evaluation_id,
        "completed": False,
    }

    path = _binding_path(session)

    with path.open(
        "x",
        encoding="utf-8",
    ) as file:
        json.dump(
            binding,
            file,
            ensure_ascii=False,
        )
        file.flush()
        os.fsync(file.fileno())

    return evaluation_id


def evaluation_id(session):
    """
    Return the sequential evaluation ID belonging to a runtime session.
    """
    validate_id(session)

    binding = _read_binding(session)

    if binding is None:
        raise HTTPException(
            409,
            "Session was created without evaluation logging",
        )

    value = binding.get("evaluationId")

    if not isinstance(value, str):
        raise HTTPException(
            503,
            "Evaluation session has no evaluation ID",
        )

    return value


def artifact_path(session, suffix):
    """
        000001_input.svg
        000001_output.svg
        000001_output.json
        000001_log.jsonl
    """
    eid = evaluation_id(session)

    return _evaluation_root() / f"{eid}_{suffix}"


@contextmanager
def session_lock(session, participant):
    validate_id(session)

    root = _session_root()
    binding_path = _binding_path(session)

    if not binding_path.exists():
        # Compatibility with sessions created without evaluation.
        yield None
        return

    lock_path = root / f"{session}.lock"

    with lock_path.open("a+b") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)

        try:
            binding = _read_binding(session)

            if binding is None:
                yield None
                return

            owner = binding.get("participantId")

            if participant != owner:
                raise HTTPException(
                    403,
                    "Evaluation participant does not match this session",
                )

            eid = binding.get("evaluationId")

            if not isinstance(eid, str):
                raise HTTPException(
                    503,
                    "Evaluation session has no evaluation ID",
                )

            yield _evaluation_root() / f"{eid}_log.jsonl"

        finally:
            fcntl.flock(lock, fcntl.LOCK_UN)


def append(path, participant, session, revision, events):
    """Caller owns session_lock. Retry IDs deduplicate transport retries."""
    if not events:
        return
    with path.open('a+b') as file:
        file.seek(0)
        raw = file.read()
        # A killed writer may leave an incomplete last line; discard only that tail.
        if raw and not raw.endswith(b'\n'):
            raw = raw[:raw.rfind(b'\n') + 1]
            file.truncate(len(raw))
        rows = [json.loads(line) for line in raw.splitlines()]
        seen = {row['eventId'] for row in rows}
        started = any(row['action'] == 'session-start' for row in rows)
        for event in events:
            event_id = validate_id(event['eventId'])
            if event_id in seen:
                continue
            if event['action'] == 'session-start':
                if started:
                    continue
                started = True
            elif not started:
                raise HTTPException(409, 'Evaluation canvas has not started')
            row = {**event, 'time': datetime.now(timezone.utc).isoformat(timespec='milliseconds').replace('+00:00', 'Z'),
                   'participantId': participant, 'sessionId': session,
                   'revision': revision, 'sequence': len(rows) + 1}
            encoded = (json.dumps(row, ensure_ascii=False, allow_nan=False, separators=(',', ':')) + '\n').encode()
            file.write(encoded)
            rows.append(row); seen.add(event_id)
        file.flush(); os.fsync(file.fileno())


def ui_events(body):
    events = body.get('events')
    if not isinstance(events, list) or not 1 <= len(events) <= 64:
        raise HTTPException(422, 'Expected 1 to 64 semantic events')
    if len(json.dumps(events, allow_nan=False).encode()) > 256 * 1024:
        raise HTTPException(413, 'Evaluation batch is too large')
    result = []
    allowed = {'eventId', 'action', 'side', 'mode', 'city', 'inputFile', 'styleName',
               'from', 'to', 'component', 'property', 'target', 'changes', 'enabled',
               'status', 'fromCheckpointId', 'toCheckpointId', 'historyIndex', 'historyTarget',
               'checkpointId', 'source', 'targetKind', 'scope', 'routeNames'}
    for event in events:
        if not isinstance(event, dict) or event.get('action') not in UI_ACTIONS:
            raise HTTPException(422, 'Unsupported semantic event')
        validate_id(event.get('eventId'))
        # Browser time, participant, session, revision and sequence cannot override server fields.
        result.append({key: value for key, value in event.items() if key in allowed})
    return result


def operation_event(operation, body, before, result, elapsed, failed=False):
    if operation not in OPERATIONS and not (operation == 'render-geometry' and 'bidirectionalRoutes' in body):
        return None
    context = body.get('evaluationContext') or {}
    event = {'eventId': validate_id(body.get('eventId')), 'action': operation,
             'status': 'failed' if failed else 'success'}
    if context.get('side') in ('left', 'right'):
        event['side'] = context['side']
    names = {'loom':'octi', 'restore':'restore-original', 'merge-stations':'merge-interchange',
             'split-station':'split-interchange', 'delete-node':'delete-node', 'render-geometry':'bidirectional-change'}
    event['action'] = names.get(operation, operation)
    for key in ('nodeId', 'nodeIds', 'segmentId', 'segmentIds', 'routeId', 'routeIds', 'name'):
        if key in body:
            event[key] = body[key]
    before_nodes = {n['id']: n for n in before.get('shape', {}).get('nodes', [])}
    after_nodes = {n['id']: n for n in (result or {}).get('shape', {}).get('nodes', [])}
    if operation == 'move-node':
        old = before_nodes.get(body.get('nodeId'), {})
        new = after_nodes.get(body.get('nodeId'), {})
        event.update(action='move-station' if old.get('isStation') else 'move-node',
                     station=old.get('stationId') or body.get('nodeId'),
                     **{'from': old.get('position'), 'to': new.get('position'),
                        'requestedTo': [body.get('x'), body.get('y')], 'coordinateSystem':'canvas', 'snap':body.get('snap',False)})
    if operation in {'add-station', 'split-station', 'split-segment', 'merge-stations', 'delete-node'} and result:
        event['addedNodeIds'] = sorted(after_nodes.keys() - before_nodes.keys())
        event['removedNodeIds'] = sorted(before_nodes.keys() - after_nodes.keys())
    if operation == 'render-geometry':
        event.update({'from':before.get('bidirectionalRoutes', []), 'to':(result or {}).get('bidirectionalRoutes')})
    if operation in {'loom', 'upload'}:
        event['durationMs'] = round(elapsed * 1000, 3)
    if operation == 'upload' and not failed:
        event['submissionId'] = result['uploadId']
    return event


def warning_append(path, participant, session, revision, events):

    try:
        append(path, participant, session, revision, events)
    except (OSError, ValueError, HTTPException):
        logger.exception('Evaluation log write failed for session %s', session)
        return 'Evaluation log could not be written; the operation result was kept.'
    return None

MAX_SVG_SIZE = 16 * 1024 * 1024


def save_svg(participant, session, kind, svg):

    validate_id(participant)
    validate_id(session)

    if kind not in ("input", "output"):
        raise HTTPException(
            422,
            "Invalid evaluation SVG kind",
        )

    if not isinstance(svg, str):
        raise HTTPException(
            422,
            "SVG must be a string",
        )

    encoded = svg.encode("utf-8")

    if len(encoded) > MAX_SVG_SIZE:
        raise HTTPException(
            413,
            "Evaluation SVG is too large",
        )

    if "<svg" not in svg[:4096]:
        raise HTTPException(
            422,
            "Invalid SVG document",
        )

    binding = _read_binding(session)

    if binding is None:
        raise HTTPException(
            409,
            "Evaluation session does not exist",
        )

    if binding.get("participantId") != participant:
        raise HTTPException(
            403,
            "Evaluation participant does not match this session",
        )

    path = artifact_path(
        session,
        f"{kind}.svg",
    )

    temporary = path.with_suffix(
        path.suffix + ".tmp"
    )

    try:
        with temporary.open(
            "w",
            encoding="utf-8",
        ) as file:
            file.write(svg)
            file.flush()
            os.fsync(file.fileno())

        temporary.replace(path)

    finally:
        if temporary.exists():
            temporary.unlink(missing_ok=True)

    return path


def close(session, participant):
    """
    Mark an evaluation task as completed after Upload.
    """
    validate_id(session)
    validate_id(participant)

    path = _binding_path(session)

    binding = _read_binding(session)

    if binding is None:
        raise HTTPException(
            409,
            "Evaluation session does not exist",
        )

    if binding.get("participantId") != participant:
        raise HTTPException(
            403,
            "Evaluation participant does not match this session",
        )

    binding["completed"] = True
    binding["completedAt"] = (
        datetime.now(timezone.utc)
        .isoformat(timespec="milliseconds")
        .replace("+00:00", "Z")
    )

    temporary = path.with_suffix(".tmp")

    try:
        with temporary.open(
            "w",
            encoding="utf-8",
        ) as file:
            json.dump(
                binding,
                file,
                ensure_ascii=False,
            )
            file.flush()
            os.fsync(file.fileno())

        temporary.replace(path)

    finally:
        if temporary.exists():
            temporary.unlink(missing_ok=True)