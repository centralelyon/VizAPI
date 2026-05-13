from contextlib import asynccontextmanager

from fastapi import FastAPI
from fastapi.middleware.cors import CORSMiddleware

from app.module import all_modules
from app.shared.db.session import Base, engine


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

app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],  # Adjust this to specify allowed origins in prod (use .env)
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)

for module in all_modules:
    app.include_router(module.router, prefix=f"/{module.root}")
