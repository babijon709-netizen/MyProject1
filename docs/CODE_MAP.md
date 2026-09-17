# Карта кода

Проект — один исполняемый файл, но исходники разложены по модулям: каждая
задача (меню, аим, автофарм, ESP, работа с памятью игры) живёт в своём файле,
а не в общем монолите. Раньше было два файла по несколько тысяч строк:
`jni/src/game.cpp` (7144) и `jni/src/main.cpp` (6117) — теперь это наборы
модулей с заголовками, а в монолитах остались только точка входа и общая шапка.

## Куда идти с правкой

| Что нужно | Файл |
| --- | --- |
| поменять цвета, тёмную тему, палитру | `jni/src/ui/theme.cpp` |
| добавить/поправить строку меню, слайдер, карточку | `jni/src/ui/widgets.cpp` |
| содержимое вкладок (Аим, ESP, Разное, Конфиги, Опции) | `jni/src/ui/tabs.cpp` |
| окно меню целиком: полоса вкладок, светофор, аватарка, перетаскивание | `jni/src/ui/window.cpp` |
| нижняя шторка и кнопки выхода | `jni/src/ui/sheet.cpp` |
| всплывающее окно и его содержимое | `jni/src/ui/popover.cpp` |
| прокрутка панелей, положение и размер окна | `jni/src/ui/scroll.cpp` |
| всплывающие подсказки (тосты) | `jni/src/ui/toast.cpp` |
| подписи поверх игры (пилюли, счётчик врагов) | `jni/src/ui/watermark.cpp` |
| настройки ESP и аима (cfg::esp, cfg::aim), прозрачность меню | `jni/src/ui/settings.cpp` |
| конфиги: сохранение, загрузка, слежение за каталогом | `jni/src/ui/config.cpp` |
| строки интерфейса РУ/EN (XOR-таблица) | `jni/src/ui/xp.h` |
| рисование рамок, скелетов и маркеров поверх игры | `jni/src/ui/esp_overlay.cpp` |
| аимбот: точка прицела, чувствительность, палец | `jni/src/aim/controller.cpp` |
| аимбот: сам такт — доводка камеры, обучение коэффициента, выбор режима Тач/Мемори | `jni/src/aim/update.cpp` |
| мемори-аим: шаг доворота по абсолютным углам (мёртвая зона, доля ошибки) | `jni/src/aim/memory.cpp` |
| мемори-аим: где игра держит поворот и как в него писать (подбор дорожки замером) | `jni/src/esp/aim_mem.cpp` |
| автофарм: фазы, удар, обход узлов, разбор крестика | `jni/src/farm/controller.cpp` |
| автофарм: цифры для строки статуса в окне | `jni/src/farm/state.cpp` |
| звуки меню (щелчок, подтверждение) | `jni/src/app/audio.cpp` |
| поиск процесса игры и поток привязки | `jni/src/app/attach.cpp` |
| журнал здоровья: почему чит «выключился», что делал до этого | `jni/src/app/diag_log.cpp` |
| окно оверлея: живучесть (пересоздание поверхности, ошибки кадра) | `jni/src/Android_draw/draw.cpp` |
| иконки вкладок | `jni/src/app/media.cpp` |
| размеры экрана, центрирование окна меню | `jni/src/app/screen.cpp` |
| флаг «программа работает», остановка потоков | `jni/src/app/lifecycle.cpp` |
| точка входа: порядок кадра меню | `jni/src/main.cpp` |
| чтение памяти игры (rd/rd_ptr/wr_buf) | `jni/src/esp/mem.cpp` |
| чтение строк игры (managed/il2cpp) | `jni/src/esp/managed.cpp` |
| имена игроков и их чистка | `jni/src/esp/names.cpp` |
| таблица имён оружия (перевод подписи) | `jni/src/esp/weapons.cpp` |
| поиск сущностей и игрока в памяти | `jni/src/esp/transform.cpp` |
| положение игрока, присед, гора/лошадь | `jni/src/esp/player_pose.cpp` |
| скелеты: кости, постройка из ragdoll | `jni/src/esp/skeleton_build.cpp` |
| скелеты: имена и поиск костей | `jni/src/esp/skeleton_names.cpp` |
| камера: матрицы, углы, разворот в экран | `jni/src/esp/camera.cpp` |
| рамки игроков (esp_get_boxes) | `jni/src/esp/boxes.cpp` |
| маркеры: руда, животные, лут, ящики | `jni/src/esp/markers.cpp` |
| подписи и сортировка маркеров | `jni/src/esp/marker_labels.cpp` |
| аим-точки на скелете (голова/грудь/таз) | `jni/src/esp/aim_points.cpp` |
| ближний бой: дальность орудия, луч | `jni/src/esp/melee.cpp` |
| цель автофарма: скан реестра узлов | `jni/src/esp/farm_scan.cpp` |
| цель автофарма: разбор узла и крестика | `jni/src/esp/farm_target.cpp` |
| кадр ESP: порядок и кэш кадра | `jni/src/esp/frame.cpp` |
| xray и вечный день (запись в игру) | `jni/src/esp/game_patch.cpp` |
| математика: векторы, матрицы, мир→экран | `jni/src/esp/math.cpp` |
| разрешение классов il2cpp | `jni/src/esp/il2cpp.cpp` |

