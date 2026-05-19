from pathlib import Path

from fastapi import Depends, File, HTTPException, UploadFile, status
from fastapi.responses import Response
from sqlalchemy.ext.asyncio import AsyncSession

from app.core.config import construct_settings
from app.modules.template import schemas_template, service_template
from app.shared.db.database import get_db
from app.types.module import Module

root = "template"  # Set the root path for the module every routes will be prefixed with this
module = Module(
    root=root,
    tag="Template",
)

_settings = construct_settings()
_base_dir = Path(_settings.DATA_PATH_MODULES)
module.add_data_dir_from(_base_dir, root, "files")


# --- File-based routes ---


@module.router.get(
    "/file/read",
    status_code=200,
)
def file_read() -> dict:
    return service_template.read_file()


@module.router.post(
    "/file/write",
    status_code=201,
)
def file_write(data: dict) -> dict:
    service_template.write_file(data)
    return {"status": "written"}


# --- DB-based routes ---


@module.router.get("/db/items", response_model=list[schemas_template.TemplateItemRead])
async def db_list(db: AsyncSession = Depends(get_db)):
    return await service_template.get_all_items(db)


@module.router.post(
    "/db/items",
    response_model=schemas_template.TemplateItemRead,
    status_code=201,
)
async def db_create(
    item: schemas_template.TemplateItemCreate,
    db: AsyncSession = Depends(get_db),
):
    return await service_template.create_item(db, item)


@module.router.get(
    "/db/items/{item_id}",
    response_model=schemas_template.TemplateItemRead,
)
async def db_get(item_id: int, db: AsyncSession = Depends(get_db)):
    item = await service_template.get_item(db, item_id)
    if item is None:
        raise HTTPException(
            status_code=status.HTTP_404_NOT_FOUND, detail="Item not found"
        )
    return item


# --- File upload/download routes ---


@module.router.post(
    "/files",
    response_model=schemas_template.UploadedFileRead,
    status_code=201,
)
async def upload_file(
    file: UploadFile = File(...),
    db: AsyncSession = Depends(get_db),
):
    return await service_template.upload_file(db, file)


@module.router.get("/files", response_model=list[schemas_template.UploadedFileRead])
async def list_files(db: AsyncSession = Depends(get_db)):
    return await service_template.list_files(db)


@module.router.get("/files/{file_id}")
async def download_file(file_id: int, db: AsyncSession = Depends(get_db)):
    try:
        data, content_type, filename = await service_template.download_file(db, file_id)
    except FileNotFoundError:
        raise HTTPException(
            status_code=status.HTTP_404_NOT_FOUND, detail="File not found"
        )
    return Response(
        content=data,
        media_type=content_type,
        headers={"Content-Disposition": f'attachment; filename="{filename}"'},
    )
