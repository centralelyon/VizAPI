"""Revision-checked edit sessions backed by the shared TransitMap C++ core.

SQLite compare-and-swap permits multiple VizAPI workers. Core/LOOM work runs
outside the transaction; a concurrent writer wins at most once. Failed commands
leave the persisted Shape, style, and revision unchanged.
"""
from contextlib import contextmanager
import json
import os
from pathlib import Path
import sqlite3
import subprocess
import time
import uuid

from fastapi import HTTPException

from app.modules.decaligne_code.loom_runner import run_octi

MAX_BODY = 16 * 1024 * 1024
CORE_TIMEOUT = 45
SESSION_TTL = 2 * 60 * 60
MAX_SESSIONS = 128


def core_call(payload):
    binary = os.environ.get("TRANSITMAP_CORE_BIN", "/usr/local/bin/transitmap-core")
    try:
        result = subprocess.run(
            [binary], input=json.dumps(payload, allow_nan=False),
            text=True, capture_output=True, timeout=CORE_TIMEOUT, check=False,
        )
    except FileNotFoundError as exc:
        raise HTTPException(503, "TransitMap C++ core is not installed. Set TRANSITMAP_CORE_BIN.") from exc
    except subprocess.TimeoutExpired as exc:
        raise HTTPException(504, "TransitMap geometry computation timed out; current map was kept.") from exc
    except (OSError, ValueError) as exc:
        raise HTTPException(502, "Could not run TransitMap C++ core.") from exc
    try:
        data = json.loads(result.stdout)
    except (ValueError, TypeError) as exc:
        raise HTTPException(502, "TransitMap core returned an invalid response.") from exc
    if result.returncode != 0 or "error" in data:
        raise HTTPException(422, data.get("error", "TransitMap core failed."))
    return data


