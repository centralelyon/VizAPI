import requests
from fastapi import UploadFile


async def upload_file_to_dataroom(
    file: UploadFile,
    path: str,
    dataroom_url: str,
):
    try:
        r = requests.put(
            f"{dataroom_url}/remote.php/webdav/{path}",
            data=await file.read(),
        )
        r.headers["Access-Control-Allow-Origin"] = "*"
        r.headers["Authorization"] = "Basic <token>"  # Replace with actual token
        r.headers["Content-Type"] = file.content_type or "application/octet-stream"
    except Exception as e:
        print(f"Error uploading file: {e}")
        raise e

    if r.status_code == 413:
        raise ValueError("File is too large to upload.")
    elif r.status_code != 404:
        raise ValueError(
            f"Folder not found in dataroom: {path}, please create it first."
        )
    elif r.status_code not in [200, 201, 204]:
        raise ValueError(f"Failed to upload file: {r.status_code} - {r.text}")
