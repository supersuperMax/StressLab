# FEM-APP

Кросс-платформенный пакет конечно-элементного анализа с модульной оболочкой в стиле VS Code.
Ядро распространяется под лицензией MIT (см. `LICENSE`). Проект на раннем этапе: план работ — `docs/plan.md`,
архитектура — `docs/architecture.md`, интерфейс — `docs/interface-design.md`.

## Сборка

Нужны CMake ≥ 3.28, Ninja, компилятор C++20 (MSVC 2022+, GCC 13+, Apple Clang 15+), vcpkg
(переменная окружения `VCPKG_ROOT`) и Python ≥ 3.12 для тестов. На Windows команды запускаются из
Developer PowerShell / x64 Native Tools for Visual Studio: там уже есть `cl`, Ninja и `VCPKG_ROOT`.

```bash
python -m pip install -r python/requirements-dev.txt   # pytest, ruff, black, clang-format, Ninja

cmake --preset core-debug          # ядро без Qt GUI и VTK
cmake --build --preset core-debug
ctest --preset core-debug
python -m pytest

cmake --preset full-debug          # всё, включая оболочку Qt (первая сборка Qt через vcpkg долгая)
```

Пресеты: `core-debug`, `core-release`, `full-debug`, `full-release`. Каталоги сборки — `build/<пресет>/`.

Если путь к профилю пользователя Windows содержит не-ASCII символы (например, `C:\Users\Дмитрий`), задайте
переменной `VCPKG_DOWNLOADS` путь только из ASCII, например `D:\vcpkg-downloads`. Иначе vcpkg распаковывает
свои инструменты в профиль, и порты, собираемые через meson (toml++), падают на этапе настройки.

## Проверки стиля

```bash
clang-format --dry-run --Werror <файлы .h/.cpp>
ruff check .
black --check .
```
