from app.modules.aquanote import repositories_file_aquanote


def get_compets() -> list[dict[str, str]]:
    return repositories_file_aquanote.list_compets()


def get_runs(compet: str) -> list[dict[str, str]]:
    return repositories_file_aquanote.list_runs(compet)


def get_datas(compet: str, run: str) -> list[dict[str, str]]:
    return repositories_file_aquanote.list_datas(compet, run)


def get_quality(compet: str, run: str) -> list[dict[str, str]]:
    return repositories_file_aquanote.list_quality(compet, run)
