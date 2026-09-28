# Обновление offsets текущего релиза

Проект поддерживает один клиент: `com.catsbit.oxidesurvivalisland`. Все адреса подключаются на этапе компиляции из `jni/src/game_offsets.h` через `jni/src/game_offsets_active.h`. Изменяемой таблицы, выбора набора и подбора смещений в процессе игры нет.

## Источники

- `dump.cs`, `il2cpp.h`, `script.json` и `libil2cpp.so` одного release-билда;
- `libunity.so` того же APK для нативной раскладки Camera/Transform и FPS;
- `tools/offsets/offsets_map.json` — карта происхождения именованных констант;
- `tools/offsets/PROVENANCE.md` — сгенерированный отчёт.

Имена обфусцированных классов и полей меняются между обновлениями. Сопоставляйте их по структуре, типам и позиции поля, а не переносите старое имя вслепую.

## Порядок обновления

1. Распаковать dump и нативные библиотеки нового release APK вне Git.
2. Проверить карту без записи:

   ```sh
   python3 tools/offsets/update_offsets.py /path/to/dump.cs \
     --il2cpp /path/to/il2cpp.h --script /path/to/script.json
   ```

3. Разобрать предупреждения. Для TypeInfo RVA использовать `tools/offsets/typeinfo_rva.py`; для Unity layout — `tools/offsets/unity_layout.py --release /path/to/libunity.so`.
4. Применить подтверждённые значения тем же вызовом с `--apply`.
5. Пересобрать provenance и выполнить проверки:

   ```sh
   python3 tools/offsets/make_provenance.py
   sh tools/offsets/run.sh
   sh tools/hostcheck/run.sh
   sh tools/syntax/check.sh
   python3 tools/lang/check.py
   ```

6. Собрать Android `arm64-v8a` и проверить на устройстве: attach, камера, player ESP, смерть/respawn, маркеры, aim, оружие, custom FOV/hands и восстановление всех патчей.

## Инварианты

- Каждый адрес цепочки — именованная compile-time константа release-таблицы.
- Проверки class identity, backlinks и диапазонов только подтверждают владение объектом; они не выбирают другое смещение.
- Неизвестный build ID или изменённый ARM64 fingerprint закрывает соответствующую функцию без записи.
- Camera-only публикация не сбрасывает attachment, локального игрока или player-кэши: обычный resolver повторяет чтение на следующем кадре, а `EspContinuity` удерживает последний валидный visual snapshot. Таймеров, которые из-за отказа ESP вызывают reattach или разрушают независимые memory-функции, нет.
- `FPMOTION_AIM_OFFSET` не принадлежит custom-hands patch: ADS pose остаётся штатной; custom FOV продолжает действовать в ADS.
