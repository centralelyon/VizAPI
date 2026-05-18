from fastapi import UploadFile
from sqlalchemy.ext.asyncio import AsyncSession

from app.modules.template import (
    models_template,
    repositories_db_template,
    repositories_file_template,
    schemas_template,
)


def read_file() -> dict:
    return repositories_file_template.read()


def write_file(data: dict) -> None:
    repositories_file_template.write(data)


async def get_all_items(db: AsyncSession) -> list[models_template.TemplateItem]:
    return await repositories_db_template.get_all(db)


async def get_item(db: AsyncSession, item_id: int) -> models_template.TemplateItem | None:
    return await repositories_db_template.get_by_id(db, item_id)


async def create_item(
    db: AsyncSession,
    item: schemas_template.TemplateItemCreate,
) -> models_template.TemplateItem:
    return await repositories_db_template.create(db, item)


async def upload_file(
    db: AsyncSession,
    file: UploadFile,
) -> models_template.UploadedFile:
    data = await file.read()
    db_file = await repositories_db_template.create_file(
        db,
        original_filename=file.filename or "unnamed",
        content_type=file.content_type or "application/octet-stream",
        size=len(data),
    )
    repositories_file_template.save_upload(db_file.id, data)
    return db_file


async def download_file(
    db: AsyncSession,
    file_id: int,
) -> tuple[bytes, str, str]:
    db_file = await repositories_db_template.get_file(db, file_id)
    if db_file is None:
        raise FileNotFoundError(file_id)
    data = repositories_file_template.read_upload(file_id)
    return data, db_file.content_type, db_file.original_filename


async def list_files(
    db: AsyncSession,
) -> list[models_template.UploadedFile]:
    return await repositories_db_template.get_all_files(db)
