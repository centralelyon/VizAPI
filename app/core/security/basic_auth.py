import httpx
from fastapi import Depends, HTTPException, status
from fastapi.security import HTTPBasic, HTTPBasicCredentials

from app.core.utils.settings import get_settings

_scheme = HTTPBasic(auto_error=False)


async def require_basic_auth(
    credentials: HTTPBasicCredentials | None = Depends(_scheme),
) -> HTTPBasicCredentials:
    if credentials is None:
        raise HTTPException(
            status_code=status.HTTP_401_UNAUTHORIZED,
            detail="Basic auth required",
            headers={"WWW-Authenticate": "Basic"},
        )
    settings = get_settings()
    url = f"{settings.OWNCLOUD_DOMAIN.rstrip('/')}/ocs/v1.php/cloud/user"
    async with httpx.AsyncClient() as client:
        resp = await client.get(
            url,
            auth=(credentials.username, credentials.password),
            params={"format": "json"},
        )
    if resp.status_code != 200:
        raise HTTPException(
            status_code=status.HTTP_401_UNAUTHORIZED,
            detail="Invalid OwnCloud credentials",
            headers={"WWW-Authenticate": "Basic"},
        )
    return credentials
