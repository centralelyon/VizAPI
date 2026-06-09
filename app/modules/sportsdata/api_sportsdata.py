from fastapi import HTTPException, status
from app.modules.sportsdata import schemas_sportsdata, service_sportsdata
from app.modules.sportsdata.schemas_sportsdata import CatalogRead, ValidationRequest, ValidationResult
from app.types.module import Module


root = "sportsdata"
module = Module(
    root=root,
    tag="SportsData",
)


@module.router.get(
    "/catalogs/{catalog_id:path}",
    response_model=CatalogRead,
    status_code=200,
    summary="Get catalog from sportsdata",
    description="Fetch a catalog JSON from the sportsdata GitHub repository by its ID (e.g., common.sexes)",
)
async def get_catalog(catalog_id: str) -> CatalogRead:
    """
    Retrieve a catalog from sportsdata repository.
    
    The catalog_id is converted to a path:
    - common.sexes -> models/catalogs/common/sexes.json
    """
    catalog = await service_sportsdata.get_catalog(catalog_id)
    if catalog is None:
        raise HTTPException(
            status_code=status.HTTP_404_NOT_FOUND,
            detail=f"Catalog '{catalog_id}' not found",
        )
    return catalog


@module.router.post(
    "/validate",
    response_model=ValidationResult,
    status_code=200,
    summary="Validate value against catalog",
    description="Validate if a value exists in a sportsdata catalog",
)
async def validate_value(request: ValidationRequest) -> ValidationResult:
    """
    Validate a value against a catalog from sportsdata.
    
    Example request body:
    {
        "id": "common.sexes",
        "value": "hommes"
    }
    
    Example response:
    {
        "valid": true,
        "catalog_id": "common.sexes",
        "input_value": "hommes",
        "expected_values": ["hommes", "femmes", "mixte"],
        "message": null
    }
    """
    return await service_sportsdata.validate_value(request)
