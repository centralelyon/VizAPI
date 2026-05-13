from pydantic import BaseModel


class GitHubUser(BaseModel):
    id: int
    name: str
    type: str
    avatar_url: str


class TokenResponse(BaseModel):
    access_token: str
    token_type: str = "bearer"


class AuthorizationURLResponse(BaseModel):
    authorization_url: str
    state: str
