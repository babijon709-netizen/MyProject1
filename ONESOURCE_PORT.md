# Перенос функций из `onesource.7z`

Дата проверки: 28.09.2026. Архив распакован только для анализа в `.tools/onesource/`; в сборку входят адаптированные файлы текущего проекта, а не архив целиком.

## Что перенесено

| Функция | Реализация в текущем проекте |
|---|---|
| Сайлент | Третий режим аима `AIM_MODE_SILENT`. Точная world-space точка кости публикуется отдельному writer-потоку с собственным `/proc/<pid>/mem`; перед каждой 12-байтовой записью заново проверяются классы, двусторонняя связь player↔handler, look-wrapper, FPManager, текущее оружие и обратная ссылка оружия на игрока. Lease снимка — 100 мс. |
| Быстрый подбор | `fast_loot::Controller`: проходит активные UniTask PlayerLoop promises и обнуляет только поле с точным побитовым значением `0.35f` (`0x3EB33333`). Запись разрешена только в анонимную writable-кучу. |
| Быстрая стрельба | `weapon_speed::Controller`: тумблер всегда применяет максимальный прежний коэффициент `20×` к `HitscanWeaponConfig.fireRate`. Оригинал сохраняется и условно восстанавливается при выключении, смене оружия и штатном выходе. |
| Быстрая перезарядка | Тот же контроллер делит `FullReloadPeriod` и `LoopReloadPeriod` на максимальные `20×`, отдельно от стрельбы и с восстановлением. |
| Без отдачи | Существующий object-data контроллер сохранён; литералы цепочки оружия заменены подтверждёнными именованными offsets. Пишутся шесть float-полей `RecoilConfig`, библиотечный код не патчится. |
| Без разброса | Существующий object-data контроллер сохранён; обнуляются min/max `DispersionConfig` и cached base калькулятора. Сайлент автоматически держит этот запрос ещё 1,5 с после краткой потери цели. |
| Положение рук | Все motion slots получают одну статичную X/Y/Z позицию; она сохраняется в ADS, при беге, атаке и перезарядке. Оригиналы возвращаются с ownership checks только при выключении/смене. |
| Custom FOV | Пользовательское значение постоянно записывается в world-FOV target и больше не сбрасывается во время ADS или FOV blend. |

Не переносились не запрошенные части архива: melee/use/bow speed, no-sway, instant ADS, удалённая авторизация и архивный runtime offset resolver.

Переключатели быстрой стрельбы/перезарядки сохраняются в профиле (биты `64/128`
в packed flags). Ползунков больше нет: включённый тумблер всегда использует
максимум прежнего диапазона — `20×`. Остальные настройки сохраняются существующим
профильным механизмом.

## Подтверждённые offsets

Все поля ниже совпали в предоставленных release и beta `il2cpp.h`, кроме явно build-specific TypeInfo RVA и имени калькулятора.

### Сайлент

| Цепочка | Offset |
|---|---:|
| `PlayerManager.playerEventHandler` | `0x78` |
| `eventHandler.manager` back-reference | `0xD0` |
| `eventHandler.LookDirection` | `0x140` |
| synced value внутри wrapper | `0x20` |
| `PlayerManager.FPManager` | `0x90` |
| `FPManager._currentWeapon` (`FPObject`) | `0x50` |
| `FPObject.Player` back-reference | `0xC0` |

Writer берёт начало луча из живой позиции native camera. World-точка сайлента хранится отдельно от vertical lift/range-drop коррекции, используемой текущими Touch/Memory режимами. После проверки на устройстве добавлена синхронная запись в кадре публикации, а интервал guarded-worker уменьшен с `2500/1200` до `500/250` мкс. Это закрывает окно, в котором worker мог быть снят планировщиком, а игра успевала вернуть направление камеры непосредственно перед расчётом пули.

### Быстрый подбор

