from contextlib import asynccontextmanager
from urllib.parse import urlparse

from fastapi import Depends, FastAPI
from fastapi.middleware.cors import CORSMiddleware

from app.core.config import construct_settings
from app.core.security.basic_auth import require_basic_auth
from app.module import core_modules, module_list
from app.shared.db.database import Base, engine
from app.types.middleware import RateLimitMiddleware


_settings = construct_settings()
_root_path = (
    urlparse(_settings.SERVER_BASE_URL).path.rstrip("/")
    if _settings.SERVER_BASE_URL
    else ""
)


def _ensure_module_data_dirs() -> None:
    required_dirs = [
        directory
        for module in [*core_modules, *module_list]
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

app.add_middleware(
    RateLimitMiddleware,
    requests_per_window=_settings.RATE_LIMIT_REQUESTS,
    window_seconds=_settings.RATE_LIMIT_WINDOW_SECONDS,
)

for module in core_modules:
    app.include_router(module.router, prefix=f"/{module.root}")
    for path, sub_app, name in getattr(module, "mounts", []):
        app.mount(f"/{module.root}{path}", sub_app, name=name)

for module in module_list:
    app.include_router(
        module.router,
        prefix=f"/{module.root}",
        # dependencies=[Depends(require_basic_auth)],
    )
    for path, sub_app, name in getattr(module, "mounts", []):
        app.mount(f"/{module.root}{path}", sub_app, name=name)
