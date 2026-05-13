import os
import logging
from app.modules.aquanote import services_aquanote as aquanote_service
from app.types.module import Module
from app.core.config import construct_settings
from fastapi.staticfiles import StaticFiles

root = "aquanote"
module = Module(
    root=root,
    tag="Aquanote",
)

aquanote_logger = logging.getLogger("aquanote")

_settings = construct_settings()
_static_dir = os.path.join(
    os.path.dirname(__file__),
    "..",
    "..",
    "..",
    _settings.DATA_PATH_MODULES,
    root,
    "courses_demo",
)
module.mounts.append(("/files", StaticFiles(directory=_static_dir), "courses_demo"))


@module.router.get(
    "/getCompets",
    status_code=200,
)
async def get_compets():
    return aquanote_service.get_compets()


@module.router.get(
    "/getRuns/{compet_id}",
    status_code=200,
)
async def get_runs(compet_id: str):
    return aquanote_service.get_runs(compet_id)


@module.router.get(
    "/getDatas/{compet_id}/{run_id}",
    status_code=200,
)
async def get_data(compet_id: str, run_id: str):
    return aquanote_service.get_datas(compet_id, run_id)


@module.router.get(
    "/getQuality/{compet_id}/{run_id}",
    status_code=200,
)
async def get_quality(compet_id: str, run_id: str):
    return aquanote_service.get_quality(compet_id, run_id)
