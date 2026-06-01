import time
from collections import defaultdict, deque

from starlette.middleware.base import BaseHTTPMiddleware
from starlette.requests import Request
from starlette.responses import JSONResponse

type ClientIP = str
type Timestamp = float
type RateLimitBuckets = defaultdict[ClientIP, deque[Timestamp]]


def _get_client_ip(request: Request) -> ClientIP:
    forwarded_for = request.headers.get("x-forwarded-for")
    if forwarded_for:
        return forwarded_for.split(",")[0].strip()
    return request.client.host if request.client else "unknown"


class RateLimitMiddleware(BaseHTTPMiddleware):
    """Sliding-window rate limiter keyed on client IP."""

    def __init__(self, app, requests_per_window: int = 60, window_seconds: int = 60):
        super().__init__(app)
        self.requests_per_window = requests_per_window
        self.window_seconds = window_seconds
        self._buckets: RateLimitBuckets = defaultdict(deque)

    async def dispatch(self, request: Request, call_next):
        ip = _get_client_ip(request)
        now = time.monotonic()
        window_start = now - self.window_seconds

        bucket = self._buckets[ip]
        while bucket and bucket[0] < window_start:
            bucket.popleft()

        if len(bucket) >= self.requests_per_window:
            retry_after = int(bucket[0] - window_start) + 1
            return JSONResponse(
                {"detail": "Too many requests"},
                status_code=429,
                headers={"Retry-After": str(retry_after)},
            )

        bucket.append(now)
        return await call_next(request)
