from contextlib import asynccontextmanager

from fastapi import FastAPI
from fastapi.middleware.cors import CORSMiddleware

from app.module import all_modules
from app.shared.db.database import Base, engine


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
