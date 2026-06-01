from fastapi import FastAPI
from fastapi.testclient import TestClient

from app.types.middleware import RateLimitMiddleware


def _make_app(requests_per_window: int = 3, window_seconds: int = 60) -> FastAPI:
    app = FastAPI()
    app.add_middleware(
        RateLimitMiddleware,
        requests_per_window=requests_per_window,
        window_seconds=window_seconds,
    )

    @app.get("/ping")
    def ping():
        return {"ok": True}

    return app


def test_requests_under_limit_pass():
    client = TestClient(_make_app(requests_per_window=3))
    for _ in range(3):
        assert client.get("/ping").status_code == 200


def test_request_over_limit_returns_429():
    client = TestClient(_make_app(requests_per_window=3))
    for _ in range(3):
        client.get("/ping")
    response = client.get("/ping")
    assert response.status_code == 429
    assert response.json() == {"detail": "Too many requests"}


def test_429_includes_retry_after_header():
    client = TestClient(_make_app(requests_per_window=1))
    client.get("/ping")
    response = client.get("/ping")
    assert "retry-after" in response.headers


def test_rate_limit_is_per_ip():
    app = _make_app(requests_per_window=2)
    client = TestClient(app)

    # exhaust limit for 127.0.0.1
    for _ in range(2):
        client.get("/ping", headers={"x-forwarded-for": "1.2.3.4"})

    assert client.get("/ping", headers={"x-forwarded-for": "1.2.3.4"}).status_code == 429
    assert client.get("/ping", headers={"x-forwarded-for": "9.9.9.9"}).status_code == 200


def test_x_forwarded_for_first_ip_is_used():
    client = TestClient(_make_app(requests_per_window=1))
    client.get("/ping", headers={"x-forwarded-for": "1.2.3.4, 10.0.0.1"})
    # second request from same first IP → blocked
    assert client.get("/ping", headers={"x-forwarded-for": "1.2.3.4, 10.0.0.1"}).status_code == 429
    # different first IP → allowed
    assert client.get("/ping", headers={"x-forwarded-for": "5.5.5.5, 10.0.0.1"}).status_code == 200
