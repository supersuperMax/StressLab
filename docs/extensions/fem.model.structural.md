# Модуль `fem.model.structural`: типы прочностной модели и результатов

Встроенный модуль без кода (`extensions/fem.model.structural/`). Он задаёт типы объектов Model IR для линейной
статики и Results IR: подписи в дереве, формы свойств (`schemas/*.json`), переводы en/ru. Объекты этих типов
создают и читают другие модули: импорт `.inp` (3.2), экспорт (3.3), адаптер решателя (3.5), чтение результатов
(3.6), просмотрщик (3.7). Решения и причины — `docs/adr/0023-structural-model-and-results-ir.md`.

У каждого объекта `owner = "fem.model.structural"`, `schemaVersion = 1`. Значения с единицами (`x-quantity` в
схеме) хранятся в системе единиц документа (`Document::units`), без пересчёта. Ссылки между объектами — это
**зависимости** модели (`dependencies`, `model.add_dependency`), а не свойства; роль цели определяется её типом.
Пока модуль выключен, его объекты остаются в модели непрозрачными и сохраняются без изменений.

Образцы допустимых и недопустимых свойств каждого типа — `tests/data/structural/samples.json`.

## Модель

### Где объекты в дереве

```
material "Steel"                      корень
solid_section / shell_section / beam_section   корень; зависят от element_set и material
boundary_condition "clamp"            корень: действует во всех шагах; зависит от node_set
static_step "Step-1"                  корень; шаги выполняются в порядке создания (id)
├── boundary_condition                действует с этого шага
├── concentrated_load                 зависит от node_set
├── pressure                          зависит от surface или element_set
├── gravity                           зависит от element_set или ни от чего (вся модель)
└── output_request                    зависит от node_set/element_set или ни от чего (вся модель)
mesh (fem.core) ── node_set / element_set / surface (fem.core)
```

Ядро не проверяет, что родитель и зависимости соответствуют этой таблице: это делают модули, которые читают
модель (проверки этапов, задача 3.9; `validate()` решателя, 3.5).

### Типы

| Тип | Свойства | Зависимости |
|---|---|---|
| `material` | `youngsModulus`* (pressure, > 0), `poissonsRatio`* (−1 < ν < 0.5), `density` (density, ≥ 0) | — |
| `solid_section` | `integration`: `full` \| `reduced` (по умолчанию `full`) | element_set, material |
| `shell_section` | `thickness`* (length, > 0), `integrationPoints` (≥ 1, 5), `offset` (доля толщины, 0), `integration` | element_set, material |
| `beam_section` | `shape`*: `rectangle` \| `circle` \| `pipe`; `width`, `height` (прямоугольник); `radius` (круг, труба); `wallThickness` (труба) — length, > 0; `direction` {x, y, z} — примерная первая ось сечения, по умолчанию (0, 0, −1) | element_set, material |
| `static_step` | `timePeriod` (1.0), `initialIncrement` (1.0), `maxIncrements` (100) — псевдовремя, без единиц | — |
| `boundary_condition` | `ux`, `uy`, `uz` (length), `rx`, `ry`, `rz` (angle): заданное значение; **нет свойства — степень свободы свободна**, 0 — закреплена | node_set |
| `concentrated_load` | `fx`, `fy`, `fz` (force), `mx`, `my`, `mz` (moment), по умолчанию 0; на каждый узел набора | node_set |
| `pressure` | `magnitude`* (pressure): положительное давление действует против внешней нормали поверхности, на оболочках — против положительной нормали элемента | surface или element_set (оболочки) |
| `gravity` | `acceleration`* (acceleration, ≥ 0), `direction` {x, y, z} (длина не важна, по умолчанию (0, 0, −1)) | element_set или нет |
| `output_request` | `displacement`, `reactionForce`, `stress` (true), `strain` (false); `destination`: `results` (файл результатов) \| `table` (текстовый вывод); `frequency` (каждый n-й инкремент, 1) | node_set/element_set или нет |

\* — обязательное свойство.

**Формулировка элемента** следует из вида сечения, которое на него назначено: `solid_section` — объёмные элементы
(`tet4/10`, `hex8/20`, `wedge`, `pyramid`), `shell_section` — оболочки (`tri3`, `quad4`…), `beam_section` —
балки (`line2`…). Редуцированное интегрирование — `integration = "reduced"` сечения. Так `tet10` под
`solid_section` — это C3D10, `quad4` под `shell_section` с `reduced` — S4R, `line2` под `beam_section` — B31.

## Results IR

Результаты расчёта не хранятся в модели и истории проекта (ADR 0021): модуль, читающий файлы решателя, строит
из них **отдельный `fem::model::Document`** — документ результатов. Его система единиц — та, в которой
записаны значения.

```
mesh "Part-1" (fem.core)               сетка, на которой заданы результаты
result_step "Step-1"                   number, procedure = "static"
└── result_frame "Increment 1"         increment, time (время шага)
    ├── result_field "U"               зависит от mesh; данные ArrayData
    ├── result_field "S"
    └── ...
```

| Тип | Свойства (все только для чтения) |
|---|---|
| `result_step` | `number`* (≥ 1), `procedure`*: `static` |
| `result_frame` | `increment`* (≥ 0), `time` |
| `result_field` | `position`*: `nodal` \| `element` \| `integrationPoint` \| `elementNodal`; `components`* — имена компонент, например `["U1", "U2", "U3"]`; `quantity` — id величины каталога (`length`, `pressure`, `force`, `strain`…), пусто для значений без величины |

Имя поля — как у решателя (`U`, `S`, `E`, `RF`). Данные поля — `ArrayData`, у всех массивов одно число строк:

| Позиция | Массивы |
|---|---|
| `nodal` | `node` int32 — индекс узла сетки; `values` float64 ширины `len(components)` |
| `element` | `element` int32 — глобальный индекс элемента (как у наборов: `Mesh::block_offset(b) + k`); `values` |
| `integrationPoint` | `element` int32; `point` int32 — номер точки интегрирования с 1; `values` |
| `elementNodal` | `element` int32; `node` int32 — номер узла в элементе с 0, в нейтральной нумерации; `values` |

Индексы — позиции в массивах сетки документа результатов, не метки узлов и элементов. Строки не обязаны быть
отсортированы и покрывать всю сетку (вывод по набору).

В Python:

```python
import numpy as np, stresslab

data = stresslab.ArrayData({"node": np.arange(n, dtype=np.int32), "values": u})  # u: (n, 3)
ctx.create_object("result_field", "fem.model.structural", name="U", parent=frame,
                  dependencies=[mesh], data=data,
                  properties={"position": "nodal", "components": ["U1", "U2", "U3"],
                              "quantity": "length"})
field.data["values"]   # (n, 3), numpy только для чтения, без копии
```
