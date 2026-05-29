from contextlib import asynccontextmanager
from urllib.parse import urlparse

from fastapi import FastAPI
from fastapi.middleware.cors import CORSMiddleware

from app.core.config import construct_settings
from app.module import all_modules
from app.shared.db.database import Base, engine

_settings = construct_settings()
_root_path = (
    urlparse(_settings.SERVER_BASE_URL).path.rstrip("/")
    if _settings.SERVER_BASE_URL
    else ""
)


def _ensure_module_data_dirs() -> None:
    required_dirs = [
        directory
        for module in all_modules
        for directory in getattr(module, "data_dirs", [])
    ]
    for directory in required_dirs:
        directory.mkdir(parents=True, exist_ok=True)


@asynccontextmanager
async def lifespan(app: FastAPI):
    _ensure_module_data_dirs()
    async with engine.begin() as conn:
        await conn.run_sync(Base.metadata.create_all)
    yield
    await engine.dispose()


app = FastAPI(
    title="VizAPI",
    version="0.0.1",
    lifespan=lifespan,
    root_path=_root_path,
)


app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],  # Adjust this to specify allowed origins in prod (use .env)
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)

for module in all_modules:
    app.include_router(module.router, prefix=f"/{module.root}")
    for path, sub_app, name in getattr(module, "mounts", []):
        app.mount(f"/{module.root}{path}", sub_app, name=name)
