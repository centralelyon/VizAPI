from authlib.integrations.httpx_client import AsyncOAuth2Client
from fastapi import Depends
from sqlalchemy.ext.asyncio import AsyncSession

from app.core.auth import repositories_auth
from app.core.config import Settings
from app.core.users import models_users, repositories_users
from app.core.utils.settings import get_settings
from app.core.auth import schemas_auth

GITHUB_AUTHORIZE_URL = "https://github.com/login/oauth/authorize"
GITHUB_TOKEN_URL = "https://github.com/login/oauth/access_token"
GITHUB_USER_URL = "https://api.github.com/user"


def get_github_authorization_url(
    settings: Settings = Depends(get_settings),
) -> tuple[str, str]:
    client = AsyncOAuth2Client(
        client_id=settings.GITHUB_CLIENT_ID,
        client_secret=settings.GITHUB_CLIENT_SECRET,
        redirect_uri=settings.GITHUB_REDIRECT_URI,
        scope=settings.GITHUB_SCOPES,
    )
    url, state = client.create_authorization_url(GITHUB_AUTHORIZE_URL)
    return url, state


async def exchange_code_for_user(
    code: str,
    settings: Settings = Depends(get_settings),
) -> dict:
    client = AsyncOAuth2Client(
        client_id=settings.GITHUB_CLIENT_ID,
        client_secret=settings.GITHUB_CLIENT_SECRET,
        redirect_uri=settings.GITHUB_REDIRECT_URI,
    )
    try:
        await client.fetch_token(
            url=GITHUB_TOKEN_URL,
            code=code,
            headers={"Accept": "application/json"},
        )
        response = await client.request(
            "GET",
            GITHUB_USER_URL,
            headers={"Accept": "application/vnd.github+json"},
        )
        response.raise_for_status()
        return response.json()
    except Exception as exc:
        raise ValueError(f"GitHub authentication failed: {exc}") from exc


async def upsert_user(
    db: AsyncSession,
    github_user: schemas_auth.GitHubUser,
) -> models_users.User:
    user = await repositories_users.get_by_github_id(db, github_user.id)
    if user is None:
        return await repositories_users.create(db, github_user)
    return await repositories_users.update(db, user, github_user)
