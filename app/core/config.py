from pydantic_settings import BaseSettings, SettingsConfigDict


class Settings(BaseSettings):
    model_config = SettingsConfigDict(
        env_file=".env",
        env_file_encoding="utf-8",
        case_sensitive=True,
        extra="forbid",
    )

    def __init__(self, _env_file: str = ".env"):
        super().__init__(_env_file=_env_file)

    SERVER_BASE_URL: str = ""

    GITHUB_CLIENT_ID: str
    GITHUB_CLIENT_SECRET: str
    GITHUB_REDIRECT_URI: str
    GITHUB_SCOPES: str

    SECRET_KEY: str
    ACCESS_TOKEN_EXPIRE_MINUTES: int = 60

    DATABASE_URL: str = "sqlite+aiosqlite:///./app.db"
    DATA_PATH_MODULES: str = "./data/modules/"

    RATE_LIMIT_REQUESTS: int = 60
    RATE_LIMIT_WINDOW_SECONDS: int = 60


def construct_settings() -> Settings:
    """Returns the production settings"""
    return Settings(_env_file=".env")
