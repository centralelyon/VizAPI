from typing import Optional
from app.modules.sportsdata.schemas_sportsdata import (
    Catalog,
    ValidationRequest,
    ValidationResult,
)
from app.modules.sportsdata import repositories_sportsdata


async def validate_value(request: ValidationRequest) -> ValidationResult:
    """
    Validate a value against a catalog from sportsdata.
    
    Args:
        request: ValidationRequest with id (e.g., "common.sexes") and value to check
    
    Returns:
        ValidationResult with valid=True/False and details
    """
    catalog = await repositories_sportsdata.get_catalog(request.id)
    
    if catalog is None:
        return ValidationResult(
            valid=False,
            catalog_id=request.id,
            input_value=request.value,
            expected_values=[],
            message=f"Catalog '{request.id}' not found in sportsdata repository",
        )
    
    # Extract all valid IDs from catalog
    valid_ids = [v.id for v in catalog.values]
    
    is_valid = request.value in valid_ids
    
    return ValidationResult(
        valid=is_valid,
        catalog_id=request.id,
        input_value=request.value,
        expected_values=valid_ids,
        message=None if is_valid else f"Value '{request.value}' is not in catalog '{request.id}'",
    )


async def get_catalog(catalog_id: str) -> Optional[Catalog]:
    """
    Get a catalog by its ID.
    """
    return await repositories_sportsdata.get_catalog(catalog_id)
