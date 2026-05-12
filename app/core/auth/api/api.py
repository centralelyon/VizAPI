from fastapi import Depends, HTTPException, Query, status
from sqlalchemy.ext.asyncio import AsyncSession

from app.core.auth.service.github import (
    exchange_code_for_user,
    get_github_authorization_url,
    upsert_user,
)
from app.core.security.jwt import create_token
from app.core.users.dependencies import get_current_user
from app.core.users.models import User
from app.core.users.schemas import TokenResponse, UserPublic
from app.shared.db.session import get_db
from app.types.module import CoreModule

module = CoreModule(root="auth", tag="Auth")


@module.router.get("/github/login")
async def github_login(
    auth_url: tuple[str, str] = Depends(get_github_authorization_url),
):
    url, state = auth_url
    return {"authorization_url": url, "state": state}


@module.router.get(
    "/github/callback",
    response_model=TokenResponse,
)
async def github_callback(
    github_user: dict = Depends(exchange_code_for_user),
    db: AsyncSession = Depends(get_db),
):
    user = await upsert_user(db, github_user)
    token = create_token(
        {
            "sub": str(user.id),
            "github_id": user.github_id,
            "username": user.username,
        }
    )
    return TokenResponse(access_token=token)


@module.router.get("/me", response_model=UserPublic)
async def get_me(current_user: User = Depends(get_current_user)):
    return current_user
