# Видимость по динамике: разбор libunity.so и почему смещений в нём нет

Задача: проверка видимости должна учитывать постройки игроков, а они спавнятся
в рантайме — в статическом меше карты их нет. Первый вариант решения «снять
нативную раскладку `Renderer`/`Collider` из `libunity.so`, который лежит в корне
репо» был проверен и **не работает**. Здесь — что именно проверено, чем
воспроизвести, и на чём вместо этого собрана динамика.

## Что в libunity.so есть и чего нет

Файл: `libunity.so`, 25 030 176 байт, AArch64 `DYN`.

```
readelf -h libunity.so
readelf -SW libunity.so          # .rodata@0xdef80  .text@0x5a79f0  .data.rel.ro@0x1777470
readelf --dyn-syms -W libunity.so | wc -l     # 371 строка -> ~185 символов
```

* **185 динамических символов**, и все они — импорты libc/NDK (`ANativeWindow_*`,
  `AImageReader_*`, `__android_log_print`) плюс четыре экспорта
  (`JNI_OnLoad`, `JNI_OnUnload`, `UnityInitialize`, `UnitySendMessage`,
  `UnitySwappy_*`). Внутренних функций Unity нет ни одной.
* **RTTI снят целиком**: `strings -a libunity.so | grep -cE '^_ZTI|^_ZTS'` → 0.
  Значит нет ни typeinfo-имён, ни связанных с ними vtable-якорей.
* **Версия Unity не определяется**: единственная версия-подобная строка —
  `2018.3.0a1` (заглушка), строки `PhysX <ver>` нет. PhysX при этом статически
  влинкован (пути исходников `./physx/source/geomutils/src/GuBV32.cpp`,
  `GuBVHStructure.cpp`, `GuHeightField.cpp` — то есть мешевые коллайдеры и
  heightfield внутри `.text` есть, но без символов).
* **Имена нативных классов в `.rodata` есть**, каждое в одном голом экземпляре
  (NUL с обеих сторон): `GameObject` 0x11abfa, `MeshRenderer` 0x12255d,
  `BoxCollider` 0x14e625, `SkinnedMeshRenderer` 0x14e0b3, `Renderer` 0x163f52,
  `Collider` 0x17352b, `MeshCollider` 0x17aac1, `MeshFilter` 0x181949,
  `Transform` 0x1ba57b, `GameObject` 0x1d3488, `Transform` 0x1d3750.
* **В `.data.rel.ro` нет ни одного указателя на эти строки** — то есть таблицы
  «RTTI-name → vtable» не существует, привязать класс к vtable нечем.

## Куда ведут имена классов

Каждое имя разыскано по коду: найдены все пары `ADRP`+`ADD`, которые
материализуют его адрес (воспроизводится `tools/unity_rev/scan_class_strings.py`,
см. ниже). Все сайты — **name-based** доступ, не раскладка:

| Адрес сайта | Что за код |
|---|---|
| 0x76a6e0…0x76a910 (6 шт.) | `switch` по типу примитива: грузит тройку имён (`MeshFilter` + `CapsuleCollider`/`MeshCollider`/… + `MeshRenderer`) и зовёт 0x758abc — добавление компонентов по имени. Рядом строка `"New-Cylinder.fbx"` → это `GameObject::CreatePrimitive`. |
| 0x952d78 | Регистрация скриптового класса: `x0="UnityEngine.CoreModule.dll"`, `x1="UnityEngine"`, `x2="MeshRenderer"` → `bl 0x880af8`, результат в глобал `0x1863000+0x570`. |
| 0x95ff14 | Там же, второй проход: `x2="MeshRenderer"`, `x3="DontStripMeshRenderer"` → `bl 0x880e20`. |

Ни в одном сайте нет обращения к полям объекта (`ldr/str [xN, #off]` от
«this»): все они работают со **строкой имени**. Полевые смещения
`Renderer::m_AABB`, `Collider::*`, `MeshFilter::m_Mesh` живут в сгенерированных
биндингах **libil2cpp.so** (`MeshRendererProxy::get_bounds` и подобные — эти
имена в `.rodata` присутствуют как `::Scripting::UnityEngine::MeshRendererProxy`),
а самого `libil2cpp.so` в репо нет.

**Вывод:** из `libunity.so` смещения полей не снимаются. Любое число, «снятое»
отсюда, было бы догадкой, а угаданные смещения в этот проект не попадают —
все существующие смещения получены из дампов (`dump.cs`) или эмпирическим сканом с
проверкой на устройстве.

## Инструменты (воспроизведение)

Сандбоксный `objdump` — x86-only (`can't use supplied machine aarch64`),
`capstone` недоступен, поэтому дизассемблер написан под задачу:

