from pathlib import Path
from typing import Any

from fastapi import UploadFile, File

from app.core.config import construct_settings
from app.types.module import Module
from app.modules.descript_sketches import services_descript_sketches

root = "descript-sketches"
module = Module(
    root=root,
    tag="Descript Sketches",
    # permissions=None,
)

_settings = construct_settings()
_base_dir = Path(_settings.DATA_PATH_MODULES)
module.add_data_dir_from(_base_dir, "descript_sketches", "palettes")


@module.router.get(
    "/palettes",
    response_model=list[dict[str, str]],
    status_code=200,
)
async def get_palettes():
    """Get a list of available palettes."""
    return services_descript_sketches.get_palettes()


@module.router.post(
    "/palettes",
    status_code=201,
)
async def create_palette(
    file: UploadFile = File(...),
):
    """Upload a new palette file. The file should be a JSON file containing the palette data."""
    return services_descript_sketches.upload_palette(file)


@module.router.get(
    "/palettes/{name}",
    response_model=dict[str, Any],
    status_code=200,
)
async def get_palette(
    name: str,
):
    """Get a specific palette by name."""
    return services_descript_sketches.get_palette(name)


@module.router.delete(
    "/palettes/{name}",
    status_code=204,
)
async def delete_palette(
    name: str,
):
    """Delete a specific palette by name."""
    return services_descript_sketches.delete_palette(name)
