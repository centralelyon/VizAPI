import subprocess
import tempfile

from pathlib import Path

from fastapi import HTTPException, Query, Request
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


@module.router.post("/gtfs/routes")
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