```
# все сайты ADRP+ADD для строки + указатели в .data.rel.ro/.data/.got
python3 tools/unity_rev/scan_class_strings.py libunity.so MeshRenderer

# узкий aarch64-дизассемблер: ADRP/ADD (с подстановкой строки из .rodata),
# LDR/STR/LDP/STP, MOVZ/MOVK/MOV, BL/B/CBZ/CBNZ/TBZ/TBNZ/B.cond/RET, CMP.
# Всё прочее печатается как .word — мнемоника не выдумывается.
python3 tools/unity_rev/adis.py libunity.so --range 0x952d70 0x952db0
python3 tools/unity_rev/adis.py libunity.so --func 0x76a6f0
python3 tools/unity_rev/adis.py libunity.so --string MeshRenderer
```

## На чём динамика собрана вместо этого

Без единого нового смещения в памяти — только то, что проект уже читает:

* **Список кусков**: `Mirror.NetworkClient.spawned` (`resolve_network_client_spawned`,
  `game_markers.cpp`) → `NetworkIdentity.NetworkBehaviours[]` → компонент →
  `managed_object_native()` → `native_component_transform()`. Тот же обход, что
  у маркеров.
* **Мировой TRS куска**: `read_transform_hierarchy_layout()` даёт и позицию, и
  **мировой поворот** (`Vec4`) — раскладка `g_skeleton_layout` уже находится
  эмпирическим сканом с проверкой (`skeleton_layout_reads_plausible`).
* **Имя префаба**: `read_transform_name()` (+ обход детей через
  `collect_transform_subtree`, геометрия часто на LOD-потомке).
* **Геометрия**: префаб как `.obj` в библиотеке `<CfgDir>/pieces/` — экспорт на
  ПК тем же AssetRipper, что и меш карты. Кусок инстансится в мир
  (`vis::TransformTri`) и складывается во второе BVH (`vis_dyn::build` →
  `vis_map::rebuild_dynamic`).

Сопоставление имени объекта с префабом — три ступени (`vis_dyn::Library::Find`):
точный ключ (токены отсортированы, порядок слов не важен), подмножество токенов
(«Foundation Wood Damaged» → `WoodFoundation.obj`), подстрока в склейке токенов
в исходном порядке («WoodWall_LOD0» → `woodwall.obj`, когда имя файла одним
токеном). Среди кандидатов побеждает самый длинный ключ. Проверяется на ПК без
устройства:

```
g++ -O2 -std=gnu++17 -I jni/src tools/vis_map/obj2bvh.cpp -o obj2bvh
./obj2bvh --check-pieces pieces/ "WoodWall_LOD0" "Foundation Wood Damaged"
```

## Что разблокировало бы путь «габарит из нативного Renderer»

Три варианта, в порядке убывания надёжности:

1. **`dump.cs`** (есть на Drive в `dump11949.7z` / `dumpbeta11957.7z`, 64 МБ):
   поля managed-классов `UnityEngine.BoxCollider` (`m_Center`, `m_Size`),
   `MeshFilter.m_Mesh`, `Building.BuildingPiece` и менеджера построек. Тогда
   габарит читается напрямую и библиотека `.obj` не нужна вовсе.
2. **`libil2cpp.so`** того же билда: по строкам `MeshRendererProxy` находятся
   биндинги, а в них — смещения нативных полей. Файл лежит в APK рядом с
   `libunity.so`, в репо его нет.
3. **Эмпирический скан на устройстве** (так проект уже находит раскладку
   иерархии трансформов): перебор `off = 0x20..0x200 step 4` на нативном
   `MeshRenderer` собственного тела игрока, кандидат годен, если два подряд
   `Vec3` конечны, полуоси в 0.05..2 м, центр в 1.5 м от таза, а `Renderer.bounds`
   содержит мировые кости. Без якоря скан бессмыслен, якорь — скелет.

## Рантайм-дампер мешей (`jni/src/mesh_dump.h` + `game_markers.cpp`)

Цель: получать `.obj` построек (и статики) из памяти запущенной игры, без
ручного экспорта AssetRipper на ПК под каждый билд. Дампер пишется прямо в
`vis_map::pieces_dir()`, поэтому библиотека видимости подхватывает файлы после
перезапуска — замкнутый цикл.

**Версия Unity (авторитетно): `6000.3.18f1`, ревизия `5ebeb53e4c07`** — из
секции `.note.unity` файла `libunity.so`:

```
readelf -x .note.unity libunity.so     # -nW обрезает строку, нужен именно -x
```

Строка `2018.3.0a1`, которую тоже находят в `.so`, — декай/наследие шейдеров,
не версия движка. `.note.android.ident`: NDK r27c, build-id `e1d6f1af14874ce8`.