## Как это собирается

* `jni/Android.mk` подхватывает модули маской (`src/esp/*.cpp`,
  `src/{app,ui,aim,farm}/*.cpp`) — новый файл в этих каталогах попадает в
  сборку сам, перечислять его не нужно;
* заголовок модуля — то, что модуль отдаёт наружу (типы, данные, функции);
  всё остальное внутри `.cpp` помечено `static` и наружу не течёт;
* общий заголовок меню и ESP — `jni/src/app/common.h` и `jni/src/esp/common.h`.

## Проверки после правки

```sh
sh tools/hostcheck/run.sh      # синтаксис и типы всех модулей (хостовый g++)
sh tools/syntax/check.sh       # то же с заглушками Android-заголовков
sh tools/lang/run.sh           # таблицы перевода и переключатель языка
sh tools/farm/run.sh           # стенд автофарма (настоящий код контроллера)
sh tools/aim/run_mem.sh        # стенд мемори-аима (настоящий код aim/memory.cpp)
```

## ESP (было `game.cpp`)

Чтение памяти игры и всё, что из неё достаётся: сущности, скелеты, камера,
маркеры, цель автофарма. Точка входа публичного API — `jni/include/game.h`
(`esp_init`, `esp_get_boxes`, `esp_get_markers`, `esp_farm_get_target`, …).

### jni/src/esp/aim_mem.cpp — Мемори-аим: поворот прицела записью в память игры

Строк: 752. Заголовок: `aim_mem.h`.

Внутри: `forward_from_angles`, `angles_from_forward`, `wrap180`, `signed_angle_diff`, `quaternion_between`, `quaternion_inverse`, `read_current_angles`, `read_mouse_look_floats`, `to_stored_yaw`, `path_apply`, `scan_state_fields`, `find_look_root`, `save_probe_value`, `restore_probe_value`, `probe_path_at`, `probe_finish_unsupported`, `probe_accept`, `probe_begin_candidate`, `probe_apply_current`, `probe_next_candidate`, `probe_undo`, `esp_mem_aim_reset`, `esp_mem_aim_tick`, `esp_mem_aim_state`, `esp_mem_aim_path`, `esp_mem_aim_reason`, `esp_mem_aim_read_angles`, `esp_mem_aim_apply`

Что тут важно: ни одна дорожка записи не «угадывается» — самотест делает
пробный доворот (yaw 2°, тангаж 1°), ждёт применения и меряет настоящий поворот
прицела; дорожка принимается только если доворот совпал с допуском 0.6° и не
поехал дальше. Порядок проверки: кватернион в MouseLook, пара углов (градусы и
радианы), поле накопленного ввода игры (+0x88/+0x8C), узел прицела в иерархии
Transform. Самотест ограничен по времени (6 с): не нашлось — режим «Мемори»
объявляет отказ с причиной, и аим продолжает работать тач-веткой.


