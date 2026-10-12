<h1>
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="resources/branding/stresslab-kit/logo/svg/stresslab-horizontal-reversed.svg">
    <img alt="StressLab" src="resources/branding/stresslab-kit/logo/svg/stresslab-horizontal-color.svg" height="64">
  </picture>
</h1>

StressLab — кросс-платформенный пакет конечно-элементного анализа с модульной оболочкой в стиле VS Code.
Ядро распространяется под лицензией MIT (см. `LICENSE`). Проект на раннем этапе: план работ — `docs/plan.md`,
архитектура — `docs/architecture.md`, интерфейс — `docs/interface-design.md`. Логотип, иконки и правила их
использования — в `resources/branding/stresslab-kit/` (`guidelines.md`).

## Сборка

Нужны CMake ≥ 3.28, Ninja, компилятор C++20 (MSVC 2022+, GCC 13+, Apple Clang 15+), vcpkg
(переменная окружения `VCPKG_ROOT`) и Python 3.12 с файлами для разработки: он встраивается в программу
(задача 1.8), на нём же идут тесты; CMake находит его сам или по `Python_ROOT_DIR`. На Windows команды запускаются из
Developer PowerShell / x64 Native Tools for Visual Studio: там уже есть `cl`, Ninja и `VCPKG_ROOT`.

```bash
python -m pip install -r python/requirements-dev.txt   # nanobind, numpy, pytest, ruff, black, clang-format, Ninja

cmake --preset core-debug          # ядро без Qt GUI и VTK
cmake --build --preset core-debug
ctest --preset core-debug          # включает pytest в обычном Python и во встроенном
python -m pytest
build/core-debug/app/stresslab --python -m pytest   # то же внутри программы

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