class EditSessions:
    def __init__(self, database=None, core=core_call, loom=run_octi):
        self.database = database
        self.core = core
        self.loom = loom

    @contextmanager
    def connect(self):
        path = Path(self.database or os.environ.get("TRANSITMAP_SESSION_DB", "data/transitmap-sessions.sqlite3"))
        path.parent.mkdir(parents=True, exist_ok=True)
        conn = sqlite3.connect(path, timeout=10)
        conn.execute("PRAGMA journal_mode=WAL")
        conn.execute("CREATE TABLE IF NOT EXISTS edit_sessions (id TEXT PRIMARY KEY, revision INTEGER NOT NULL, touched REAL NOT NULL, state TEXT NOT NULL)")
        conn.execute("""
            CREATE TABLE IF NOT EXISTS edit_checkpoints (
                id TEXT PRIMARY KEY,
                session_id TEXT NOT NULL,
                created REAL NOT NULL,
                state TEXT NOT NULL
            )
        """)
        conn.execute("""
            CREATE INDEX IF NOT EXISTS idx_edit_checkpoints_session
            ON edit_checkpoints(session_id)
        """)
        try:
            with conn:
                yield conn
        finally:
            conn.close()

    @staticmethod
    def public(result, session_id, revision):
        return {**{k: v for k, v in result.items() if k != "state"}, "sessionId": session_id, "revision": revision}

    def create(self, body):
        if not isinstance(body.get("map"), dict):
            raise HTTPException(422, "map must be a GeoJSON object")
        result = self.core({"op": "session", **{k: body[k] for k in ("map", "obstacles", "styles", "style", "bidirectionalRoutes") if k in body}})
        result["state"]["original"] = body["map"]
        sid = uuid.uuid4().hex
        checkpoint_id = uuid.uuid4().hex
        now = time.time()

        state_json = json.dumps(
            result["state"],
            allow_nan=False,
        )

        with self.connect() as conn:
            conn.execute(
                "DELETE FROM edit_sessions WHERE touched < ?",
                (now - SESSION_TTL,),
            )

            if conn.execute(
                "SELECT COUNT(*) FROM edit_sessions"
            ).fetchone()[0] >= MAX_SESSIONS:
                raise HTTPException(
                    429,
                    "Too many active edit sessions",
                )

            conn.execute(
                "INSERT INTO edit_sessions VALUES (?, 1, ?, ?)",
                (
                    sid,
                    now,
                    state_json,
                ),
            )

            conn.execute(
                """
                INSERT INTO edit_checkpoints
                    (id, session_id, created, state)
                VALUES (?, ?, ?, ?)
                """,
                (
                    checkpoint_id,
                    sid,
                    now,
                    state_json,
                ),
            )

        response = self.public(
            result,
            sid,
            1,
        )

        response["checkpointId"] = checkpoint_id

        return response

    def read(self, body):
        sid, revision = body.get("sessionId"), body.get("revision")
        if not isinstance(sid, str) or type(revision) is not int or revision < 1:
            raise HTTPException(422, "sessionId and integer revision are required")
        with self.connect() as conn:
            row = conn.execute("SELECT revision, touched, state FROM edit_sessions WHERE id=?", (sid,)).fetchone()
        if row is None or row[1] < time.time() - SESSION_TTL:
            raise HTTPException(404, "Edit session expired or does not exist. Reload the map.")
        if row[0] != revision:
            raise HTTPException(409, {"message": "Stale edit revision; reload the session before editing.", "revision": row[0]})
        return sid, revision, json.loads(row[2])

    def apply(self, operation, body):
        if operation == "snapshot":
            sid = body.get("sessionId")

            if not isinstance(sid, str):
                raise HTTPException(422, "sessionId is required")

            with self.connect() as conn:
                row = conn.execute(
                    "SELECT revision, touched, state "
                    "FROM edit_sessions WHERE id=?",
                    (sid,),
                ).fetchone()

            if row is None or row[1] < time.time() - SESSION_TTL:
                raise HTTPException(
                    404,
                    "Edit session expired or does not exist. Reload the map.",
                )

            revision = row[0]
            state = json.loads(row[2])

            checkpoint_id = uuid.uuid4().hex

            with self.connect() as conn:
                conn.execute(
                    """
                    INSERT INTO edit_checkpoints
                        (id, session_id, created, state)
                    VALUES (?, ?, ?, ?)
                    """,
                    (
                        checkpoint_id,
                        sid,
                        time.time(),
                        json.dumps(state, allow_nan=False),
                    ),
                )

            result = self.core({
                "op": "render-geometry",
                "state": state,
            })

            response = self.public(result, sid, revision)
            response["checkpointId"] = checkpoint_id

            return response

        if operation == "session":
            return self.create(body)

        if operation == "checkout":
            sid, revision, current_state = self.read(body)

            checkpoint_id = body.get("checkpointId")

            if not isinstance(checkpoint_id, str):
                raise HTTPException(
                    422,
                    "checkpointId is required",
                )

            with self.connect() as conn:
                row = conn.execute(
                    """
                    SELECT state
                    FROM edit_checkpoints
                    WHERE id=? AND session_id=?
                    """,
                    (checkpoint_id, sid),
                ).fetchone()

            if row is None:
                raise HTTPException(
                    404,
                    "Checkpoint does not exist for this session.",
                )

            checkpoint_state = json.loads(row[0])

            # Keep the original input map belonging to this session.
            checkpoint_state["original"] = current_state["original"]

            payload = {
                "op": "render-geometry",
                "state": checkpoint_state,
            }

            # Undo/redo may restore a different visual/geometry style state.
            if "styles" in body:
                payload["styles"] = body["styles"]

            result = self.core(payload)
            result["state"]["original"] = current_state["original"]

            with self.connect() as conn:
                updated = conn.execute(
                    """
                    UPDATE edit_sessions
                    SET revision=revision+1,
                        touched=?,
                        state=?
                    WHERE id=? AND revision=?
                    """,
                    (
                        time.time(),
                        json.dumps(result["state"], allow_nan=False),
                        sid,
                        revision,
                    ),
                ).rowcount

            if updated != 1:
                raise HTTPException(
                    409,
                    "Session changed while checking out checkpoint; "
                    "the stale result was discarded.",
                )

            response = self.public(
                result,
                sid,
                revision + 1,
            )

            response["checkpointId"] = checkpoint_id

            return response
        sid, revision, state = self.read(body)

        if operation == "close":
            with self.connect() as conn:
                deleted = conn.execute(
                    "DELETE FROM edit_sessions WHERE id=? AND revision=?",
                    (sid, revision),
                ).rowcount

                if deleted:
                    conn.execute(
                        "DELETE FROM edit_checkpoints WHERE session_id=?",
                        (sid,),
                    )

            if not deleted:
                raise HTTPException(
                    409,
                    "Session changed while closing",
                )

            return {
                "sessionId": sid,
                "revision": revision,
                "closed": True,
            }
            if not deleted:
                raise HTTPException(409, "Session changed while closing")
            return {"sessionId": sid, "revision": revision, "closed": True}
            
        allowed = ("nodeId", "nodeIds", "segmentId", "segmentIds", "routeId", "routeIds", "x", "y", "snap", "name", "stationId", "offset", "styles", "style", "bidirectionalRoutes")
        payload = {k: body[k] for k in allowed if k in body}
        payload.update(op=operation, state=state)
        if operation == "loom":
            graph = self.core({"op": "loom-export", "state": state})["map"]
            # Embedded visual data is not LOOM input.
            graph.pop("transitMapObstacles", None)
            try:
                new_map = self.loom(graph)
            except (OSError, RuntimeError) as exc:
                raise HTTPException(502, str(exc)) from exc
            # OCTI may omit the optional name field. Route IDs are identity,
            # labels are display text; recover metadata from the current Shape.
            routes = {r["id"]: r for r in state["shape"]["routes"]}
            for feature in new_map.get("features", []):
                for line in (feature.get("properties") or {}).get("lines", []):
                    route = routes.get(str(line.get("id", "")))
                    if route:
                        line.update(name=route["name"], label=route["name"])
            if "directionalData" in state:
                new_map["transitMapDirections"] = state["directionalData"]
                new_map["bidirectionalRoutes"] = state.get("bidirectionalRoutes", [])
            payload.update(op="replace-map", map=new_map, obstacles=state["obstacles"], preserveRouteDirections=True)
        elif operation == "restore":
            payload.update(op="replace-map", map=state["original"], obstacles=state["obstacles"])
        result = self.core(payload)
        if operation == "export":
            # Export is a snapshot at the requested revision, not a mutation.
            return self.public(result, sid, revision)
        result["state"]["original"] = state["original"]

        new_revision = revision + 1
        now = time.time()
        checkpoint_id = uuid.uuid4().hex

        state_json = json.dumps(
            result["state"],
            allow_nan=False,
        )

        with self.connect() as conn:
            updated = conn.execute(
                """
                UPDATE edit_sessions
                SET revision=revision+1,
                    touched=?,
                    state=?
                WHERE id=? AND revision=?
                """,
                (
                    now,
                    state_json,
                    sid,
                    revision,
                ),
            ).rowcount

            if updated == 1:
                conn.execute(
                    """
                    INSERT INTO edit_checkpoints
                        (id, session_id, created, state)
                    VALUES (?, ?, ?, ?)
                    """,
                    (
                        checkpoint_id,
                        sid,
                        now,
                        state_json,
                    ),
                )

        if updated != 1:
            raise HTTPException(
                409,
                "Session changed while computing; "
                "the stale result was discarded.",
            )

        response = self.public(
            result,
            sid,
            new_revision,
        )

        response["checkpointId"] = checkpoint_id

        return response
