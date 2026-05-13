from sqlalchemy import select
from sqlalchemy.ext.asyncio import AsyncSession

from app.core.auth import schemas_auth
from app.core.users import models_users


async def get_by_id(db: AsyncSession, user_id: int) -> models_users.User | None:
    result = await db.execute(
        select(models_users.User).where(models_users.User.id == user_id)
    )
    return result.scalar_one_or_none()


async def get_by_github_id(
    db: AsyncSession,
    github_id: int,
) -> models_users.User | None:
    result = await db.execute(
        select(models_users.User).where(models_users.User.github_id == github_id)
    )
    return result.scalar_one_or_none()


async def create(
    db: AsyncSession, github_user: schemas_auth.GitHubUser
) -> models_users.User:
    user = models_users.User(
        github_id=github_user.id,
        username=github_user.name,
        avatar_url=github_user.avatar_url,
    )
    db.add(user)
    await db.commit()
    await db.refresh(user)
    return user


async def update(
    db: AsyncSession,
    user: models_users.User,
    github_user: schemas_auth.GitHubUser,
) -> models_users.User:
    user.username = github_user.name
    user.avatar_url = github_user.avatar_url
    await db.commit()
    await db.refresh(user)
    return user
