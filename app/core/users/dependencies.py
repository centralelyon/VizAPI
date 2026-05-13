from fastapi import Depends, HTTPException, status
from fastapi.security import HTTPAuthorizationCredentials, HTTPBearer
from sqlalchemy.ext.asyncio import AsyncSession

from app.core.security.jwt import verify_token
from app.core.users import models_users, repositories_users
from app.shared.db.session import get_db

bearer_scheme = HTTPBearer()


async def get_current_user(
    credentials: HTTPAuthorizationCredentials = Depends(bearer_scheme),
    db: AsyncSession = Depends(get_db),
) -> models_users.User:
    credentials_exception = HTTPException(
        status_code=status.HTTP_401_UNAUTHORIZED,
        detail="Could not validate credentials",
        headers={"WWW-Authenticate": "Bearer"},
    )
    try:
        claims = verify_token(credentials.credentials)
    except ValueError:
        raise credentials_exception

    user_id = claims.get("sub")
    if user_id is None:
        raise credentials_exception

    user = await repositories_users.get_by_id(db, int(user_id))
    if user is None:
        raise credentials_exception

    return user
