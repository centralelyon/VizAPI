# Add a New Module

This guide walks through creating a new business domain module from scratch. We'll use `template` module as a concrete example.

---

## 1. Create the directory structure

```bash
mkdir app/modules/template/
touch app/modules/template/__init__.py
touch app/modules/template/api_template.py
touch app/modules/template/models_template.py
touch app/modules/template/schemas_template.py
touch app/modules/template/services_template.py
touch app/modules/template/repositories_template.py
touch app/modules/template/dependencies_template.py
```

---

## 2. Define the ORM model

In the case you are using a database, we fisrt create the datastructures of the module by defining the SQLAlchemy models.

Create the SQLAlchemy model using `Base` type from `shared`.

```python
# app/modules/template/models_template.py
from sqlalchemy.orm import Mapped, mapped_column
from app.shared.db.base import Base

class TemplateItem(Base):
    __tablename__ = "items"

    id: Mapped[int] = mapped_column(primary_key=True, autoincrement=True)
    name: Mapped[str] = mapped_column(String(1024), nullable=False)
    value: Mapped[float] = mapped_column(String(1024), nullable=False)
```

Models represent the database SQL tables and are used in the repository layer for data access. They should not contain any business logic or validation rules.

---

## 3. Define Pydantic schemas

Schemas are meant for data validation when receiving input from the client (e.g. in POST requests) and for shaping the output data (e.g. in GET responses).

```python
# app/modules/template/schemas_template.py
from pydantic import BaseModel
from datetime import datetime

class TemplateItemCreate(BaseModel):
    name: str
    value: float

class TemplateItemRead(BaseModel):
    id: int
    name: str
    value: float

    model_config = {"from_attributes": True}
```

---

## 4. Create the repository

Repositories are responsible for all data access logic — SQL queries, file system operations, or external API calls. They should not contain any business rules/logic.

```python
# app/modules/template/repositories_template.py
from sqlalchemy.ext.asyncio import AsyncSession
from sqlalchemy import select
from app.modules.template.models_template import TemplateItem
from app.modules.template.schemas_template import TemplateItemCreate

class TemplateItemRepository:
    def __init__(self, session: AsyncSession):
        self.session = session

    async def create(self, data: TemplateItemCreate) -> TemplateItem:
        template_item = TemplateItem(**data.model_dump())
        self.session.add(template_item)
        await self.session.commit()
        await self.session.refresh(template_item)
        return template_item

    async def list_for_user(self, user_id: str) -> list[TemplateItem]:
        result = await self.session.scalars(
            select(TemplateItem).where(TemplateItem.user_id == user_id)
        )
        return list(result.all())

```

---

## 5. Create the service

```python
# app/modules/template/services_template.py
from app.modules.template.repositories_template import TemplateItemRepository
from app.modules.template.schemas_template import TemplateItemCreate, TemplateItemRead

class TemplateItemService:
    def __init__(self, repo: TemplateItemRepository):
        self.repo = repo

    async def create(self, data: TemplateItemCreate) -> TemplateItemRead:
        template_item = await self.repo.create(data)
        return TemplateItemRead.model_validate(template_item)

    async def list_for_user(self, user_id: str) -> list[TemplateItemRead]:
        template_items = await self.repo.list_for_user(user_id)
        return [TemplateItemRead.model_validate(t) for t in template_items]
```

---

## 6. Wire dependencies

```python
# app/modules/template/dependencies_template.py
from fastapi import Depends
from sqlalchemy.ext.asyncio import AsyncSession
from shared.db.session import get_db
from core.permissions.dependencies import require_permission
from app.modules.template.repositories_template import TemplateItemRepository
from app.modules.template.services_template import TemplateItemService

def get_template_item_service(db: AsyncSession = Depends(get_db)) -> TemplateItemService:
    return TemplateItemService(TemplateItemRepository(db))

require_template_items_read = require_permission("template_items:read")
```

---

## 9. Register the model for Alembic

```python
# alembic/env.py
import app.modules.template.models_template  # noqa: F401
```

Then generate and apply the migration:

```bash
alembic revision --autogenerate -m "add template items table"
alembic upgrade head
```

---

<!-- ## 10. Add permissions for the new module
(Maybe later)
Grant the `template_items:read` permission to a user (via admin tooling or directly in the DB during development):

```sql
INSERT INTO user_permissions (user_id, permission)
VALUES ('<user_id>', 'template_items:read');
``` -->

---

## Checklist

- [ ] Directory structure + `__init__.py` files created
- [ ] ORM model inherits `TimestampMixin, Base`
- [ ] Pydantic schemas defined with `model_config = {"from_attributes": True}`
- [ ] Repository contains only data access logic
- [ ] Service contains only business logic, calls repository
- [ ] Dependencies wire service and attach permission guards
- [ ] Router uses only `Depends()` — no direct instantiation
- [ ] Router registered in `main.py`
- [ ] Model imported in `alembic/env.py`
- [ ] Migration generated and applied
- [ ] Tests written in `tests/modules/test_template_items.py`
