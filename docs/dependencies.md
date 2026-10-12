# Зависимости

Перед добавлением зависимости проверить лицензию и дописать строку сюда (CLAUDE.md, правило 10).
Версии C++-библиотек задаёт baseline vcpkg в `vcpkg-configuration.json`
(сейчас `9e593bb18ea69cc5095e012465dcd675a822ed0d`, тег `2026.07.29`).

## Подключены

| Зависимость | Версия | Лицензия | Как подключена | Где используется |
|---|---|---|---|---|
| GoogleTest | 1.17.0 | BSD-3-Clause | vcpkg, только тесты | `tests/` |
| {fmt} | 12.2.0 | MIT | vcpkg | пока не используется: журналу 1.3 хватило `std::to_chars` (см. `docs/backlog.md`) |
| spdlog | 1.17.0 | MIT | vcpkg | логирование (задача 1.7) |
| nlohmann/json | 3.12.0 | MIT | vcpkg, header-only | Model IR, настройки (с задачи 1.2) |
| toml++ | 3.4.0 | MIT | vcpkg, header-only | манифесты модулей (задача 1.5) |
| Qt 6 (qtbase: Core, Gui, Widgets) | 6.11.1 | LGPL-3.0-only | vcpkg, фича `gui`; в дистрибутиве только динамическая линковка | `shell/` |
| Qt Advanced Docking System | 4.5.0 | LGPL-2.1-only | vcpkg (порт `qt-advanced-docking-system`), фича `gui` | `shell/` |

## Инструменты разработки (в продукт не входят)

| Инструмент | Версия | Лицензия | Как подключен |
|---|---|---|---|
| CMake | ≥ 3.28 | BSD-3-Clause | системный |
| Ninja | 1.11.1.1 | Apache-2.0 | `python/requirements-dev.txt` |
| vcpkg | baseline выше | MIT | `VCPKG_ROOT` |
| clang-format | 18.1.8 | Apache-2.0 WITH LLVM-exception | `python/requirements-dev.txt` |
| pytest | 8.3.3 | MIT | `python/requirements-dev.txt` |
| ruff | 0.6.9 | MIT | `python/requirements-dev.txt` |
| black | 24.8.0 | MIT | `python/requirements-dev.txt` |

Генератор бренд-кита `resources/branding/stresslab-kit/source/build.py` в сборке не участвует и нужен только для
перерисовки кита. Он использует CairoSVG (LGPL-3.0-or-later), Pillow (MIT-CMU) и fontTools (MIT); версии не закреплены,
в `python/requirements-dev.txt` они не входят.

## Шрифты (рекомендованы, пока не подключены)

| Шрифт | Лицензия | Статус | Где будет использоваться |
|---|---|---|---|
| Inter | SIL OFL 1.1 | рекомендован бренд-китом (`guidelines.md` §5); файлов шрифта в репозитории нет, в сборку не входит | интерфейс оболочки (решить в задаче 1.11) и документация; при встраивании в дистрибутив положить рядом текст OFL |
| Inter Display | SIL OFL 1.1 | только в словесном знаке логотипа, переведён в кривые; файлов шрифта в репозитории нет | `resources/branding/stresslab-kit/logo/` |

## Запланированы (по `docs/architecture.md` §9–10)

| Зависимость | Лицензия | Как будет подключена | Задача |
|---|---|---|---|
| Python | PSF-2.0 | встроенный интерпретатор 3.12 | 1.8 |
| nanobind | BSD-3-Clause | vcpkg | 1.8 |
| numpy | BSD-3-Clause | pip, в site-packages дистрибутива | 1.8 |
| HDF5 | BSD-3-Clause (вариант HDF Group) | vcpkg | 2.2 |
| zstd | BSD-3-Clause | vcpkg | 2.2 |
| libgit2 | GPL-2.0 с linking exception | vcpkg | 2.3 |
| Eigen | MPL-2.0 | vcpkg, header-only | по необходимости |
| VTK 9 | BSD-3-Clause | vcpkg, фича `gui` | 3.7 |
| CalculiX (ccx) | GPL-2.0 | **отдельный процесс**, не линкуется; бинарник и исходники в дистрибутиве | 3.5 |
| OpenCASCADE | LGPL-2.1 с исключением | vcpkg | этап 6 |
| Gmsh | GPL-2.0-or-later | **отдельный процесс**, не линкуется | этап 6 |
| Netgen | LGPL-2.1 | vcpkg или сборка из исходников | этап 6 |

Не используются: TetGen (AGPL).
