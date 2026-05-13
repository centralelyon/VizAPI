from sqlalchemy import String
from sqlalchemy.orm import Mapped, mapped_column

from app.shared.db.session import Base


class TemplateItem(Base):
    __tablename__ = "template_items"

    id: Mapped[int] = mapped_column(primary_key=True, autoincrement=True) # we could use uuid too
    name: Mapped[str] = mapped_column(String(255), nullable=False)
    value: Mapped[str] = mapped_column(String(1024), nullable=False)
