import json
from pathlib import Path
from urllib.parse import parse_qs

from fastapi import HTTPException, Request
from fastapi.responses import HTMLResponse, PlainTextResponse, Response

from PIL import UnidentifiedImageError

from app.core.config import construct_settings
from app.modules.image_transform import services_image_transform
from app.types.module import Module


root = "image-transform"
module = Module(
    root=root,
    tag="Image Transform",
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
    return (
        "Glacis local image processing server\n"
        "\n"
        "Use /<image-name>?width=300&height=200 or /process/<image-name>.\n"
        "Images are read from the images/ directory only.\n"
        "List local images with /images and datasets with /data.\n"
        "Dataset files are read from the data/ directory only.\n"
        "Check health with /health.\n"
        "Open the Vega editor with /editor.\n"
    )


@module.router.get(
    "/health",
    status_code=200,
)
async def health_check():
    return {"status": "ok"}


@module.router.get(
    "/images",
    status_code=200,
)
async def get_images():
    return services_image_transform.get_images()


@module.router.get(
    "/data",
    status_code=200,
)
async def get_data():
    return services_image_transform.get_data()


@module.router.get(
    "/editor",
    response_class=HTMLResponse,
    status_code=200,
)
async def open_editor(request: Request):
    server_origin = str(request.base_url).rstrip("/") + f"/{root}"
    editor_path = Path(__file__).resolve().parents[3] / "public" / "editor.html"
    html = editor_path.read_text(encoding="utf-8").replace(
        "__SERVER_ORIGIN_JSON__", json.dumps(server_origin)
    )
    return HTMLResponse(html, headers={"Cache-Control": "no-store"})


@module.router.get(
    "/data/{filename:path}",
    status_code=200,
)
async def get_data_file(filename: str):
    payload, content_type = services_image_transform.get_data_file(filename)
    if payload is None:
        raise HTTPException(status_code=404, detail="Dataset not found.")
    return Response(
        payload,
        media_type=content_type,
        headers={"Cache-Control": "no-store"},
    )


async def _serve_image(image_name: str, request: Request) -> Response:
    query = parse_qs(str(request.url.query), keep_blank_values=True)
    try:
        payload, content_type = services_image_transform.process_image(
            image_name, query
        )
    except ValueError as exc:
        raise HTTPException(status_code=400, detail=str(exc))
    except OSError, UnidentifiedImageError:
        raise HTTPException(status_code=415, detail="Unsupported image file.")
    if payload is None:
        raise HTTPException(status_code=404, detail="Image not found.")
    return Response(
        payload,
        media_type=content_type,
        headers={"Cache-Control": "public, max-age=86400"},
    )


@module.router.get(
    "/process/{image_name:path}",
    status_code=200,
)
async def process_image(image_name: str, request: Request):
    return await _serve_image(image_name, request)


@module.router.get(
    "/{image_name:path}",
    status_code=200,
)
async def serve_image(image_name: str, request: Request):
    return await _serve_image(image_name, request)
