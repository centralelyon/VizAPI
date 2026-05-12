from sqlalchemy import select
from sqlalchemy.ext.asyncio import AsyncSession

from app.core.users.models import User


async def get_by_github_id(db: AsyncSession, github_id: int) -> User | None:
    result = await db.execute(select(User).where(User.github_id == github_id))
    return result.scalar_one_or_none()


async def create(db: AsyncSession, github_user: dict) -> User:
    user = User(
        github_id=github_user["id"],
        username=github_user.get("login", ""),
        email=github_user.get("email"),
        avatar_url=github_user.get("avatar_url"),
    )
    db.add(user)
    await db.commit()
    await db.refresh(user)
    return user


async def update(db: AsyncSession, user: User, github_user: dict) -> User:
    user.username = github_user.get("login", user.username)
    user.email = github_user.get("email", user.email)
    user.avatar_url = github_user.get("avatar_url", user.avatar_url)
    await db.commit()
    await db.refresh(user)
    return user
