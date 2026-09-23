from datetime import datetime, timezone
import json
import os
from pathlib import Path
import re
import tempfile

from fastapi import APIRouter, HTTPException
from fastapi.responses import FileResponse


router = APIRouter()


def data_root():
    explicit = os.environ.get("TRANSITMAP_DATA_DIR")

    if explicit:
        return Path(explicit).resolve()

    modules_root = Path(
        os.environ.get(
            "DATA_PATH_MODULES",
            "./data/modules/",
        )
    ).resolve()

    return (modules_root / "decaligne_code").resolve()


def city_path(city, suffix):
    if not re.fullmatch(
        r"[a-zA-Z0-9_-]+",
        city,
    ):
        raise HTTPException(
            404,
            "Unknown city",
        )

    root = data_root() / "cities"

    path = (
        root
        / city
        / (city + suffix)
    ).resolve()

    if (
        not path.is_relative_to(root.resolve())
        or not path.is_file()
    ):
        raise HTTPException(
            404,
            "City data not found",
        )

    return path


@router.get("/catalog")
def catalog():
    root = data_root() / "cities"

    if not root.is_dir():
        raise HTTPException(
            503,
            "City catalog is not installed "
            "in the module data directory",
        )

    result = []

    for path in sorted(root.iterdir()):
        if re.fullmatch(
            r"[a-zA-Z0-9_-]+",
            path.name,
        ):
            try:
                city_path(
                    path.name,
                    ".json",
                )
                result.append(path.name)

            except HTTPException:
                pass

    return {
        "cities": result,
    }


@router.get("/catalog/{city}/network", include_in_schema=False)
def network(city: str):
    return FileResponse(
        city_path(
            city,
            ".json",
        ),
        media_type="application/json",
    )


@router.get("/catalog/{city}/obstacles", include_in_schema=False)
def obstacles(city: str):
    return FileResponse(
        city_path(
            city,
            ".geojson",
        ),
        media_type="application/geo+json",
    )


def save_upload(
    network,
    body,
    session_id,
    revision,
    evaluation_id,
):
    """
    Save the final edited network directly into evaluation/.

    Example:

        evaluation/000001_output.json

    No uploads/ directory is used.
    """

    canvas = body.get("canvas")
    participant = body.get("participantId")

    if (
        not isinstance(canvas, dict)
        or canvas.get("side")
        not in ("left", "right")
        or canvas.get("mode")
        not in ("geometry", "styled")
    ):
        raise HTTPException(
            422,
            "Canvas side and mode are required",
        )

    if (
        not isinstance(participant, str)
        or not participant
    ):
        raise HTTPException(
            422,
            "Invalid participant ID",
        )

    if (
        not isinstance(evaluation_id, str)
        or not re.fullmatch(
            r"\d{6}",
            evaluation_id,
        )
    ):
        raise HTTPException(
            422,
            "Invalid evaluation ID",
        )

    css = canvas.get("styleCss")

    if (
        canvas["mode"] == "styled"
        and not isinstance(css, str)
    ):
        raise HTTPException(
            422,
            "Styled canvas requires styleCss",
        )

    if (
        len(
            json.dumps(
                canvas,
                allow_nan=False,
            ).encode()
        )
        > 2 * 1024 * 1024
    ):
        raise HTTPException(
            413,
            "Canvas settings are too large",
        )

    # Keep the saved file directly importable
    # by TransitMapWeb.
    document = dict(network)

    document.pop(
        "transitMapStyle",
        None,
    )

    if canvas["mode"] == "styled":
        document["transitMapStyle"] = {
            "version": 1,
            "css": css,
        }

    created = (
        datetime.now(timezone.utc)
        .isoformat()
    )

    document["transitMapCanvas"] = canvas

    document["transitMapSubmission"] = {
        "version": 1,
        "evaluationId": evaluation_id,
        "participantId": participant,
        "createdAt": created,
        "sessionId": session_id,
        "revision": revision,
    }

    contents = json.dumps(
        document,
        ensure_ascii=False,
        allow_nan=False,
        indent=2,
    )

    directory = (
        data_root()
        / "evaluation"
    )

    directory.mkdir(
        parents=True,
        exist_ok=True,
    )

    destination = (
        directory
        / f"{evaluation_id}_output.json"
    )

    temporary = None

    try:
        with tempfile.NamedTemporaryFile(
            mode="w",
            encoding="utf-8",
            dir=directory,
            suffix=".tmp",
            delete=False,
        ) as output:
            temporary = Path(
                output.name
            )

            output.write(contents)
            output.flush()
            os.fsync(
                output.fileno()
            )

        temporary.replace(
            destination
        )

    except OSError as exc:
        raise HTTPException(
            503,
            "Could not store evaluation output. "
            "Please retry.",
        ) from exc

    finally:
        if (
            temporary is not None
            and temporary.exists()
        ):
            temporary.unlink(
                missing_ok=True
            )

    return {
        "uploaded": True,
        "uploadId": evaluation_id,
        "evaluationId": evaluation_id,
        "createdAt": created,
        "sessionId": session_id,
        "revision": revision,
    }