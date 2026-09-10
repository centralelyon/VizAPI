import csv
import io
import json
import os
import subprocess
import tempfile
import threading
import zipfile
from pathlib import Path


MAX_UPLOAD = 100 * 1024 * 1024
MAX_OUTPUT = 256 * 1024 * 1024

GTFS_SLOT = threading.BoundedSemaphore(1)


class GtfsBusyError(Exception):
    pass



def _read_csv_from_zip(archive, name):
    with archive.open(name) as file:
        text = io.TextIOWrapper(file, encoding="utf-8-sig", newline="")
        return list(csv.DictReader(text))


def _write_csv_to_zip(output_zip, name, rows, fieldnames):
    buffer = io.StringIO(newline="")
    writer = csv.DictWriter(buffer, fieldnames=fieldnames)
    writer.writeheader()
    writer.writerows(rows)

    output_zip.writestr(
        name,
        buffer.getvalue().encode("utf-8"),
    )


def filter_gtfs(input_path, output_path, selected_route_ids):
    selected_route_ids = set(str(x) for x in selected_route_ids)

    with zipfile.ZipFile(input_path, "r") as source:
        routes = _read_csv_from_zip(source, "routes.txt")
        trips = _read_csv_from_zip(source, "trips.txt")
        stop_times = _read_csv_from_zip(source, "stop_times.txt")
        stops = _read_csv_from_zip(source, "stops.txt")

        # 1. routes
        filtered_routes = [
            row for row in routes
            if row.get("route_id") in selected_route_ids
        ]

        if not filtered_routes:
            raise ValueError("None of the selected routes exist in routes.txt")

        # 2. trips
        filtered_trips = [
            row for row in trips
            if row.get("route_id") in selected_route_ids
        ]

        trip_ids = {
            row.get("trip_id")
            for row in filtered_trips
            if row.get("trip_id")
        }

        shape_ids = {
            row.get("shape_id")
            for row in filtered_trips
            if row.get("shape_id")
        }

        service_ids = {
            row.get("service_id")
            for row in filtered_trips
            if row.get("service_id")
        }

        # 3. stop_times
        filtered_stop_times = [
            row for row in stop_times
            if row.get("trip_id") in trip_ids
        ]

        stop_ids = {
            row.get("stop_id")
            for row in filtered_stop_times
            if row.get("stop_id")
        }

        # Include parent stations
        stop_by_id = {
            row.get("stop_id"): row
            for row in stops
        }

        parent_ids = set()

        for stop_id in list(stop_ids):
            stop = stop_by_id.get(stop_id)

            if stop:
                parent_id = stop.get("parent_station")

                if parent_id:
                    parent_ids.add(parent_id)

        stop_ids.update(parent_ids)

        # 4. stops
        filtered_stops = [
            row for row in stops
            if row.get("stop_id") in stop_ids
        ]

        replacements = {
            "routes.txt": filtered_routes,
            "trips.txt": filtered_trips,
            "stop_times.txt": filtered_stop_times,
            "stops.txt": filtered_stops,
        }

        # Optional files that also depend on the selected network
        if "shapes.txt" in source.namelist():
            shapes = _read_csv_from_zip(source, "shapes.txt")
            replacements["shapes.txt"] = [
                row for row in shapes
                if row.get("shape_id") in shape_ids
            ]

        if "frequencies.txt" in source.namelist():
            frequencies = _read_csv_from_zip(source, "frequencies.txt")
            replacements["frequencies.txt"] = [
                row for row in frequencies
                if row.get("trip_id") in trip_ids
            ]
            
        if "transfers.txt" in source.namelist():
            transfers = _read_csv_from_zip(source, "transfers.txt")

            replacements["transfers.txt"] = [
                row for row in transfers
                if row.get("from_stop_id") in stop_ids
                and row.get("to_stop_id") in stop_ids
            ]

        if "calendar.txt" in source.namelist():
            calendar = _read_csv_from_zip(source, "calendar.txt")
            replacements["calendar.txt"] = [
                row for row in calendar
                if row.get("service_id") in service_ids
            ]

        if "calendar_dates.txt" in source.namelist():
            calendar_dates = _read_csv_from_zip(source, "calendar_dates.txt")
            replacements["calendar_dates.txt"] = [
                row for row in calendar_dates
                if row.get("service_id") in service_ids
            ]

        with zipfile.ZipFile(
            output_path,
            "w",
            compression=zipfile.ZIP_DEFLATED,
        ) as destination:

            for info in source.infolist():
                name = info.filename

                if name in replacements:
                    rows = replacements[name]

                    # Preserve original columns
                    original_rows = _read_csv_from_zip(source, name)

                    if original_rows:
                        fieldnames = list(original_rows[0].keys())
                    else:
                        continue

                    _write_csv_to_zip(
                        destination,
                        name,
                        rows,
                        fieldnames,
                    )

                else:
                    destination.writestr(
                        info,
                        source.read(name),
                    )


