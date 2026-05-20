# VizAPI

FastAPI service that aggregates core and module-specific endpoints under a single API.

## Requirements

- Python 3.11+ (recommended)

## Setup

```bash
python -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
cp .env.template .env
```

## Run

```bash
fastapi dev
```

The API will be available at http://127.0.0.1:8000 and the OpenAPI docs at http://127.0.0.1:8000/docs.

## Docker

To build the Docker image:

```bash
docker build -t vizapi:latest .
```

To run the Docker container:

```bash
docker run -d --name vizapi --hostname vizapi -p 8000:8000 --env-file .env -v $(pwd)/vizapi-data:/vizapi/data vizapi:latest
  ```

or using Docker Compose:

```bash
docker compose up -d
```

## Modules

Modules are discovered automatically from:

- app/modules/* /api_*.py
- app/core/* /api_*.py

Each module should expose a `module` object with `router` and `root` fields.

## Tests

```bash
pytest
```
