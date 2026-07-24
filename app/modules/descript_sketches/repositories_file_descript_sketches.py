from pathlib import Path

from app.core.config import construct_settings


_settings = construct_settings()
BASE_DIR = Path(_settings.DATA_PATH_MODULES) / "descript_sketches" / "palettes"
_UPLOAD_DIR = Path(_settings.DATA_PATH_MODULES) / "descript_sketches" / "palettes"

IMAGES_DIR = Path(_settings.DATA_PATH_MODULES) / "descript_sketches" / "images"
_IMAGE_SUFFIXES = {".png", ".jpg", ".jpeg", ".gif", ".webp", ".svg", ".bmp"}


def list_palettes() -> list[dict[str, str]]:
    res: list[dict[str, str]] = []
    if not BASE_DIR.exists():
        return res
    for entry in BASE_DIR.iterdir():
        if entry.is_file() and entry.suffix == ".json":
            res.append({"name": entry.name.removesuffix(".json"), "type": "JSON"})
    return res


def save_palette(filename: str, data: bytes) -> None:
    _UPLOAD_DIR.mkdir(parents=True, exist_ok=True)
    (_UPLOAD_DIR / str(filename)).write_bytes(data)


def read_palette(name: str) -> bytes:
    path = _UPLOAD_DIR / f"{name}.json"
    if not path.exists():
        raise FileNotFoundError(name)
    return path.read_bytes()


def delete_palette(name: str) -> None:
    path = _UPLOAD_DIR / f"{name}.json"
    if not path.exists():
        raise FileNotFoundError(name)
    path.unlink(missing_ok=True)


def list_images() -> list[dict[str, str]]:
    res: list[dict[str, str]] = []
    if not IMAGES_DIR.exists():
        return res
    for entry in IMAGES_DIR.iterdir():
        if entry.is_file() and entry.suffix.lower() in _IMAGE_SUFFIXES:
            res.append(
                {"name": entry.stem, "type": entry.suffix.removeprefix(".").upper()}
            )
    return res


def save_image(filename: str, data: bytes) -> None:
    IMAGES_DIR.mkdir(parents=True, exist_ok=True)
    (IMAGES_DIR / str(filename)).write_bytes(data)


def find_image(name: str) -> Path:
    if not IMAGES_DIR.exists():
        raise FileNotFoundError(name)
    for entry in IMAGES_DIR.iterdir():
        if (
            entry.is_file()
            and entry.stem == name
            and entry.suffix.lower() in _IMAGE_SUFFIXES
        ):
            return entry
    raise FileNotFoundError(name)
