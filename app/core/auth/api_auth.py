from fastapi import Depends
from sqlalchemy.ext.asyncio import AsyncSession

from app.core.auth.services_auth import (
    exchange_code_for_user,
    get_github_authorization_url,
    upsert_user,
)
from app.core.security.jwt import create_token
from app.core.auth import schemas_auth

from app.shared.db.session import get_db
from app.types.module import CoreModule

root = "auth"
module = CoreModule(
    root=root,
    tag="Auth",
)


@module.router.get(
    "/github/login",
    response_model=schemas_auth.AuthorizationURLResponse,
    status_code=200,
)
async def github_login(
    auth_url: tuple[str, str] = Depends(get_github_authorization_url),
):
    url, state = auth_url
    return schemas_auth.AuthorizationURLResponse(
        authorization_url=url,
        state=state,
    )


@module.router.get(
    "/github/callback",
    response_model=schemas_auth.TokenResponse,
    status_code=200,
)
async def github_callback(
    github_user: dict = Depends(exchange_code_for_user),
    db: AsyncSession = Depends(get_db),
):
    user = await upsert_user(
        db=db,
        github_user=schemas_auth.GitHubUser(**github_user),
    )
    token = create_token(
        {
            "sub": str(user.id),
            "github_id": user.github_id,
            "username": user.username,
        }
    )
    return schemas_auth.TokenResponse(access_token=token)
