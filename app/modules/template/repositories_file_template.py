import json
from pathlib import Path

from app.core.config import construct_settings

_DATA_FILE = Path(__file__).parent / "data" / "sample.json"

_settings = construct_settings()
_UPLOAD_DIR = Path(_settings.DATA_PATH_MODULES) / "template" / "files"


def read() -> dict:
    if not _DATA_FILE.exists():
        return {}
    with _DATA_FILE.open("r", encoding="utf-8") as f:
        return json.load(f)


def write(data: dict) -> None:
    _DATA_FILE.parent.mkdir(parents=True, exist_ok=True)
    with _DATA_FILE.open("w", encoding="utf-8") as f:
        json.dump(data, f, indent=2)


def save_upload(file_id: int, data: bytes) -> None:
    _UPLOAD_DIR.mkdir(parents=True, exist_ok=True)
    (_UPLOAD_DIR / str(file_id)).write_bytes(data)


def read_upload(file_id: int) -> bytes:
    path = _UPLOAD_DIR / str(file_id)
    if not path.exists():
        raise FileNotFoundError(file_id)
    return path.read_bytes()


def delete_upload(file_id: int) -> None:
    (_UPLOAD_DIR / str(file_id)).unlink(missing_ok=True)
