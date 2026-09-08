from fastapi import HTTPException

from app.types.module import Module
from app.modules.decaligne_code.loom_runner import run_loom


root = "decaligne-code"

module = Module(
    root=root,
    tag="Decaligne",
)


@module.router.get("/ping")
def ping():
    return {"status": "ok"}


@module.router.post("/loom")
def loom(graph: dict):
    try:
        return run_loom(graph)
    except RuntimeError as exc:
        raise HTTPException(
            status_code=500,
            detail=str(exc),
        ) from exc
