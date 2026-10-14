# Протокол внепроцессных модулей

Версия протокола: **1**. Решения и причины — `docs/adr/0010-extension-host.md`.

Модуль вида `process` — исполняемый файл в папке модуля (`entry` в манифесте, на Windows можно без `.exe`).
Приложение запускает его в папке модуля и говорит с ним по этому протоколу через стандартные ввод и вывод.
Модули вида `python` пишутся на `stresslab_host` (см. «Python-модули» в конце): этот протокол за них реализует
хост, но он тот же.

## Транспорт

- JSON-RPC 2.0 (<https://www.jsonrpc.org/specification>), без пакетов (batch).
- Каждое сообщение — кадр как в Language Server Protocol: `Content-Length: <число байт>\r\n\r\n<JSON в UTF-8>`.
  Другие заголовки допустимы и игнорируются, имена заголовков — без учёта регистра.
- Стандартный вывод модуля — **только** кадры протокола. Всё остальное модуль пишет в стандартный вывод
  ошибок: приложение пишет его в свой лог построчно, последние строки попадают в отчёт о падении.
- Запросы идут в обе стороны. Пока сторона ждёт ответа, она обязана читать и обрабатывать входящие сообщения:
  приложение, выполняя команду модуля, присылает вложенные вызовы, а модуль, выполняя команду, вызывает
  приложение.
- Ошибки — коды JSON-RPC (`-32700`, `-32600`, `-32601`, `-32602`, `-32603`) и свои:

| Код | Смысл |
|---|---|
| `-32001` | `commandFailed`: код модуля или команда приложения сообщили об ошибке |
| `-32002` | `cancelled` |
| `-32004` | `notFound`: нет объекта, вызова или задачи |

Незнакомый метод — ответ `-32601`, незнакомое уведомление игнорируется.

## Жизненный цикл

1. Приложение запускает процесс и шлёт `initialize`. До ответа модуль регистрирует свои команды
   (`contributions/register`) и при желании объявляет ключи контекста.
2. Команды модуля приходят как `command/execute`.
3. При выключении модуля — запрос `shutdown` (модуль освобождает ресурсы и отвечает), затем уведомление `exit`
   и конец стандартного ввода. Модуль, который не завершился за 3 с, завершается приложением.

Конец стандартного ввода в любой момент значит, что приложение закрылось: модуль должен выйти.

### Здоровье модуля

- Приложение шлёт `$/ping` каждые 5 с; модуль, который не ответил (пустым объектом) за 10 с, считается
  зависшим. Поэтому модуль не блокирует чтение надолго: долгая работа идёт в отдельном потоке как задача, а
  поток чтения отвечает на ping (хост python-модулей так и делает).
- Команда, по которой модуль ничего не присылает (ни ответа, ни вложенных запросов) 30 с, считается
  зависшей. Долгая работа — задача (см. ниже), а не долгий ответ.
- Зависший модуль завершается вместе с процессами, которые он запустил. Упавший или завершённый модуль
  запускается заново, и его команда отменяется (изменения модели откатываются); после трёх остановок за
  5 минут он больше не перезапускается.

## Приложение → модуль

### `initialize` (запрос)

```json
{"protocolVersion": 1,
 "product": {"name": "StressLab", "version": "0.1.0"},
 "extension": {"id": "acme.solver", "version": "1.2.0", "kind": "process", "entry": "bin/solver",
               "folder": "C:/Users/…/acme.solver"},
 "sitePackages": []}
```

`sitePackages` — для python-модулей: папки зависимостей модуля, первыми в `sys.path`. Ответ:

```json
{"capabilities": {"sharedMemory": true}}
```

`sharedMemory: false` (или отсутствует) — модуль не умеет shared memory; тогда массивы от приложения приходят
списками JSON. Ошибка в ответе — модуль не активирован (текст ошибки видит пользователь).

### `command/execute` (запрос)

```json
{"call": 7, "command": "acme.solver.run", "args": {"job": "bracket", "threads": 4}}
```

Аргументы уже проверены по схеме из манифеста. `call` — номер вызова для вложенных запросов `call/*`. Ответ —
значение команды `{"value": …}` или задача `{"task": {"id": "t1", "title": "Расчёт"}}`. Значения: `null`,
`true`/`false`, целые, числа, строки, списки, объекты. Ошибка в ответе — команда не выполнена, все её
изменения модели откатываются.

### `task/cancel` (уведомление)

`{"task": "t1"}` — пользователь отменил задачу; модуль завершает её с `state: "cancelled"`.

### `context/changed` (уведомление)

`{"watch": 3, "value": true}` — изменилось значение выражения, на которое модуль подписан (`context/watch`).

### `model/changed` (уведомление)

После каждого шага, изменившего модель, если модуль подписан (`model/subscribe`):

```json
{"cause": "execute", "label": "model.create", "created": [12], "updated": [], "removed": [], "units": false}
```

`cause` — `execute`, `undo` или `redo`; `label` — id команды или название транзакции.

### `$/ping`, `shutdown` (запросы), `exit` (уведомление)

См. «Жизненный цикл».

## Модуль → приложение

### Регистрация

- `contributions/register` `{"commands": ["acme.solver.run", …]}` — у этих команд есть код. Только команды из
  манифеста; команда без кода при вызове отказывает.

### Ключи контекста

- `context/declare` `{"name": "acme.ready", "type": "boolean" | "number" | "string" | "list", "default": …}` →
  `true`, если объявлен сейчас (`false`, если уже был).
- `context/set` `{"name", "value"}` — только ключи этого модуля (из манифеста или объявленные).
- `context/evaluate` `{"when": "stage == 'solve'"}` → `true`/`false`.
- `context/watch` `{"when": …}` → `{"watch": 3, "value": false}`; дальше — уведомления `context/changed`.
- `context/unwatch` `{"watch": 3}`.

### Настройки

- `settings/get` `{"key": "acme.threads"}` → `{"value": …}` — значение в силе (по умолчанию < пользователь <
  проект) своей настройки модуля (`contributes.settings`) или любой другой известной приложению. Ключ без
  схемы — ошибка `-32004`. Модуль читает настройку, когда она нужна: уведомления об изменении нет.

### Модель

- `model/subscribe`, `model/unsubscribe` — уведомления `model/changed`.
- `document/objects` `{"type"?, "owner"?}` → ids объектов по возрастанию.
- `document/object` `{"id"}` → `{"id", "type", "owner", "name", "schemaVersion", "properties", "parent",
  "children", "dependencies", "opaque", "data"}`; `data` — вид данных (`mesh`, `nodeSet`, `elementSet`,
  `surface`) или `null`.
- `document/data` `{"id"}` → данные (см. «Данные») или `null`.

### Изменения модели — только внутри вызова

Эти запросы действуют, пока выполняется команда `call`, и входят в её шаг undo:

- `call/execute` `{"call", "command", "args"}` → `{"value"}` — выполнить другую команду как часть этой.
- `call/createObject` `{"call", "type", "owner", "name"?, "properties"?, "parent"?, "dependencies"?,
  "schemaVersion"?, "data"?}` → `{"id"}`.
- `call/setData` `{"call", "object", "data"}`.

Вне вызова модель меняется только командой целиком:

- `command/execute` `{"command", "args"}` → `{"value"}` — выполняется, когда приложение свободно (не внутри
  другой команды), отдельным шагом undo и строкой журнала.

### Задачи

После ответа `{"task": {"id": "t1", …}}` на `command/execute` (не раньше):

- `task/progress` `{"task", "progress": 0.0…1.0 | null}` (`null` — неизвестно, сколько осталось);
- `task/message` `{"task", "text", "severity": "info" | "warning" | "error"}`;
- `task/finished` `{"task", "state": "succeeded", "value"}`, `{"task", "state": "cancelled"}` или
  `{"task", "state": "failed", "error": "текст"}`.

Если модуль падает, его задачи завершаются с ошибкой.

### Shared memory

- `shm/release` `{"name"}` (уведомление) — модуль отобразил сегмент приложения; приложение удаляет имя, память
  остаётся у модуля.

## Данные

Сетка:

```json
{"kind": "mesh",
 "nodes": {"ids": A, "x": A, "y": A, "z": A},
 "blocks": [{"type": "hex8", "ids": A, "connectivity": A}]}
```

Наборы: `{"kind": "nodeSet", "nodes": A}`, `{"kind": "elementSet", "elements": A}`,
`{"kind": "surface", "sides": A}` (пары «индекс элемента, номер стороны» подряд). Типы элементов и нумерация —
как в Model IR (ADR 0002): узлы в `connectivity` и наборах — индексы с нуля, `ids` — метки.

Массив `A` — список JSON или ссылка на shared memory:

```json
{"shm": "slh1234_a1b2c3d4e5f6", "offset": 0, "dtype": "f8", "count": 1000000}
```

`dtype`: `f8` (double) — координаты; `i8` (int64) — метки; `i4` (int32) — индексы. Порядок байт —
little-endian, `offset` кратен 8.

- **Имя сегмента** — до 30 символов `[A-Za-z0-9_-]`. Windows: именованный file mapping (`CreateFileMappingW` /
  `OpenFileMappingW` с этим именем). macOS, Linux: `shm_open("/" + имя)`.
- **Модуль → приложение**: модуль создаёт сегмент, пишет массивы, шлёт запрос; приложение копирует данные,
  модуль удаляет сегмент после ответа (`shm_unlink` / закрыть отображение).
- **Приложение → модуль**: приложение кладёт все массивы ответа в один сегмент; модуль отображает его и шлёт
  `shm/release`. Модуль без `capabilities.sharedMemory` получает списки.

## Python-модули

Манифест: `kind = "python"`, `entry = "пакет.модуль:activate"`. Хост (`python -m stresslab_host`) ставит
первыми в `sys.path` папку модуля, `<папка модуля>/site-packages` и личный `site-packages` модуля в папке
пользователя; `PYTHONPATH` и пользовательский site-packages не действуют. `print` идёт в лог.

```python
from stresslab_host import Call, Extension, Mesh, TaskControl


def activate(extension: Extension) -> None:
    @extension.command("acme.solver.run")
    def run(call: Call, job: str, threads: int = 0):
        mesh = call.data(call.objects(type="mesh")[0])     # Массивы — numpy, только чтение.

        def work(control: TaskControl) -> dict:
            for step in range(100):
                control.check_cancelled()
                control.set_progress(step / 100)
            return {"job": job}

        return call.run_task("Расчёт " + job, work)       # Задача стартует после ответа.


def deactivate() -> None:                                  # Необязательно: при выключении.
    ...
```

Изменения модели — через `call.create_object`, `call.set_data`, `call.execute` (часть команды) или
`extension.execute` (команда сама по себе, например из задачи). Ключи — `extension.context`, изменения модели —
`extension.on_model_changed`.
