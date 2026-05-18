from sqlalchemy import select
from sqlalchemy.ext.asyncio import AsyncSession

from app.modules.template import models_template, schemas_template


async def get_all(
    db: AsyncSession,
) -> list[models_template.TemplateItem]:
    result = await db.execute(select(models_template.TemplateItem))
    return list(result.scalars().all())


async def get_by_id(
    db: AsyncSession,
    item_id: int,
) -> models_template.TemplateItem | None:
    result = await db.execute(
        select(models_template.TemplateItem).where(
            models_template.TemplateItem.id == item_id
        )
    )
    return result.scalar_one_or_none()


async def create(
    db: AsyncSession,
    item: schemas_template.TemplateItemCreate,
) -> models_template.TemplateItem:

    db_item = models_template.TemplateItem(name=item.name, value=item.value)
    db.add(db_item)
    await db.commit()
    await db.refresh(db_item)
    return db_item


async def create_file(
    db: AsyncSession,
    original_filename: str,
    content_type: str,
    size: int,
) -> models_template.UploadedFile:
    db_file = models_template.UploadedFile(
        original_filename=original_filename,
        content_type=content_type,
        size=size,
    )
    db.add(db_file)
    await db.commit()
    await db.refresh(db_file)
    return db_file


async def get_file(
    db: AsyncSession,
    file_id: int,
) -> models_template.UploadedFile | None:
    result = await db.execute(
        select(models_template.UploadedFile).where(
            models_template.UploadedFile.id == file_id
        )
    )
    return result.scalar_one_or_none()


async def get_all_files(
    db: AsyncSession,
) -> list[models_template.UploadedFile]:
    result = await db.execute(select(models_template.UploadedFile))
    return list(result.scalars().all())
