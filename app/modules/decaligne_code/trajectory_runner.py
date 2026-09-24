import csv
import io
import zipfile
from collections import OrderedDict
from pathlib import Path


REQUIRED_COLUMNS = {
    "participant_id",
    "x",
    "y",
    "raw_index",
}

ROUTE_COLORS = [
    "E41A1C",
    "377EB8",
    "4DAF4A",
    "984EA3",
    "FF7F00",
    "A65628",
    "F781BF",
    "17BECF",
    "BCBD22",
    "1F77B4",
    "D62728",
    "2CA02C",
    "9467BD",
    "8C564B",
    "E377C2",
    "7F7F7F",
]


def _decode_csv(data: bytes, filename: str):
    try:
        text = data.decode("utf-8-sig")
    except UnicodeDecodeError as exc:
        raise ValueError(
            f"{filename}: CSV must be UTF-8"
        ) from exc

    reader = csv.DictReader(
        io.StringIO(text, newline="")
    )

    fields = set(reader.fieldnames or [])

    missing = REQUIRED_COLUMNS - fields

    if missing:
        raise ValueError(
            f"{filename}: missing CSV columns: "
            f"{', '.join(sorted(missing))}"
        )

    rows = list(reader)

    if not rows:
        raise ValueError(
            f"{filename}: CSV contains no trajectory rows"
        )

    return rows


def csv_files_from_zip(data: bytes):
    try:
        archive = zipfile.ZipFile(
            io.BytesIO(data)
        )
    except zipfile.BadZipFile as exc:
        raise ValueError(
            "Invalid ZIP file"
        ) from exc

    with archive:
        names = [
            name
            for name in archive.namelist()
            if not name.endswith("/")
            and name.lower().endswith(".csv")
        ]

        if not names:
            raise ValueError(
                "ZIP is neither a GTFS feed "
                "nor a CSV trajectory archive"
            )

        return [
            (
                Path(name).name,
                archive.read(name),
            )
            for name in names
        ]


def is_gtfs_zip(data: bytes):

    try:
        with zipfile.ZipFile(
            io.BytesIO(data)
        ) as archive:

            names = {
                Path(name).name.lower()
                for name in archive.namelist()
                if not name.endswith("/")
            }

    except zipfile.BadZipFile:
        return False

    required = {
        "routes.txt",
        "trips.txt",
        "stops.txt",
        "stop_times.txt",
    }

    return required.issubset(names)


def _route_from_file(
    filename: str,
    data: bytes,
):

    rows = _decode_csv(
        data,
        filename,
    )

    route_ids = {
        str(
            row.get(
                "participant_id",
                "",
            )
        ).strip()
        for row in rows
    }

    route_ids.discard("")

    if len(route_ids) != 1:
        raise ValueError(
            f"{filename}: expected exactly one "
            "participant_id "
            "(one CSV = one route)"
        )

    route_id = next(
        iter(route_ids)
    )

    samples = []

    for row_number, row in enumerate(
        rows,
        start=2,
    ):
        try:
            x = float(row["x"])
            y = float(row["y"])
            sequence = float(
                row["raw_index"]
            )

        except (
            TypeError,
            ValueError,
        ) as exc:

            raise ValueError(
                f"{filename}:{row_number}: "
                "x, y and raw_index "
                "must be numeric"
            ) from exc

        samples.append(
            (
                sequence,
                row_number,
                x,
                y,
            )
        )

    samples.sort(
        key=lambda item: (
            item[0],
            item[1],
        )
    )

    ordered_points = []

    for _, _, x, y in samples:

        point = (
            x,
            y,
        )

        if (
            not ordered_points
            or ordered_points[-1]
            != point
        ):
            ordered_points.append(
                point
            )

    if len(ordered_points) < 2:
        raise ValueError(
            f"{filename}: route needs "
            "at least two distinct "
            "consecutive positions"
        )

    return {
        "route_id": route_id,
        "filename": filename,
        "points": ordered_points,
    }


