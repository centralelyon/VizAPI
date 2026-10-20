from datetime import datetime, timezone
import json
import os
from pathlib import Path
import re
import tempfile
import uuid
import hashlib
import shutil
from functools import lru_cache

from fastapi import APIRouter, HTTPException
from fastapi.responses import FileResponse


router = APIRouter()


def versions_path(participant):
    if not isinstance(participant, str) or not re.fullmatch(r"[A-Za-z0-9_-]{1,80}", participant):
        raise HTTPException(422, "Invalid participant ID")
    return data_root() / "versions" / participant


def map_identity(network):
    existing = network.get("transitMapMapId")
    if existing:
        return existing
    # Content identity, not a filename/city label. Saved descendants carry it forward.
    source = {key: network.get(key) for key in ("features", "coordinateSystem")}
    return hashlib.sha256(json.dumps(source, sort_keys=True, separators=(",", ":")).encode()).hexdigest()


def version_directory(participant, version_id):
    if not re.fullmatch(r"[a-f0-9]{32}", version_id):
        raise HTTPException(404, "Version not found")
    directory = versions_path(participant) / version_id
    if not (directory / "info.json").is_file():
        raise HTTPException(404, "Version not found")
    return directory


def save_version(network, body, core_state=None):
    name = body.get("name", "").strip()
    if not name or len(name) > 160:
        raise HTTPException(422, "Version name must contain 1–160 characters")
    svg = body.get("svg")
    canvas = body.get("canvas", {})
    if not isinstance(svg, str) or "<svg" not in svg:
        raise HTTPException(422, "Version SVG is required")
    if canvas.get("side") not in ("left", "right", "bottomLeft", "bottomRight"):
        raise HTTPException(422, "Select a canvas to save")
    version = {"id": uuid.uuid4().hex, "name": name,
               "createdAt": datetime.now(timezone.utc).isoformat(), "side": canvas["side"],
               "mapId": map_identity(network)}
    document = dict(network)
    document["transitMapCanvas"] = canvas
    document["transitMapCanvasState"] = {"version": 1, **body.get("canvasState", {})}
    document["transitMapStyle"] = {"version": 1, "css": canvas["styleCss"]}
    document["transitMapVersion"] = version
    document["transitMapMapId"] = version["mapId"]
    root = versions_path(body.get("participantId"))
    try:
        root.mkdir(parents=True, exist_ok=True)
        # Publish the pair together; incomplete writes never appear in the list.
        with tempfile.TemporaryDirectory(dir=root, prefix=".saving-") as temporary:
            pending = Path(temporary) / "version"
            pending.mkdir()
            (pending / "network.json").write_text(json.dumps(document, ensure_ascii=False, allow_nan=False, separators=(",", ":")), encoding="utf-8")
            (pending / "image.svg").write_text(svg, encoding="utf-8")
            (pending / "info.json").write_text(json.dumps(version, ensure_ascii=False), encoding="utf-8")
            if core_state is not None:
                (pending / "state.json").write_text(json.dumps(core_state, separators=(",", ":")), encoding="utf-8")
            pending.rename(root / version["id"])
    except OSError as exc:
        raise HTTPException(503, "Could not save version. Please retry.") from exc
    return version


@router.delete("/versions/{participant}/{version_id}")
def delete_version(participant: str, version_id: str):
    shutil.rmtree(version_directory(participant, version_id))
    version_state.cache_clear()
    version_render.cache_clear()
    return {"deleted": version_id}


@router.post("/versions/{participant}/compare")
def compare_versions(participant: str, body: dict):
    ids = list(dict.fromkeys(body.get("ids", [])))
    if len(ids) > 4:
        raise HTTPException(422, "Compare supports at most four versions")
    identities = set()
    documents = []
    known = set()
    for version_id in ids:
        if version_id == "current":
            from .edit_sessions import EditSessions
            _, _, state = EditSessions().read(body)
            document = {**state["original"], "transitMapTransform": state["transform"]}
            identities.add(map_identity(document))
            if document.get("transitMapMapId"):
                known.add(document["transitMapMapId"])
            documents.append(document)
        else:
            directory = version_directory(participant, version_id)
            info = json.loads((directory / "info.json").read_text(encoding="utf-8"))
            document = directory / "network.json"
            if info.get("mapId"):
                known.add(info["mapId"])
            if not info.get("mapId"):
                document = json.loads(document.read_text(encoding="utf-8"))
                info["mapId"] = map_identity(document)
            documents.append(document)
            identities.add(info["mapId"])
    if len(identities) > 1:
        if len(known) > 1:
            raise HTTPException(422, "These versions are from different maps and cannot be compared.")
        documents = [json.loads(item.read_text(encoding="utf-8")) if isinstance(item, Path) else item for item in documents]
        # Pre-lineage versions: require the same coordinate frame AND shared
        # station identifiers. A matching filename alone never establishes identity.
        def stations(document):
            return {str(f["properties"].get("station_id", f["properties"].get("id")))
                    for f in document.get("features", []) if f.get("geometry", {}).get("type") == "Point"
                    and (f.get("properties", {}).get("station_id") is not None or f.get("properties", {}).get("station_label"))}
        first = documents[0]
        legacy_match = len(known) <= 1 and bool(first.get("transitMapTransform")) and all(
            item.get("coordinateSystem") == first.get("coordinateSystem")
            and item.get("transitMapTransform") == first["transitMapTransform"]
            and len(stations(item) & stations(first)) >= 2 for item in documents[1:])
        if not legacy_match:
            raise HTTPException(422, "These versions are from different maps and cannot be compared.")
    return {"ids": ids}


