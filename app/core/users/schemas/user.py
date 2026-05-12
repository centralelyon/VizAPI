from datetime import datetime

from pydantic import BaseModel


class UserPublic(BaseModel):
    id: int
    github_id: int
    username: str
    email: str | None
    avatar_url: str | None
    created_at: datetime

    model_config = {"from_attributes": True}


class TokenResponse(BaseModel):
    access_token: str
    token_type: str = "bearer"
