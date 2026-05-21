from pathlib import Path

from app.core.config import construct_settings


_settings = construct_settings()
BASE_DIR = Path(_settings.DATA_PATH_MODULES) / "image_transform"
IMAGES_DIR = BASE_DIR / "images"
DATA_DIR = BASE_DIR / "data"
# _UPLOAD_DIR = Path(_settings.DATA_PATH_MODULES) / "image_transform" / "palettes"
EXTENSION_FORMATS = {
    ".jpg": "JPEG",
    ".jpeg": "JPEG",
    ".png": "PNG",
    ".webp": "WEBP",
}

DATA_EXTENSIONS = [".csv", ".json"]


def list_images() -> list[dict[str, str]]:
    res: list[dict[str, str]] = []
    if not IMAGES_DIR.exists():
        return res
    for entry in IMAGES_DIR.iterdir():
        if entry.is_file() and entry.suffix in EXTENSION_FORMATS:
            res.append({"name": entry.name, "type": EXTENSION_FORMATS[entry.suffix]})
    return res


def list_datasets() -> list[dict[str, str]]:
    res: list[dict[str, str]] = []
    if not DATA_DIR.exists():
        return res
    for entry in DATA_DIR.iterdir():
        if entry.is_file() and entry.suffix in DATA_EXTENSIONS:
            res.append(
                {"name": entry.name, "type": entry.suffix.upper().removeprefix(".")}
            )
    return res
