# VizAPI

**VizAPI** is a scalable, multi-module backend API built with [FastAPI](https://fastapi.tiangolo.com/). It serves as the unified backend for multiple applications — data visualization dashboards, video management, user data persistence, and future services.

---

## Key principles

| Principle | Description |
| --- | --- |
| **Multi-module** | Each business domain lives in its own isolated module |
| **Clean architecture** | Strict separation: API → Service → Repository |
| **Extensible** | Adding a new module never interacts with existing ones |
| **Testable** | Every layer is independently testable; no hidden coupling |

---

## Quick overview

```text
app/
├── core/          # Auth, users, permissions, security (shared by all modules)
├── shared/        # DB session, base models, utilities
└── modules/       # Business domains (video, analytics, site_x…)
```

* The **core** handles everything cross-cutting: who you are and what you are allowed to do. 
* The **shared** layer provides the technical infrastructure (database connection, base ORM class). 
* **Modules** are where the actual business logic lives — fully isolated from each other.

---

## Tech stack

<div align="center" markdown>

| Component | Technology |
| --- | --- |
| Framework | FastAPI |
| ORM | SQLAlchemy (async) |
| Database | SQLite (dev) / PostgreSQL (prod) |
| Auth | OAuth2 GitHub + JWT (`python-jose`) |
| Validation | Pydantic |
| Migrations | Alembic |
| Docs | MkDocs Material |

</div>
---

## Where to start

<div class="grid cards" markdown>

- :material-clock-fast: **[Getting Started](guides/getting-started.md)**

    Install dependencies, configure your environment, and run the API locally in minutes.

- :material-floor-plan: **[Architecture Overview](architecture/overview.md)**

    Understand the three-layer model (core / shared / modules) and the data flow.

- :material-puzzle: **[Add a New Module](guides/add-module.md)**

    Step-by-step guide to scaffolding a new business domain module.

- :material-shield-key: **[Permissions System](guides/permissions.md)**

    Learn how string-based RBAC works and how to protect your endpoints.

</div>
