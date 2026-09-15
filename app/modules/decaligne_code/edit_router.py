import json
from fastapi import APIRouter, HTTPException, Request
from starlette.concurrency import run_in_threadpool
from app.modules.decaligne_code.edit_sessions import EditSessions, MAX_BODY

router = APIRouter()
sessions = EditSessions()


def endpoint(operation):
    async def handle(request: Request):
        contents = bytearray()
        async for chunk in request.stream():
            contents.extend(chunk)
            if len(contents) > MAX_BODY:
                raise HTTPException(413, "Edit request must be at most 16 MB")
        try:
            def reject_constant(value):
                raise ValueError(f"Invalid numeric constant: {value}")
            body = json.loads(contents, parse_constant=reject_constant)
            if not isinstance(body, dict):
                raise ValueError("Expected an object")
        except (ValueError, UnicodeDecodeError) as exc:
            raise HTTPException(400, "Expected a valid JSON object") from exc
        return await run_in_threadpool(sessions.apply, operation, body)
    handle.__name__ = "edit_" + operation.replace("-", "_")
    return handle


for operation in ("session", "move-node", "delete-node", "add-station", "merge-stations", 
                  "split-station", "split-segment", "add-route", "delete-route", 
                  "render-geometry", "snapshot", "checkout", "loom", "export", "restore", "close"): 
    router.add_api_route("/edit/" + operation, endpoint(operation), methods=["POST"])
