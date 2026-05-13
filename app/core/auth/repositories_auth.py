from sqlalchemy import select
from sqlalchemy.ext.asyncio import AsyncSession

from app.core.auth import schemas_auth
from app.core.users import models_users


async def add_token(
    db: AsyncSession,
    token: schemas_auth.TokenCreate
):
    db_token = models_auth.Token(**token.dict())
    db.add(db_token)
    await db.commit()
    await db.refresh(db_token)
    return db_token