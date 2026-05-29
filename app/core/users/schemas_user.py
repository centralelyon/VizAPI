from datetime import datetime

from pydantic import BaseModel


class User(BaseModel):
    id: int
    github_id: int
    username: str
    avatar_url: str | None
    created_at: datetime

    model_config = {"from_attributes": True}