def list_gtfs_routes(path):
    try:
        with zipfile.ZipFile(path) as archive:
            if "routes.txt" not in archive.namelist():
                raise ValueError("GTFS ZIP does not contain routes.txt")

            with archive.open("routes.txt") as file:
                text = io.TextIOWrapper(file, encoding="utf-8-sig", newline="")
                reader = csv.DictReader(text)

                routes = []

                for row in reader:
                    routes.append(
                        {
                            "route_id": row.get("route_id", ""),
                            "route_short_name": row.get("route_short_name", ""),
                            "route_long_name": row.get("route_long_name", ""),
                            "route_type": row.get("route_type", ""),
                            "route_color": row.get("route_color", ""),
                            "route_text_color": row.get("route_text_color", ""),
                        }
                    )

                return {"routes": routes}

    except zipfile.BadZipFile as exc:
        raise ValueError("Invalid ZIP archive") from exc
        
def run_gtfs(path, selected_route_ids=None):
    if not GTFS_SLOT.acquire(blocking=False):
        raise GtfsBusyError("GTFS conversion is busy")

    try:
        return _run_gtfs(
            path,
            selected_route_ids,
        )
    finally:
        GTFS_SLOT.release()

def _run_gtfs(path, selected_route_ids=None):

    try:
        with zipfile.ZipFile(path) as archive:
            names = set(archive.namelist())

            required = {
                "stops.txt",
                "routes.txt",
                "trips.txt",
                "stop_times.txt",
            }

            if not required.issubset(names):
                raise ValueError(
                    "GTFS ZIP must contain stops.txt, routes.txt, "
                    "trips.txt and stop_times.txt at its root"
                )

            if (
                sum(info.file_size for info in archive.infolist())
                > 1024 * 1024 * 1024
            ):
                raise ValueError("Uncompressed GTFS exceeds 1 GB")

    except zipfile.BadZipFile as exc:
        raise ValueError("Invalid ZIP archive") from exc


    # filtered.zip   -> filtered GTFS, if routes were selected
    # raw.json       -> output of gtfs2graph
    # topo.json      -> output of topo
    with tempfile.TemporaryDirectory() as directory:
        directory = Path(directory)

        raw = directory / "raw.json"
        topo = directory / "topo.json"

        # By default use the original GTFS ZIP
        gtfs_input = Path(path)

        if selected_route_ids:
            filtered_gtfs = directory / "filtered.zip"

            filter_gtfs(
                input_path=path,
                output_path=filtered_gtfs,
                selected_route_ids=selected_route_ids,
            )

            gtfs_input = filtered_gtfs

        stages = [
            (
                [
                    os.environ.get(
                        "GTFS2GRAPH_BIN",
                        "/usr/local/bin/gtfs2graph",
                    ),
                    str(gtfs_input),
                ],
                None,
                raw,
            ),
            (
                [
                    os.environ.get(
                        "TOPO_BIN",
                        "/usr/local/bin/topo",
                    )
                ],
                raw,
                topo,
            ),
        ]

        for command, source, destination in stages:
            with (
                open(source, "rb")
                if source
                else open(os.devnull, "rb")
            ) as stdin, open(
                destination,
                "wb",
            ) as stdout, tempfile.TemporaryFile() as stderr:

                result = subprocess.run(
                    command,
                    stdin=stdin,
                    stdout=stdout,
                    stderr=stderr,
                    timeout=600,
                    check=False,
                )

                if result.returncode:
                    stderr.seek(
                        max(
                            0,
                            stderr.tell() - 3000,
                        )
                    )

                    error_text = stderr.read().decode(
                        errors="replace"
                    )

                    raise RuntimeError(
                        f"{Path(command[0]).name} failed: "
                        f"{error_text}"
                    )

            if destination.stat().st_size > MAX_OUTPUT:
                raise RuntimeError(
                    "Conversion output exceeds 256 MB"
                )

        try:
            graph = json.loads(
                topo.read_text(
                    encoding="utf-8"
                )
            )

            if (
                not isinstance(graph, dict)
                or graph.get("type") != "FeatureCollection"
                or not graph.get("features")
            ):
                raise ValueError("Empty network")

        except ValueError as exc:
            raise RuntimeError(
                "topo did not return a valid network GeoJSON"
            ) from exc

        return graph
