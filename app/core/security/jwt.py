import time

from joserfc import jwt
from joserfc.errors import BadSignatureError, ExpiredTokenError, JoseError
from joserfc.jwk import OctKey
from joserfc.jwt import JWTClaimsRegistry

from app.shared.utils.settings import get_settings

ALGORITHM = "HS256"


def _make_key() -> OctKey:
    return OctKey.import_key(get_settings().SECRET_KEY.encode("utf-8"))


def create_token(payload: dict) -> str:
    settings = get_settings()
    now = int(time.time())
    claims = {
        **payload,
        "iat": now,
        "exp": now + settings.ACCESS_TOKEN_EXPIRE_MINUTES * 60,
    }
    return jwt.encode(header={"alg": ALGORITHM}, claims=claims, key=_make_key())


def verify_token(token: str) -> dict:
    try:
        decoded = jwt.decode(value=token, key=_make_key(), algorithms=[ALGORITHM])
        JWTClaimsRegistry(exp={"essential": True}).validate(decoded.claims)
        return decoded.claims
    except ExpiredTokenError:
        raise ValueError("Token has expired")
    except (BadSignatureError, JoseError) as exc:
        raise ValueError(f"Token validation failed: {exc}") from exc
