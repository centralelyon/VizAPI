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
