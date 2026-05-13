from pathlib import Path
from app.core.config import construct_settings

_settings = construct_settings()
BASE_DIR = Path(_settings.DATA_PATH_MODULES) / "aquanote" / "courses_demo"


def list_compets() -> list[dict[str, str]]:
    res: list[dict[str, str]] = []
    if not BASE_DIR.exists():
        return res
    for entry in BASE_DIR.iterdir():
        if entry.is_dir():
            res.append({"name": entry.name, "type": "directory"})
    return res


def list_runs(compet: str) -> list[dict[str, str]]:
    res: list[dict[str, str]] = []
    base_dir = BASE_DIR / compet
    if not base_dir.exists():
        return res
    for entry in base_dir.iterdir():
        if entry.is_dir():
            res.append({"name": entry.name, "type": "directory"})
    return res


def list_datas(compet: str, run: str) -> list[dict[str, str]]:
    res: list[dict[str, str]] = []
    base_dir = BASE_DIR / compet / run
    if not base_dir.exists():
        return res
    for entry in base_dir.iterdir():
        if entry.is_file():
            res.append({"name": entry.name, "type": "file"})
    return res


def list_quality(compet: str, run: str) -> list[dict[str, str]]:
    res: list[dict[str, str]] = []
    base_dir = BASE_DIR / compet / run
    if not base_dir.exists():
        return res
    for entry in base_dir.iterdir():
        if entry.is_file():
            res.append({"name": entry.name, "type": "file"})
    return res
