from pathlib import Path

from app.types.module import Module
from fastapi.responses import FileResponse, PlainTextResponse
from app.core.config import construct_settings
from app.modules.image_transform import services_image_transform

root = "image-transform"
module = Module(
    root=root,
    tag="Image Transform",
    # permissions=None,
)

_settings = construct_settings()
_base_dir = Path(_settings.DATA_PATH_MODULES)
module.add_data_dir_from(_base_dir, "image_transform", "images")
module.add_data_dir_from(_base_dir, "image_transform", "data")


@module.router.get(
    "/",
    response_class=PlainTextResponse,
    status_code=200,
)
async def root_module():
    """Root endpoint for the Image Transform module."""
    return (
        "Glacis local image processing server\n"
        "\n"
        "Use /<image-name>?width=300&height=200 or /process/<image-name>.\n"
        "Images are read from the images/ directory only.\n"
        "List local images with /images and datasets with /data.\n"
        "Dataset files are read from the data/ directory only.\n"
        "Check health with /healthz.\n"
        "Open the Vega editor with /editor.\n"
    )


@module.router.get(
    "/health",
    status_code=200,
)
async def health_check():
    """Health check endpoint."""
    return {"status": "ok"}


@module.router.get(
    "/images",
    status_code=200,
)
async def get_images():
    """List all available images."""
    return services_image_transform.get_images()


@module.router.get(
    "/data",
    status_code=200,
)
async def get_data():
    """List all available data files."""
    return services_image_transform.get_data()


@module.router.get(
    "/editor",
    response_model=FileResponse,
    status_code=200,
)
async def open_editor():
    """Open the Vega editor."""
    editor_path = Path(__file__).resolve().parents[3] / "public" / "editor.html"
    return FileResponse(editor_path, media_type="text/html")
