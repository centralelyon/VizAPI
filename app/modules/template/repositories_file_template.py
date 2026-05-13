import json
from pathlib import Path

_DATA_FILE = Path(__file__).parent / "data" / "sample.json"


def read() -> dict:
    if not _DATA_FILE.exists():
        return {}
    with _DATA_FILE.open("r", encoding="utf-8") as f:
        return json.load(f)


def write(data: dict) -> None:
    _DATA_FILE.parent.mkdir(parents=True, exist_ok=True)
    with _DATA_FILE.open("w", encoding="utf-8") as f:
        json.dump(data, f, indent=2)