### jni/src/esp/aim_points.cpp — Точки прицела: голова/шея/грудь и локальный ADS

Строк: 369. Заголовок: `aim_points.h`.

Внутри: `head_lift_for_range`, `set_aim_point`, `fill_skeleton_box`, `read_local_aim_state`, `esp_local_player_is_aiming`


### jni/src/esp/boxes.cpp — esp_get_boxes: рамки, имена, скелет

Строк: 621. Заголовок: `boxes.h`.

Внутри: `esp_get_boxes`, `esp_nearby_player_count`, `esp_wants_reattach`


### jni/src/esp/camera.cpp — Камера игры: поза, матрицы, углы, чувствительность

Строк: 426. Заголовок: `camera.h`.

Внутри: `esp_read_look_sensitivity`, `read_camera_transform_pose`, `read_native_camera_matrices`, `w2s_transform_camera`, `optimize_matrix_configuration`, `read_configured_player_transforms`, `esp_camera_fov_deg`, `esp_camera_state`, `read_local_aim_reference`


### jni/src/esp/farm_scan.cpp — Автофарм: скан узлов реестра

Строк: 449. Заголовок: `farm_scan.h`.

Внутри: `farm_kind_from_loot`, `farm_scan_abort`, `farm_scan_reset`, `farm_classify_identity`, `farm_scan_tick`, `esp_farm_set_resources`, `esp_farm_set_range`, `esp_farm_blacklist`, `farm_spot_on_node`, `farm_resolve_extension`


### jni/src/esp/farm_target.cpp — Автофарм: выбор узла и точка удара

Строк: 738. Заголовок: `farm_target.h`.

Внутри: `esp_farm_spot_raw`, `farm_read_spot`, `esp_farm_debug`, `esp_farm_tool_info`, `ray_hit_in_node_subtree`, `ray_hit_is_self_node`, `ray_hit_is_self_node`, `farm_ore_anchor_point`, `esp_farm_get_target`


### jni/src/esp/frame.cpp — Кадр ESP: состояние, публикация, сброс

Строк: 360. Заголовок: `frame.h`.

Внутри: `frame_note_published`, `frame_drop_unpublished`, `world_reloading`, `farm_cam_source_ok`, `angles_from_forward`, `esp_camera_angles`, `esp_aim_camera_angles`, `esp_local_eye_position`, `reset_world_caches`, `esp_reset`, `publish_camera_only_frame`

Что тут важно: кадр камеры больше не гасится на входе (раньше флаги
`g_frame_vp_valid`/`g_frame_local_valid` сбрасывались в начале `esp_get_boxes`, и
любой сбой чтения указателя камеры или матриц оставлял экран пустым ровно в этот
кадр — это и было «мерцание»). Теперь кадр помечается опубликованным
(`frame_note_published`) при реальной публикации, а при неудаче прошлый кадр
держится ещё 0.35 с (`frame_drop_unpublished`): камера за это время не уезжает, а
мигания нет. `world_reloading()` даёт окно в 1 с после `reset_world_caches` — в
нём «позиции не читаются» и «все игроки в одной точке» не считаются приговором
смещению, иначе каждый респавн начинался с перепоиска смещения без боксов.


### jni/src/esp/game_patch.cpp — Запись в память игры: X-ray и «всегда день»

Строк: 178. Заголовок: `game_patch.h`.

Внутри: `esp_set_xray`, `xray_apply`, `esp_set_always_day`, `always_day_tick`


### jni/src/esp/il2cpp.cpp — Il2Cpp: базовые адреса, классы, статические поля

Строк: 212. Заголовок: `il2cpp.h`.

Внутри: `class_looks_alive`, `class_identity`, `base_resolves_game`, `validate_player_list`, `player_list_contains`, `get_class_static_fields`, `resolve_runtime_player_list`, `resolve_local_player`, `resolve_native_transform`


### jni/src/esp/managed.cpp — Чтение managed-строк и коллекций

Строк: 121. Заголовок: `managed.h`.

Внутри: `remote_string_equals`, `read_managed_string_ex`, `read_managed_string`


