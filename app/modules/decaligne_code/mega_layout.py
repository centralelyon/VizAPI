"""Transport adapter for composite operations implemented in transitmap_core."""

import json
import os
import subprocess


def _call(op, state, **options):
    result = subprocess.run(
        [os.environ.get("TRANSITMAP_CORE_BIN", "/usr/local/bin/transitmap-core")],
        input=json.dumps(
            dict(op=op, state=state, **options), allow_nan=False, separators=(",", ":")
        ),
        text=True,
        capture_output=True,
        timeout=120,
    )
    data = json.loads(result.stdout)
    if result.returncode or "error" in data:
        raise ValueError(data.get("error", "Composite operation failed"))
    return data


def optimize(state, body, after_octi=False, before=None):
    options = {
        k: body[k]
        for k in (
            "members",
            "method",
            "kind",
            "points",
            "rotate",
            "containerId",
            "symbolStyles",
            "spacingScale",
        )
        if k in body
    }
    args = dict(options=options, afterOcti=after_octi)
    if before is not None:
        args["before"] = before
    updated = _call("mega-layout", state, **args)["state"]
    state.clear()
    state.update(updated)


def remove(state, container_id=None):
    updated = _call("mega-remove", state, containerId=container_id or "")["state"]
    state.clear()
    state.update(updated)


def validate_edit(state):
    _call("mega-validate", state)
