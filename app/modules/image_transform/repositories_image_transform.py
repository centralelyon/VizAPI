from __future__ import annotations

from pathlib import Path
from urllib.parse import unquote

from app.core.config import construct_settings


_settings = construct_settings()
BASE_DIR = (Path(_settings.DATA_PATH_MODULES) / "image_transform").resolve()
IMAGES_DIR = BASE_DIR / "images"
DATA_DIR = BASE_DIR / "data"

EXTENSION_FORMATS = {
    ".jpg": "JPEG",
    ".jpeg": "JPEG",
    ".png": "PNG",
    ".webp": "WEBP",
}
INPUT_EXTENSIONS = set(EXTENSION_FORMATS) | {".gif", ".bmp", ".tif", ".tiff"}
IMAGE_PATH_PREFIXES = ("process/", "image/", "img/", "images/")
DATA_CONTENT_TYPES = {
    ".csv": "text/csv; charset=utf-8",
    ".json": "application/json; charset=utf-8",
    ".tsv": "text/tab-separated-values; charset=utf-8",
}


def list_images() -> list[str]:
    if not IMAGES_DIR.is_dir():
        return []
    return [
        path.relative_to(IMAGES_DIR).as_posix()
        for path in sorted(IMAGES_DIR.rglob("*"))
        if path.is_file() and path.suffix.lower() in INPUT_EXTENSIONS
    ]


def list_datasets() -> list[str]:
    if not DATA_DIR.is_dir():
        return []
    return [
        path.relative_to(DATA_DIR).as_posix()
        for path in sorted(DATA_DIR.rglob("*"))
        if path.is_file() and path.suffix.lower() in DATA_CONTENT_TYPES
    ]


def resolve_request_image_path(request_path: str) -> Path | None:
    rel_path = unquote(request_path).lstrip("/")
    for prefix in IMAGE_PATH_PREFIXES:
        if rel_path.startswith(prefix):
            rel_path = rel_path[len(prefix):]
            break

    if not rel_path or rel_path.endswith("/"):
        return None

    candidate = safe_image_candidate(rel_path)
    if candidate is not None and candidate.is_file():
        return candidate

    if Path(rel_path).suffix:
        return candidate

    for suffix in EXTENSION_FORMATS:
        candidate = safe_image_candidate(f"{rel_path}{suffix}")
        if candidate is not None and candidate.is_file():
            return candidate

    return safe_image_candidate(rel_path)


def resolve_request_data_path(request_path: str) -> Path | None:
    rel_path = unquote(request_path).lstrip("/")
    if rel_path.startswith("data/"):
        rel_path = rel_path[len("data/"):]

    if not rel_path or rel_path.endswith("/"):
        return None

    if Path(rel_path).suffix.lower() not in DATA_CONTENT_TYPES:
        return None

    return safe_data_candidate(rel_path)


def safe_image_candidate(rel_path: str) -> Path | None:
    candidate = (IMAGES_DIR / rel_path).resolve()
    try:
        candidate.relative_to(IMAGES_DIR)
    except ValueError:
        return None
    return candidate


def safe_data_candidate(rel_path: str) -> Path | None:
    candidate = (DATA_DIR / rel_path).resolve()
    try:
        candidate.relative_to(DATA_DIR)
    except ValueError:
        return None
    return candidate


def data_content_type(path: Path) -> str:
    return DATA_CONTENT_TYPES[path.suffix.lower()]