@lru_cache(maxsize=16)
def version_state(directory):
    from .edit_sessions import core_call
    directory = Path(directory)
    document = json.loads((directory / "network.json").read_text(encoding="utf-8"))
    path = directory / "state.json"
    if path.is_file():
        state = json.loads(path.read_text(encoding="utf-8"))
    else:
        state = core_call({"op": "session", "map": document, "deferRender": True})["state"]
        state["original"] = document
    return document, state


@lru_cache(maxsize=16)
def version_render(directory, styles_json, scale):
    from .edit_sessions import core_call, geographic_map
    document, state = version_state(directory)
    result = core_call({"op": "render-geometry", "state": state,
                        "styles": json.loads(styles_json), "spacingScale": scale})
    return {**{key: value for key, value in result.items() if key != "state"},
            "geographicMap": geographic_map(state["original"], result["state"])}


@router.post("/versions/{participant}/{version_id}/preview")
def preview_version(participant: str, version_id: str, body: dict):
    directory = str(version_directory(participant, version_id))
    document, state = version_state(directory)
    if "styles" in body:
        scale = body.get("spacingScale", 1)
        if type(scale) not in (int, float) or not 0.1 <= scale <= 5:
            raise HTTPException(422, "Invalid zoom scale")
        return version_render(directory, json.dumps(body["styles"], sort_keys=True), scale)
    canvas = document["transitMapCanvas"]
    settings = document.get("transitMapCanvasState", {})
    return {"shape": state["shape"], "canvas": canvas, "canvasState": settings,
            "comments": document.get("transitMapComments", {}).get("items", []),
            "styleCss": document["transitMapStyle"]["css"],
            "style": settings.get("edits", {}).get(f'{canvas["side"]}:{canvas.get("styleName", "")}'),
            "version": document["transitMapVersion"]}


@router.get("/versions/{participant}")
def list_versions(participant: str):
    versions = [json.loads(path.read_text(encoding="utf-8"))
                for path in versions_path(participant).glob("*/info.json")]
    return {"versions": sorted(versions, key=lambda item: (item["createdAt"], item["id"]), reverse=True)}


@router.get("/versions/{participant}/{version_id}/{file_format}")
def download_version(participant: str, version_id: str, file_format: str):
    if not re.fullmatch(r"[a-f0-9]{32}", version_id) or file_format not in ("json", "svg"):
        raise HTTPException(404, "Version file not found")
    directory = versions_path(participant) / version_id
    path = directory / ("network.json" if file_format == "json" else "image.svg")
    if not path.is_file():
        raise HTTPException(404, "Version file not found")
    version = json.loads((directory / "info.json").read_text(encoding="utf-8"))
    filename = re.sub(r'[\\/\x00-\x1f]', '_', version["name"]) + "." + file_format
    return FileResponse(path, media_type="application/json" if file_format == "json" else "image/svg+xml", filename=filename)


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
            {
                "message": "City catalog is not installed",
                "dataRoot": str(data_root()),
                "citiesPath": str(root),
                "cwd": str(Path.cwd()),
                "TRANSITMAP_DATA_DIR": os.environ.get("TRANSITMAP_DATA_DIR"),
                "DATA_PATH_MODULES": os.environ.get("DATA_PATH_MODULES"),
            },
        )

    result = []

    for path in sorted(root.iterdir()):
        if re.fullmatch(r"[a-zA-Z0-9_-]+", path.name):
            try:
                city_path(path.name, ".json")
                result.append(path.name)
            except HTTPException:
                pass

    return {"cities": result}


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
        not in ("left", "right", "bottomLeft", "bottomRight")
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
