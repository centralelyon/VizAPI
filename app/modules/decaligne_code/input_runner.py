from .gpx_runner import convert_gpx
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
        if lower.endswith(".gpx"):
            return {"type":"gpx", "routes":[]}
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
    if all(name.lower().endswith((".csv", ".jsonl")) for name, _ in files):
        return list_trajectory_routes(files)
    if len(files) == 1 and files[0][0].lower().endswith((".csv", ".jsonl")):
        return list_trajectory_routes(files)
    raise ValueError("Choose GPX, JSON/GeoJSON, one GTFS/trajectory ZIP, or trajectory CSV/JSONL files")


def convert_input(files, route_ids=None):
    if route_ids and len(route_ids) > 500:
        raise ValueError("The existing edit engine supports at most 500 loaded routes. Select a smaller subset in the legend.")
    if len(files) == 1 and files[0][0].lower().endswith((".json", ".geojson")):
        return _validate_json(files[0][1])
    info = inspect_input(files)
    kind = info["type"]
    if kind == "gpx":
        return convert_gpx(*files[0])
    if kind == "json":
        return _validate_json(files[0][1])
    if kind == "gtfs":
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "feed.zip"
            path.write_bytes(files[0][1])
            graph = run_gtfs(path, route_ids or [])
            graph["routeModes"] = {str(r["route_id"]): "rail" if str(r.get("route_type")) in ("2","100","101","102","103","106","109") else "transit" for r in info["routes"]}
            return graph
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
