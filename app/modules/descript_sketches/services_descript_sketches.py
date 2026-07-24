import json
from pathlib import Path
from typing import Any

from fastapi import HTTPException, UploadFile, File

from app.modules.descript_sketches import repositories_file_descript_sketches
from app.modules.descript_sketches.repositories_file_descript_sketches import (
    list_palettes,
    list_images,
)


def get_palettes() -> list[dict[str, str]]:
    return list_palettes()


def get_palette(name: str) -> dict[str, Any]:
    try:
        data = repositories_file_descript_sketches.read_palette(name)
    except FileNotFoundError as exc:
        raise HTTPException(
            status_code=404, detail=f"Palette '{name}' not found"
        ) from exc
    return json.loads(data)


def upload_palette(file: UploadFile = File(...)) -> None:
    data = file.file.read()

    if not file.filename:
        raise ValueError("Filename is required")

    return repositories_file_descript_sketches.save_palette(file.filename, data)


def delete_palette(name: str) -> None:
    try:
        return repositories_file_descript_sketches.delete_palette(name)
    except FileNotFoundError as exc:
        raise HTTPException(
            status_code=404, detail=f"Palette '{name}' not found"
        ) from exc


def get_images() -> list[dict[str, str]]:
    return list_images()


def get_image(name: str) -> Path:
    try:
        return repositories_file_descript_sketches.find_image(name)
    except FileNotFoundError as exc:
        raise HTTPException(
            status_code=404, detail=f"Image '{name}' not found"
        ) from exc


def upload_image(file: UploadFile = File(...)) -> None:
    data = file.file.read()

    if not file.filename:
        raise ValueError("Filename is required")

    return repositories_file_descript_sketches.save_image(file.filename, data)
