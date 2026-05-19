from contextlib import asynccontextmanager
from pathlib import Path

from fastapi import FastAPI
from fastapi.middleware.cors import CORSMiddleware

from app.module import all_modules
from app.core.utils.settings import construct_settings
from app.shared.db.database import Base, engine


def _ensure_module_data_dirs() -> None:
    settings = construct_settings()
    base_dir = Path(settings.DATA_PATH_MODULES)
    required_dirs = [
        base_dir / "aquanote" / "courses_demo",
        base_dir / "descript_sketches" / "palettes",
        base_dir / "template" / "files",
    ]
    for directory in required_dirs:
        directory.mkdir(parents=True, exist_ok=True)


@asynccontextmanager
async def lifespan(app: FastAPI):
    async with engine.begin() as conn:
        await conn.run_sync(Base.metadata.create_all)
    yield
    await engine.dispose()


app = FastAPI(
    title="VizAPI",
    version="0.0.1",
    lifespan=lifespan,
)


_ensure_module_data_dirs()

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
