from app.modules.aquanote.repositories import local_fs


def get_compets() -> list[dict[str, str]]:
    return local_fs.list_compets()


def get_runs(compet: str) -> list[dict[str, str]]:
    return local_fs.list_runs(compet)


def get_datas(compet: str, run: str) -> list[dict[str, str]]:
    return local_fs.list_datas(compet, run)


def get_quality(compet: str, run: str) -> list[dict[str, str]]:
    return local_fs.list_quality(compet, run)
