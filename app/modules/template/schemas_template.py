from datetime import datetime

from pydantic import BaseModel


class TemplateItemCreate(BaseModel):
    name: str
    value: str


class TemplateItemRead(BaseModel):
    id: int
    name: str
    value: str
    model_config = {"from_attributes": True}


class UploadedFileRead(BaseModel):
    id: int
    original_filename: str
    content_type: str
    size: int
    uploaded_at: datetime
    model_config = {"from_attributes": True}