| Поле | Offset |
|---|---:|
| `Il2CppClass.static_fields` | `0xB8` |
| `PlayerLoopHelper.static_fields.runners` | `0x30` |
| `PlayerLoopRunner.tail` | `0x30` |
| `PlayerLoopRunner.loopItems` | `0x38` |
| IL2CPP array length / first item | `0x18` / `0x20` |
| `DelayIgnoreTimeScalePromise` timespan / elapsed / frame | `0x18` / `0x1C` / `0x20` |
| `DelayPromise` timespan / elapsed / frame | `0x1C` / `0x20` / `0x18` |

`PlayerLoopHelper TypeInfo RVA`:

- release: `0xD9C52E0`;
- current beta dump: `0xD9A3488`.

Архивное beta-значение `0xDAE2AC0` относится к другой сборке и намеренно не использовано.

### Оружие: темп, отдача и разброс

| Поле | Offset |
|---|---:|
| `FPHitscan.config` | `0x130` |
| calculator внутри `FPHitscan` | `0x1F8` |
| embedded `WeaponRecoil` | `0x200` |
| `WeaponRecoil.recoilConfig` / `.weaponConfig` | `0x00` / `0x08` |
| calculator dispersion / hitscan config / cached base | `0x10` / `0x18` / `0x20` |
| `HitscanWeaponConfig.fireRate` | `0x34` |
| full / loop reload period | `0x48` / `0x4C` |
| config dispersion / recoil pointers | `0x60` / `0x68` |
| `RecoilConfig` изменяемые floats | `0x18..0x2C` |
| `DispersionConfig` min / max | `0x20` / `0x24` |

Имя calculator-класса build-specific: release `Ze`, beta `FJ`; раскладка одинакова. Перед записью контроллер проверяет типизированные обратные ссылки, class identity и Unity native identity. Build-id и старые instruction guards используются как дополнительное подтверждение, но больше не блокируют новый бинарник с неизменной managed-раскладкой: неизвестное имя calculator принимается только с пустым namespace и совпавшими typed backlinks.

### Custom FOV и руки

| Поле | Offset |
|---|---:|
| world-FOV target в `FPManager` | `0x9C` |
| `FPObject.Draw` activity | `0x20` |
| `FPMotion` idle/run/run-reload/attack | `0x78 / 0x80 / 0x88 / 0x90` |
| `FPMotion` ladder/jump/look/too-close | `0xA0 / 0xA8 / 0xB0 / 0xB8` |
| `TransformOffset.position` / lerp speed | `0x24 / 0x20` |

Offsets заведены в `game_offsets.h`, `game_offsets_beta.h`, активную таблицу и `tools/offsets/offsets_map.json`, чтобы последующее обновление дампов снова их проверяло.

## Lifecycle и ограничения записи

- Ни одна из перенесённых функций не пишет инструкции или file-backed mappings.
- Weapon/loot writes проходят через `object_data::heap_writable` и свежий `/proc/<pid>/maps` gate.
- Оригиналы fire/reload/recoil/spread/FOV/hands возвращаются только пока объект сохранил исходные class/native identities и текущее значение всё ещё принадлежит нашему контроллеру. Чужое обновление не затирается.
- При потере процесса адреса забываются без записи в потенциально переиспользованную память.
- Fast loot не требует восстановления: каждое новое promise игра создаёт с исходным `0.35f`; после выключения новые ожидания больше не меняются.
- Silent writer не использует кадровый cache/descriptor главного потока и останавливается при просроченном snapshot, смене owner-chain, открытом меню, freecam, detach и штатном завершении.

## Проверки

Новые стенды:

- `sh tools/silent/run.sh` — нормализация направления, invalid geometry, class/owner/weapon guards, fast-math;
- `sh tools/loot/run.sh` — exact cadence, оба promise layouts, tail bound, class mismatch, write failure;
- `sh tools/weapons/run.sh` — дополнен быстрым fire/reload: оба build-id, независимое выключение, recapture чужого обновления, смена оружия, restoration/failure paths.

Общие проверки: `tools/viewmodel/run.sh`, `tools/offsets/run.sh`, `tools/lang/check.py`, `tools/syntax/check.sh`, `tools/hostcheck/run.sh`.
