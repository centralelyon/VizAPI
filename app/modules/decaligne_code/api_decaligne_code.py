import subprocess
import tempfile

from pathlib import Path

from fastapi import File, HTTPException, Query, Request, UploadFile
from starlette.concurrency import run_in_threadpool

from app.modules.decaligne_code.gtfs_runner import (
    MAX_UPLOAD,
    GtfsBusyError,
    run_gtfs,
    list_gtfs_routes,
)
from app.modules.decaligne_code.loom_runner import run_octi
from app.types.module import Module


root = "decaligne-code"

module = Module(
    root=root,
    tag="Decaligne",
)


@module.router.get("/ping")
def ping():
    return {"status": "ok"}


@module.router.post("/octi")
def octi(graph: dict):
    try:
        return run_octi(graph)

    except RuntimeError as exc:
        raise HTTPException(
            status_code=500,
            detail=str(exc),
        ) from exc


@module.router.post(
    "/gtfs/routes",
    include_in_schema=False,
)
async def gtfs_routes(request: Request):
    try:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "feed.zip"

            size = 0

            with path.open("wb") as output:
                async for chunk in request.stream():
                    size += len(chunk)

                    if size > MAX_UPLOAD:
                        raise HTTPException(
                            413,
                            "ZIP file must be at most 100 MB",
                        )

                    output.write(chunk)

            return await run_in_threadpool(
                list_gtfs_routes,
                path,
            )

    except ValueError as exc:
        raise HTTPException(
            400,
            str(exc),
        ) from exc


@module.router.post("/gtfs")
async def gtfs(
    request: Request,
    route_id: list[str] = Query(default=[]),
):
    try:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "feed.zip"

            size = 0

            with path.open("wb") as output:
                async for chunk in request.stream():
                    size += len(chunk)

                    if size > MAX_UPLOAD:
                        raise HTTPException(
                            413,
                            "ZIP file must be at most 100 MB",
                        )

                    output.write(chunk)

            return await run_in_threadpool(
                run_gtfs,
                path,
                route_id,
            )

    except GtfsBusyError as exc:
        raise HTTPException(
            429,
            str(exc),
        ) from exc

    except ValueError as exc:
        raise HTTPException(
            400,
            str(exc),
        ) from exc

    except subprocess.TimeoutExpired as exc:
        raise HTTPException(
            504,
            "GTFS conversion timed out.",
        ) from exc

    except (RuntimeError, OSError) as exc:
        raise HTTPException(
            502,
            str(exc),
        ) from exc

from app.modules.decaligne_code.input_runner import inspect_input, convert_input


async def _uploaded_files(files: list[UploadFile]):
    result = []
    total = 0
    for upload in files:
        data = await upload.read()
        total += len(data)
        if total > MAX_UPLOAD:
            raise HTTPException(413, "Input files must be at most 100 MB total")
        result.append((upload.filename or "input", data))
    return result


@module.router.post("/input/routes")
async def input_routes(files: list[UploadFile] = File(...)):
    try:
        payload = await _uploaded_files(files)
        return await run_in_threadpool(inspect_input, payload)
    except ValueError as exc:
        raise HTTPException(400, str(exc)) from exc


@module.router.post("/input")
async def input_convert(files: list[UploadFile] = File(...), route_id: list[str] = Query(default=[])):
    try:
        payload = await _uploaded_files(files)
        return await run_in_threadpool(convert_input, payload, route_id)
    except GtfsBusyError as exc:
        raise HTTPException(429, str(exc)) from exc
    except ValueError as exc:
        raise HTTPException(400, str(exc)) from exc
    except subprocess.TimeoutExpired as exc:
        raise HTTPException(504, "GTFS conversion timed out.") from exc
    except (RuntimeError, OSError) as exc:
        raise HTTPException(502, str(exc)) from exc

# Reuse this module's service, proxy path and existing VizAPI lifecycle.
from app.modules.decaligne_code.edit_router import router as edit_router
from app.modules.decaligne_code.storage import router as storage_router

module.router.include_router(edit_router)
module.router.include_router(storage_router)

from app.modules.decaligne_code.background_image import router as background_router
module.router.include_router(background_router)
