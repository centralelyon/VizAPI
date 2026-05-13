import os
import dotenv

import logging
from app.modules.aquanote import services_aquanote as aquanote_service
from app.types.module import Module
from fastapi.staticfiles import StaticFiles

root = "aquanote"
module = Module(
    root=root,
    tag="Aquanote",
)

aquanote_logger = logging.getLogger("aquanote")


# Mount the static directory
dotenv.load_dotenv()
static_dir = os.path.join(
    os.path.dirname(__file__),
    "..",
    "..",
    "..",
    os.getenv("DATA_PATH_MODULES", ""),
    root,
    "courses_demo",
)
module.router.mount("/files", StaticFiles(directory=static_dir), name="courses_demo")


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