### jni/src/esp/marker_labels.cpp — Маркеры: подписи и цвета сущностей

Строк: 525. Заголовок: `marker_labels.h`.

Внутри: `animal_look`, `marker_for_entity_type`, `for_each_name_token`, `animal_look_from_object_name`, `barrel_look_from_object_name`, `scan_loot_name`, `loot_marker`, `read_managed_collection`, `ore_look_for_item_name`, `marker_from_loot`, `pickup_label_for_item`, `gather_look_for_item_name`, `gather_marker_from_loot`, `pickup_marker`


### jni/src/esp/markers.cpp — Маркеры мира: скан реестра и выдача

Что тут важно: после смерти мир грузится, и позиции объектов (цепочка
Transform — несколько syscall'ов) не читаются секундами. Прежний код в этом
месте просто не рисовал маркер, и руда, ящики и трава пропадали вместе с
боксами. Теперь позиция держится до предела (1.5 с, а пока идёт перезагрузка
мира — 4 с) и перечитывается при первой возможности, а сам список сущностей при
сбросе кэшей НЕ выбрасывается: он остаётся на экране, пока новый цикл скана не
доедет (готовый список подменяет старый разом).

Строк: 576. Заголовок: `markers.h`.

Внутри: `esp_set_markers_enabled`, `esp_set_marker_max_distance`, `marker_scan_abort`, `marker_class_of`, `resolve_network_client_spawned`, `resolve_network_identity_class`, `marker_world_position`, `rebuild_marker_entities`, `reset_marker_caches`, `esp_get_markers`


### jni/src/esp/math.cpp — Вектор/кватернион/матрица и мировое→экранное

Строк: 155. Заголовок: `math.h`.

Внутри: `vec3_is_finite`, `cross_product`, `rotate_vector`, `multiply_quaternion`, `normalize_quaternion`, `matrix34_is_valid`, `mat_get`, `mat_set`, `matrix_is_finite`, `mat_mul`, `mat_perspective`, `mat_world_to_camera`, `camera_position_from_view`, `w2s`


### jni/src/esp/melee.cpp — Ближний бой: дальность удара и hitRadius

Строк: 149. Заголовок: `melee.h`.

Внутри: `read_local_melee_reach`


### jni/src/esp/mem.cpp — Доступ к памяти игры и состояние привязки

Строк: 140. Заголовок: `mem.h`.

Внутри: `rd_buf`, `rd`, `rd_exact`, `rd_ptr`, `rd_v3`, `rd_m4`, `wr_buf`, `esp_attach_state`, `esp_init`, `esp_mem_frame_begin`, `esp_alive_check`


### jni/src/esp/names.cpp — Имена игроков: что считать настоящим ником

Строк: 189. Заголовок: `names.h`.

Внутри: `looks_like_long_id`, `looks_like_generated_id`, `accept_display_name`, `valid_obj`, `read_name_source`, `read_player_group`, `groups_are_allied`, `player_display_name`, `read_item_data_display_name`, `fp_object_display_name`, `prune_player_text`


### jni/src/esp/player_pose.cpp — Позиция игрока: трек, скачки, «сидит/на маунте»

Строк: 442. Заголовок: `player_pose.h`.

Внутри: `player_aux`, `player_is_crouched`, `player_head_world`, `player_head_hitbox_world`, `prune_player_aux`, `mono_seconds`, `prune_player_track`, `vec3_horiz2`, `player_is_mounted`, `player_saved_position`, `player_rendered_position`, `player_model_position`, `player_mount_engaged`, `apply_mounted_position`, `… ещё 3`


### jni/src/esp/skeleton_build.cpp — Сборка скелета из ragdoll и по именам

Строк: 492. Заголовок: `skeleton_build.h`.

Внутри: `build_skeleton_from_ragdoll`, `build_skeleton_from_names`, `build_skeleton`, `skeleton_local_walk`


### jni/src/esp/skeleton_cache.cpp — Кеш скелетов и переключатели

Строк: 56. Заголовок: `skeleton_cache.h`.

Внутри: `esp_set_skeleton_enabled`, `esp_set_aim_bones_enabled`, `prune_skeleton_cache`


### jni/src/esp/skeleton_names.cpp — Скелет: имена костей, поиск скелета игрока, KCC/ragdoll

Строк: 370. Заголовок: `skeleton_names.h`.

Внутри: `read_transform_children`, `collect_transform_subtree`, `string_is_reasonable_name`, `normalize_bone_name`, `match_bone_name`, `read_gameobject_name_at`, `read_transform_name`, `discover_gameobject_name_offset`, `resolve_skeleton_layout`, `managed_object_native`, `skeleton_transform_ptr_valid`, `native_component_transform`, `kcc_head_transform_valid`, `looks_like_kcc`, `… ещё 5`


### jni/src/esp/transform.cpp — Иерархия Transform: где у объекта позиция и как её читать

Строк: 480. Заголовок: `transform.h`.

Внутри: `read_transform_hierarchy_arrays`, `read_transform_hierarchy_layout`, `read_transform_hierarchy_position`, `resolve_player_native_transform`, `likely_native_pointer`, `evaluate_transform_hierarchy_layout`, `discover_layout_from_native_transforms`, `discover_transform_hierarchy_layout`, `read_entity_position`, `read_entity_pose`, `position_looks_like_world_space`, `evaluate_player_position_offset`, `find_direct_player_position_offset`, `discover_player_position_offset`, `… ещё 1`


### jni/src/esp/weapons.cpp — Оружие: метка в руках игрока

Строк: 523. Заголовок: `weapons.h`.

Внутри: `canonical_weapon_label`, `player_weapon_name_raw`, `fix_weapon_label_spelling`, `player_weapon_name`, `normalize_weapon_token`, `weapon_name_is_junk`, `weapon_label_from_object_name`, `managed_component_gameobject_name`, `resolve_player_weapon_component`, `resolve_player_model_info`, `weapon_label_for_item_id`, `weapon_name_from_view`, `weapon_name_from_model_holders`, `remote_weapon_display_name`



## Меню, аим и автофарм (было `main.cpp`)

Интерфейс оверлея, конфиги, аимбот и автофарм. Точка входа — `jni/src/main.cpp`
(в нём только `main()`: инициализация, кадр меню, остановка потоков).

### jni/src/aim/controller.cpp — Параметры аима, чувствительность, палец

Строк: 153. Заголовок: `aim/controller.h`.

Внутри: `ColU32`, `AimFovRadiusPx`, `AimReleaseFinger`, `MonoNow`, `AimSensitivityScale`, `AimSensitivityGain`


### jni/src/aim/memory.cpp — Мемори-аим: такт доворота по абсолютным углам

Строк: 113. Заголовок: `aim/memory.h`.

Внутри: `AimMemoryStep`

В отличие от тач-аима здесь нет петли «палец -> камера -> экранная ошибка»
(квант ввода, обучение град/px, ожидание подтверждения). Контроллер получает
углы прицела и остаток ошибки и решает, куда поставить прицел: мёртвая зона
(3 см в мире цели), доля остатка за такт из слайдера «Скорость», потолок 12° за
кадр. Стенд с настоящим кодом этого файла — `tools/aim/run_mem.sh`.


### jni/src/aim/update.cpp — UpdateAim: один такт аимбота (режимы Тач и Мемори)

Что тут важно: режим «Мемори» не использует палец НИ В ОДНОЙ ветке — ни когда
дорожка записи найдена, ни во время самотеста, ни при отказе. Раньше при отказе
(«ни одна дорожка не подтвердилась») аим «на всякий случай» уходил в тач-ветку
ниже, и режим «Мемори» водил палец по экрану — то есть делал ровно то, от чего
этот режим и заводился. Теперь отказ остаётся отказом: палец отпущен, причина
написана во вкладке «Аим» («Через память нельзя — аим не работает»). Повторный
самотест запускается не по таймеру, а когда сменилось поколение мира
(g_world_reload_count): иначе на устройстве без записи аим каждые несколько
секунд дёргал бы прицел пробными доворотами. Ещё отсюда поднимается признак
AimIsDriving() — по нему автофарм уступает камеру и записывающему аиму (пальца у
него нет, и по одному s_fingerDown фарм не понял бы, что камера занята).

Строк: 697. Заголовок: `aim/update.h`.

Внутри: `UpdateAim`


### jni/src/app/attach.cpp — Поиск процесса игры и поток привязки

Строк: 299. Заголовок: `app/attach.h`.

Внутри: `TargetPackageA`, `TargetPackageB`, `cmdline_rank`, `pid_cmdline_matches`, `proc_libil2cpp`, `find_game_pid`, `find_unity_pid`, `pid_still_game`, `access_confirmed_lost`, `AttachStateName`, `start_attach_thread`, `stop_attach_thread`

Что тут важно: привязка больше не рвётся по одному неудачному чтению. Прежде
`esp_alive_check()` (одно чтение ELF-заголовка) мог мигнуть на нагрузке, после
чего вызывался `esp_reset()`, чит замолкал до следующей удачной привязки — со
стороны это выглядело как «чит сам выключился». Теперь потеря доступа
подтверждается: переоткрыть дескриптор (`rebind_now`), подождать 150 мс,
проверить снова — и только потом сбрасывать состояние, записав в журнал фазу
чтения, errno, счётчики и частоту кадров.


### jni/src/app/diag_log.cpp — Журнал здоровья: почему чит «выключился»

Строк: 231. Заголовок: `app/diag_log.h`.

Внутри: `write_locked`, `emit`, `crash_append`, `crash_append_uint`, `crash_append_hex`, `crash_handler`, `diag_init`, `diag_install_crash_handler`, `diag_self_stats`, `diag_enabled`, `diag_path`, `diag_log`

Что тут важно: это не отладочный вывод, а след для разбора живых жалоб («на
некоторых устройствах чит выключается сам через некоторое время», «ESP мерцает
после смерти»). Пишется всегда, в `<каталог конфигов>/xvcen_health.log`
(`/storage/emulated/0/benzhack/xvcen_health.log`), дорос до мегабайта — прошлый
уходит в `.1`. Формат строки: `секунды тег текст`; теги — `app`, `attach`,
`health`, `esp`, `touch`, `draw`. События: привязка к процессу (pid, база,
сборка), потеря доступа к памяти и что ей предшествовало (фаза чтения, errno,
счётчики), «чтение мигнуло — переоткрыли дескриптор», уход процесса игры,
перепривязка по сторожу, отказы тача и инъекции, пересборка окна оверлея,
«падение: сигнал N, адрес» (обработчик сигнала живёт здесь же и пишет в файл
через write(), без мьютекса и stdio), а раз в 30 с — «пульс»: привязано или нет,
частота кадров, сколько чтений и отказов, есть ли инъекция, память процесса,
число открытых дескрипторов, потоков и oom_score_adj. По пульсу видно, течёт ли
процесс и не собирается ли его выгрузить системный убийца памяти.


### jni/src/app/lifecycle.cpp — Признаки жизни процесса (main_thread_flag, g_frame_done)

Строк: 13. Заголовок: `app/lifecycle.h`.


### jni/src/app/media.cpp — Иконки вкладок: загрузка GL-текстур

Строк: 56. Заголовок: `app/media.h`.

Внутри: `LoadTexFromMemory`, `LoadTabIcons`, `LoadAnimeImage`


### jni/src/app/screen.cpp — Размеры экрана и центрирование окна меню

Строк: 50. Заголовок: `app/screen.h`.

Внутри: `VisibleScreen`, `CenterMenuOnDisplay`


### jni/src/farm/controller.cpp — UpdateFarm/UpdateFarmInner: контроллер автофарма

Строк: 891. Заголовок: `farm/controller.h`.

Внутри: `UpdateFarm`, `UpdateFarmInner`


### jni/src/farm/state.cpp — g_farm*: состояние автофарма для окна

Строк: 79. Заголовок: `farm/state.h`.


### jni/src/ui/config.cpp — Конфиги: файлы, XOR, слежение за каталогом

Строк: 460. Заголовок: `ui/config.h`.

Внутри: `CfgPath`, `CfgLastPath`, `RememberLastConfigName`, `ForgetLastConfigName`, `CfgLangPath`, `RememberLang`, `RestoreLang`, `CfgBuildPath`, `RememberBuild`, `RestoreBuild`, `ApplyBuildChoice`, `XorBuf`, `CfgScanDir`, `CfgWatchInit`, `… ещё 8`


### jni/src/ui/esp_overlay.cpp — Отрисовка ESP поверх игры

Что тут важно: FrameBoxes() держит прошлый НЕПУСТОЙ снимок, когда новый кадр
пришёл пустым, — до 0.7 с, а пока данные игроков не читаются
(g_player_data_stale) до 3 с. Раньше пустой кадр отдавался как есть, и вместе с
рамками гасли скелеты, подписи и цель аима: «ESP мерцает, часто после смерти».
FrameMarkers() держит маркеры по тому же правилу.

Строк: 394. Заголовок: `ui/esp_overlay.h`.

Внутри: `FrameBoxes`, `DrawEspOverlay`


### jni/src/ui/layout.cpp — Layout, AppState, ввод и анимации

Строк: 129. Заголовок: `ui/layout.h`.

Внутри: `GetRNG`, `Lerpf`, `Clamp01`, `EaseOut3`, `EaseInOut`, `WasTappedHere`, `TapInRect`, `PtInClip`, `SpringTick`, `Tick`, `AimTouchFracX`, `AimTouchFracY`


### jni/src/ui/popover.cpp — Всплывающее окно и его содержимое

Строк: 637. Заголовок: `ui/popover.h`.

Внутри: `PopoverOpenColor`, `PopoverOpen`, `PopoverClose`, `DrawPopoverContentFG`, `DrawPopover`


### jni/src/ui/scroll.cpp — Прокрутка панелей и состояние окна

Строк: 102. Заголовок: `ui/scroll.h`.

Внутри: `ScrollTick`, `IsScrollDragging`


### jni/src/ui/settings.cpp — Настройки ESP и аима (cfg::esp, cfg::aim, ui::bar)

Строк: 14. Заголовок: `ui/settings.h`.


### jni/src/ui/sheet.cpp — Нижняя шторка и кнопки выхода

Строк: 220. Заголовок: `ui/sheet.h`.

Внутри: `SheetOpen`, `SheetClose`, `DrawExitButtons`, `DrawSheet`


### jni/src/ui/tabs.cpp — TabContent: содержимое вкладок

Строк: 741. Заголовок: `ui/tabs.h`.

Внутри: `TabContent`


### jni/src/ui/theme.cpp — Палитра, тёмная тема, применение темы

Строк: 59. Заголовок: `ui/theme.h`.

Внутри: `ApplyTheme`


### jni/src/ui/toast.cpp — Всплывающие подсказки

Строк: 167. Заголовок: `ui/toast.h`.

Внутри: `ShowToast`, `DrawToast`


### jni/src/ui/watermark.cpp — Пилюли-подписи поверх игры

Строк: 102. Заголовок: `ui/watermark.h`.

Внутри: `DrawWatermark`


### jni/src/ui/widgets.cpp — Строки-переключатели, слайдеры, карточки

Строк: 242. Заголовок: `ui/widgets.h`.

Внутри: `DrawToggle`, `RenderToggleRowVisuals`, `RenderSliderVisuals`, `ToggleRow`, `TickSliderAnim`, `SliderRow`, `CardBg`, `SHdr`, `CollapsibleHeader`, `TickSlideAnim`


### jni/src/ui/window.cpp — RenderMenu: окно меню целиком

Строк: 906. Заголовок: `ui/window.h`.

Внутри: `TabTitle`, `RailTabsW`, `TrafficLightW`, `TrafficLightMetrics`, `AvatarRailW`, `AvatarR`, `AvatarCx`, `AvatarCy`, `AvatarBottom`, `TrafficLightRect`, `TrafficLightColor`, `MenuHeaderH`, `TrafficLightPos`, `TrafficLight`, `… ещё 1`
