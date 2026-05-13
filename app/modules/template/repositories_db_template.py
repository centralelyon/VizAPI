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
