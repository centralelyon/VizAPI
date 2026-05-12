import importlib
from pathlib import Path
from app.types.module import CoreModule, Module

all_modules: list[CoreModule | Module] = []
core_modules: list[CoreModule] = []
module_list: list[Module] = []

for endpoint_files in Path().glob("app/modules/*/api/api.py"):
    endpoint_module = importlib.import_module(
        ".".join(endpoint_files.with_suffix("").parts),
    )
    if hasattr(endpoint_module, "module"):
        module: Module = endpoint_module.module
        module_list.append(module)
        all_modules.append(module)
    else:
        print(f"Module in {endpoint_files} does not declare a 'module', skipping.")

for endpoint_files in Path().glob("app/core/*/api/api.py"):
    endpoint_module = importlib.import_module(
        ".".join(endpoint_files.with_suffix("").parts),
    )
    if hasattr(endpoint_module, "module"):
        core_module: CoreModule = endpoint_module.module
        core_modules.append(core_module)
        all_modules.append(core_module)
    else:
        print(f"Core module in {endpoint_files} does not declare a 'module', skipping.")
