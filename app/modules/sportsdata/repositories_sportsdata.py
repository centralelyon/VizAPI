import httpx
import os
from pathlib import Path
from typing import Optional
from app.core.config import construct_settings
from app.modules.sportsdata.schemas_sportsdata import Catalog


_settings = construct_settings()

# Set SPORTSDATA_REPO_ROOT to the local clone of sportsdata repo
# This allows sportsdata_models to load catalogs from the cloned repository
SPORTSDATA_LOCAL_PATH = Path(_settings.DATA_PATH_MODULES).parent / "external" / "sportsdata"
if SPORTSDATA_LOCAL_PATH.exists():
    os.environ["SPORTSDATA_REPO_ROOT"] = str(SPORTSDATA_LOCAL_PATH)

BASE_GITHUB_URL = "https://raw.githubusercontent.com/centralelyon/sportsdata/main"


def _catalog_id_to_path(catalog_id: str) -> str:
    """
    Convert catalog_id like 'common.sexes' to GitHub path:
    models/catalogs/common/sexes.json
    """
    parts = catalog_id.split(".")
    return f"models/catalogs/{'/'.join(parts)}.json"


def _catalog_id_to_scope_and_name(catalog_id: str) -> tuple[str, str]:
    """
    Convert catalog_id like 'common.sexes' to scope and name for sportsdata_models.
    Returns: (scope, name) e.g., ('common', 'sexes')
    """
    parts = catalog_id.split(".")
    if len(parts) >= 2:
        return ".".join(parts[:-1]), parts[-1]
    return "", catalog_id


def get_catalog_from_sportsdata_package(catalog_id: str) -> Optional[Catalog]:
    """
    Try to load catalog from the sportsdata Python package (sportsdata_models).
    This is the primary method when the package is installed and SPORTSDATA_REPO_ROOT is set.
    """
    try:
        from sportsdata_models import load_catalog
        scope, name = _catalog_id_to_scope_and_name(catalog_id)
        if not scope:
            return None
        catalog_data = load_catalog(scope, name)
        return Catalog(**catalog_data)
    except (ImportError, ModuleNotFoundError):
        # sportsdata_models package not installed
        return None
    except (FileNotFoundError, ValueError, KeyError) as e:
        # Catalog not found in the package
        return None


async def fetch_catalog_from_github(catalog_id: str) -> Optional[Catalog]:
    """
    Fetch a catalog JSON from GitHub repository.
    Returns Catalog object or None if not found/error.
    """
    github_path = _catalog_id_to_path(catalog_id)
    url = f"{BASE_GITHUB_URL}/{github_path}"
    
    async with httpx.AsyncClient() as client:
        try:
            response = await client.get(url, timeout=30.0)
            response.raise_for_status()
            data = response.json()
            return Catalog(**data)
        except (httpx.HTTPError, ValueError) as e:
            # Try with .json extension if not already present
            if not github_path.endswith(".json"):
                url_json = f"{url}.json"
                try:
                    response = await client.get(url_json, timeout=30.0)
                    response.raise_for_status()
                    data = response.json()
                    return Catalog(**data)
                except (httpx.HTTPError, ValueError):
                    pass
            return None


async def get_catalog(catalog_id: str) -> Optional[Catalog]:
    """
    Get catalog by ID.
    Priority:
    1. sportsdata_models Python package (if installed with SPORTSDATA_REPO_ROOT)
    2. GitHub repository (HTTP fetch)
    """
    # Try Python package first
    catalog = get_catalog_from_sportsdata_package(catalog_id)
    if catalog is not None:
        return catalog
    
    # Fall back to GitHub
    return await fetch_catalog_from_github(catalog_id)
