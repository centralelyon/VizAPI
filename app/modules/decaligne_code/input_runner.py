import json
import tempfile
from pathlib import Path

from app.modules.decaligne_code.gtfs_runner import list_gtfs_routes, run_gtfs
from app.modules.decaligne_code.trajectory_runner import (
    build_trajectory_network, csv_files_from_zip, is_gtfs_zip, list_trajectory_routes,
)


def inspect_input(files):
    if not files:
        raise ValueError("No input files")
    if len(files) == 1:
        name, data = files[0]
        lower = name.lower()
        if lower.endswith((".json", ".geojson")):
            _validate_json(data)
            return {"type": "json", "routes": []}
        if lower.endswith(".zip"):
            if is_gtfs_zip(data):
                with tempfile.TemporaryDirectory() as directory:
                    path = Path(directory) / "feed.zip"
                    path.write_bytes(data)
                    result = list_gtfs_routes(path)
                    return {"type": "gtfs", "routes": result.get("routes", [])}
            return list_trajectory_routes(csv_files_from_zip(data))
    if all(name.lower().endswith(".csv") for name, _ in files):
        return list_trajectory_routes(files)
    if len(files) == 1 and files[0][0].lower().endswith(".csv"):
        return list_trajectory_routes(files)
    raise ValueError("Choose JSON/GeoJSON, one GTFS/CSV ZIP, or one or more trajectory CSV files")


def convert_input(files, route_ids=None):
    info = inspect_input(files)
    kind = info["type"]
    if kind == "json":
        return _validate_json(files[0][1])
    if kind == "gtfs":
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "feed.zip"
            path.write_bytes(files[0][1])
            return run_gtfs(path, route_ids or [])
    trajectory_files = csv_files_from_zip(files[0][1]) if len(files) == 1 and files[0][0].lower().endswith(".zip") else files
    return build_trajectory_network(trajectory_files, route_ids or [])


def _validate_json(data):
    try:
        graph = json.loads(data.decode("utf-8-sig"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise ValueError("Invalid JSON/GeoJSON input") from exc
    if not isinstance(graph, dict) or graph.get("type") != "FeatureCollection" or not isinstance(graph.get("features"), list):
        raise ValueError("Expected a GeoJSON FeatureCollection")
    return graph
