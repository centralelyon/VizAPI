from pydantic import BaseModel


class TemplateItemCreate(BaseModel):
    name: str
    value: str


class TemplateItemRead(BaseModel):
    id: int
    name: str
    value: str
    model_config = {"from_attributes": True}
