from typing import AsyncGenerator
from xml.etree import ElementTree as ET

import httpx

from app.core.utils.settings import get_settings

_DAV_NS = {"d": "DAV:"}


class OwnCloudClient:
    def __init__(self, base_url: str, username: str, password: str) -> None:
        self._base_url = base_url.rstrip("/")
        self._username = username
        self._client = httpx.AsyncClient(auth=(username, password))

    @property
    def _dav(self) -> str:
        return f"{self._base_url}/remote.php/dav/files/{self._username}"

    @property
    def _ocs(self) -> str:
        return f"{self._base_url}/ocs/v2.php"

    async def list_folder(self, path: str = "/") -> list[dict[str, str]]:
        resp = await self._client.request(
            "PROPFIND",
            f"{self._dav}{path}",
            headers={"Depth": "1"},
        )
        resp.raise_for_status()
        return _parse_propfind(resp.text)

    async def upload(self, remote_path: str, content: bytes) -> None:
        resp = await self._client.put(f"{self._dav}{remote_path}", content=content)
        resp.raise_for_status()

    async def download(self, remote_path: str) -> bytes:
        resp = await self._client.get(f"{self._dav}{remote_path}")
        resp.raise_for_status()
        return resp.content

    async def delete(self, remote_path: str) -> None:
        resp = await self._client.request("DELETE", f"{self._dav}{remote_path}")
        resp.raise_for_status()

    async def mkdir(self, remote_path: str) -> None:
        resp = await self._client.request("MKCOL", f"{self._dav}{remote_path}")
        resp.raise_for_status()

    # async def create_share(
    #     self,
    #     path: str,
    #     share_type: int = 3,
    #     permissions: int = 1,
    # ) -> dict[str, object]:
    #     """share_type: 0=user, 1=group, 3=public link — permissions: 1=read, 15=read/write"""
    #     resp = await self._client.post(
    #         f"{self._ocs}/apps/files_sharing/api/v1/shares",
    #         data={"path": path, "shareType": share_type, "permissions": permissions},
    #         headers={"OCS-APIRequest": "true"},
    #     )
    #     resp.raise_for_status()
    #     return resp.json()  # type: ignore[no-any-return]

    async def close(self) -> None:
        await self._client.aclose()


def _parse_propfind(xml_text: str) -> list[dict[str, str]]:
    root = ET.fromstring(xml_text)
    entries = []
    for response in root.findall("d:response", _DAV_NS):
        href = response.findtext("d:href", default="", namespaces=_DAV_NS)
        prop = response.find("d:propstat/d:prop", _DAV_NS)
        if prop is None:
            continue
        is_collection = prop.find("d:resourcetype/d:collection", _DAV_NS) is not None
        entries.append(
            {
                "href": href,
                "name": href.rstrip("/").rsplit("/", 1)[-1],
                "type": "folder" if is_collection else "file",
                "last_modified": prop.findtext(
                    "d:getlastmodified", default="", namespaces=_DAV_NS
                ),
                "size": prop.findtext(
                    "d:getcontentlength", default="0", namespaces=_DAV_NS
                ),
            }
        )
    return entries


async def get_cloud() -> AsyncGenerator[OwnCloudClient, None]:
    settings = get_settings()
    client = OwnCloudClient(
        base_url=settings.OWNCLOUD_DOMAIN,
        username=settings.OWNCLOUD_ADMIN_USERNAME,
        password=settings.OWNCLOUD_ADMIN_PASSWORD,
    )
    try:
        yield client
    finally:
        await client.close()