**Почему раскладку ищем эмпирически, а не берём из `dump.cs`/`libunity.so`:**
смещения нативных полей `Mesh` (указатели VBO/IBO, число вершин, шаг, размер
индекса) живут в `libil2cpp.so` (в репо его нет) и в метаданных il2cpp; RTTI в
`libunity.so` вычищен, символов PhysX/Gu нет. `dump.cs` (64 МБ, `dump11949.7z`)
в этой песочнице не распаковать. Статически `libunity.so` — тупик, не повторять.

**Подход (`mesh_dump.h`, std-only, покрыт `jni/tests/test_mesh_dump.cpp`):**

- `Layout` — смещения полей VBO/IBO от базы нативного `Mesh` (цель
  `m_CachedPtr` managed-объекта): `vbuf_ptr_off`, `vcount_off`,
  `vstride_off`/`vstride_fixed`, `vpos_sub_off`, `ibuf_ptr_off`, `icount_off`,
  `isize_off`/`isize_fixed`.
- `Reader{ bool(*read)(ctx,addr,out,size); void* ctx; }` — на устройстве
  `rd_buf`/`rd_ptr` (внешний транспорт), на хосте — «арена» (addr == index).
  Никаких вызовов внутри процесса игры.
- `DiscoverLayout` — двухфазный, ограниченный поиск. Фаза 1: поля-указатели на
  кучу → (vcount, stride), **слой A** — stride читается из поля рядом с vcount
  (так у настоящего VBO Unity), **слой B** — перебор фикс. шагов `kStrideSet`
  только если поля не нашлось; короткая проверка `VerticesPlausible` (конечные
  координаты + полуоси) ДО чтения индексов; шорт-лист ≤ 96, кандидаты «из поля»
  раньше «угаданных». Фаза 2: указатели/счётчики индексов, `isize ∈ {4,2}`
  (u32 раньше u16), всё декодируется через `ApplyLayout`. Лимит
  `kMaxDecodeAttempts = 4096`. Порядок A-перед-B важен: угаданный шаг может дать
  косо выровненные, но конечные вершины, которые прошли бы валидацию.
- `ApplyLayout` — применить известную раскладку: быстрый путь после первого
  схождения И распознаватель «это нативный Mesh?» (дешёвые проверки полей
  vcount/stride/icount до чтения больших буферов).
- `DecodeTris`/`DecodeValid` — локальные треугольники; отбраковка по границам,
  несовместимому stride/isize, `icount % 3`, выходам индексов за диапазон,
  не-конечным координатам, полуосям вне `[kMinExtent, kMaxExtent]` и доле
  вырожденных треугольников > 0.5. Мусор не пишем никогда.
- `WriteObj` — `v`/`f` в локальных координатах (1-based, вершины на
  треугольник), шапка `# dumped at runtime`. `DedupKey` — ключ уникальности.

**Сторона устройства (`game_markers.cpp`, `vis_dump_meshes`):** возобновляемый
проход по `Mirror.NetworkClient.spawned` (тот же снимок реестра и helper'ы, что
у маркеров/`vis_dyn_collect`). Для каждой `NetworkIdentity`: behaviours →
`managed_object_native` → нативный `GameObject` (`+COMPONENT_GAMEOBJECT`) и
`Transform`; имя через `read_transform_name`; поддерево через
`collect_transform_subtree` (геометрия часто на LOD-потомке). Для каждого узла —
`gameobject_component_by_class(go, "MeshFilter")` (запасной `MeshRenderer`) и
скан его полей-указателей: каждый пробуем и как есть, и через
`managed_object_native` (поле `sharedMesh` хранит УПРАВЛЯЕМЫЙ `Mesh`, нативный
лежит по `m_CachedPtr`). Раскладка VBO кэшируется глобально после первого
схождения; дедуп по указателю нативного меша (`g_dump_seen`); лимиты
`kDumpMaxMeshes = 400`, бюджет на тик, `kDumpSubtreeNodes = 8`.
`vis_dump_reset()` чистит дедуп/счётчики при перезагрузке мира, раскладку
оставляет (она свойство сборки Unity). Кнопка «Дамп мешей» в карточке
«Видимость» (`menu.cpp`, `##vvis_dump`) запускает один тик (48 мешей / 0.35 с)
и показывает сводку `vis_dump_report` в строке и в тосте.

**Статус:** ядро + host-тест зелёные (`test_mesh_dump: all checks passed`, весь
набор `scripts/host_tests.sh`). `game_markers.cpp` компилируется чисто
(`-Wall -Wextra`, без новых предупреждений). **Прогон на устройстве не
проверялся** (в песочнице нет NDK/устройства): нужно одно подтверждение на
девайсе — что `DiscoverLayout` сходится на реальном `Mesh` этого билда и `.obj`
ложатся в `pieces_dir`.