def parse_routes(files):

    routes = [
        _route_from_file(
            name,
            data,
        )
        for name, data in files
    ]

    seen = set()

    for route in routes:

        route_id = route["route_id"]

        if route_id in seen:
            raise ValueError(
                "Duplicate route_id/"
                "participant_id across "
                f"CSV files: {route_id}"
            )

        seen.add(route_id)

    return routes


def list_trajectory_routes(files):

    routes = parse_routes(files)

    return {
        "type": "trajectory",
        "coordinateSystem": "planar",
        "routes": [
            {
                "route_id":
                    route["route_id"],

                "route_name":
                    route["route_id"],

                "filename":
                    route["filename"],
            }
            for route in routes
        ],
    }


def _route_color(index: int):


    return ROUTE_COLORS[
        index % len(ROUTE_COLORS)
    ]


def build_trajectory_network(
    files,
    selected_route_ids=None,
):

    routes = parse_routes(files)

    selected = {
        str(route_id)
        for route_id in (
            selected_route_ids or []
        )
    }

    if selected:
        routes = [
            route
            for route in routes
            if route["route_id"]
            in selected
        ]

    if not routes:
        raise ValueError(
            "No selected CSV routes remain"
        )

    for index, route in enumerate(routes):
        route["color"] = _route_color(
            index
        )

    node_ids = OrderedDict()

    for route in routes:

        for point in route["points"]:

            if point not in node_ids:
                node_ids[point] = (
                    f"n{len(node_ids)}"
                )

    routes_at_point = {
        point: set()
        for point in node_ids
    }

    for route in routes:

        route_id = route["route_id"]

        for point in route["points"]:
            routes_at_point[
                point
            ].add(route_id)

    features = []

    for point, node_id in (
        node_ids.items()
    ):

        route_ids = sorted(
            routes_at_point[point]
        )

        is_interchange = (
            len(route_ids) > 1
        )

        features.append(
            {
                "type": "Feature",

                "geometry": {
                    "type": "Point",
                    "coordinates": [
                        point[0],
                        point[1],
                    ],
                },

                "properties": {
                    "id": node_id,

                    "station_id":
                        node_id,

                    "station_label":
                        node_id,

                    "interchange":
                        is_interchange,

                    "route_ids":
                        route_ids,
                },
            }
        )

    segments = OrderedDict()

    for route in routes:

        route_id = route["route_id"]

        line = {
            "id": route_id,
            "name": route_id,
            "label": route_id,
            "color": route["color"],
        }

        points = route["points"]

        for a, b in zip(
            points,
            points[1:],
        ):

            if a == b:
                continue

            from_id = node_ids[a]
            to_id = node_ids[b]


            key = tuple(
                sorted(
                    (
                        from_id,
                        to_id,
                    )
                )
            )

            if key not in segments:

                segments[key] = {
                    "a": a,
                    "b": b,
                    "from": from_id,
                    "to": to_id,
                    "lines": [],
                }

            segment = segments[key]

            #
            # Add this route membership once.
            #
            if not any(
                existing["id"]
                == route_id
                for existing
                in segment["lines"]
            ):
                segment[
                    "lines"
                ].append(
                    dict(line)
                )

    for index, segment in enumerate(
        segments.values()
    ):

        features.append(
            {
                "type": "Feature",

                "geometry": {
                    "type":
                        "LineString",

                    "coordinates": [
                        [
                            segment["a"][0],
                            segment["a"][1],
                        ],
                        [
                            segment["b"][0],
                            segment["b"][1],
                        ],
                    ],
                },

                "properties": {
                    "id":
                        f"s{index}",

                    "from":
                        segment["from"],

                    "to":
                        segment["to"],

                    "lines":
                        segment["lines"],
                },
            }
        )

    return {
        "type": "FeatureCollection",

        "coordinateSystem": "planar",

        "inputType": "trajectory",

        "features": features,
    }