from fastapi import APIRouter
# from sqlalchemy.ext.asyncio import AsyncSession


class CoreModule:
    def __init__(
        self,
        root: str,
        tag: str,
        router: APIRouter | None = None,
        # permissions: type[ModulePermissions] | None = None,
    ):
        """
        Initialize a new Module object.
        :param root: the root of the module, used by Titan
        :param tag: the tag of the module, used by FastAPI
        :param router: an optional custom APIRouter
        :param permissions: enum declaring permissions strings used by module
        """
        self.root = root
        self.router = router or APIRouter(tags=[tag])
        # self.permissions = permissions


class Module(CoreModule):
    def __init__(
        self,
        root: str,
        tag: str,
        # default_allowed_groups_ids: list[GroupType] | None = None,
        # default_allowed_account_types: list[AccountType] | None = None,
        router: APIRouter | None = None,
        # permissions: type[ModulePermissions] | None = None,
    ):
        """
        Initialize a new Module object.
        :param root: the root of the module, used by Titan
        :param tag: the tag of the module, used by FastAPI
        :param default_allowed_groups_ids: list of groups that should be able to see the module by default
        :param default_allowed_account_types: list of account_types that should be able to see the module by default
        :param router: an optional custom APIRouter
        :param permissions: enum declaring permissions strings used by module
        """
        self.root = root
        # self.default_allowed_groups_ids = default_allowed_groups_ids
        # self.default_allowed_account_types = default_allowed_account_types
        self.router = router or APIRouter(tags=[tag])
        # self.permissions = permissions
