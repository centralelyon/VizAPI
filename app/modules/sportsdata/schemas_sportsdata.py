from pydantic import BaseModel
from typing import Optional


class CatalogValue(BaseModel):
    id: str
    label: str


class Catalog(BaseModel):
    id: str
    title: Optional[str] = None
    values: list[CatalogValue]


class CatalogRead(Catalog):
    pass


class ValidationRequest(BaseModel):
    id: str  # e.g., "common.sexes"
    value: str  # e.g., "hommes" or "tree"


class ValidationResult(BaseModel):
    valid: bool
    catalog_id: str
    input_value: str
    expected_values: list[str]
    message: Optional[str] = None
