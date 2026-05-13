from fastapi import Depends

from app.types.module import CoreModule
from app.core.users.dependencies import get_current_user
from app.core.users import schemas_user

root = "users"
module = CoreModule(
    root="users",
    tag="Users",
)


@module.router.get(
    "/me",
    response_model=schemas_user.User,
    status_code=200,
)
async def get_me(
    current_user: schemas_user.User = Depends(get_current_user),
):
    return current_user
