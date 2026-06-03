from app.shared.db.owncloud import OwnCloudClient


async def list_folder(cloud: OwnCloudClient, path: str) -> list[dict[str, str]]:
    return await cloud.list_folder(path)


async def upload(cloud: OwnCloudClient, path: str, content: bytes) -> None:
    await cloud.upload(path, content)


async def download(cloud: OwnCloudClient, path: str) -> bytes:
    return await cloud.download(path)


async def delete(cloud: OwnCloudClient, path: str) -> None:
    await cloud.delete(path)


# async def create_share(
#     cloud: OwnCloudClient, path: str
# ) -> dict[str, object]:
#     return await cloud.create_share(path)
