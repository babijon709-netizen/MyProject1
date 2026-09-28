# Release offset tools

Проект использует только `jni/src/game_offsets.h`. `game_offsets_active.h` экспортирует эти compile-time константы в namespace `go_active` для существующих consumers.

## Основные команды

```sh
# Проверка release-only header
sh tools/offsets/run.sh

# Сверка нового dump без изменения файлов
python3 tools/offsets/update_offsets.py /path/to/dump.cs \
  --il2cpp /path/to/il2cpp.h --script /path/to/script.json

# Применение проверенных изменений
python3 tools/offsets/update_offsets.py /path/to/dump.cs \
  --il2cpp /path/to/il2cpp.h --script /path/to/script.json --apply

# Проверка release libunity.so
python3 tools/offsets/unity_layout.py --release /path/to/libunity.so

# Отчёт происхождения
python3 tools/offsets/make_provenance.py
```

`offsets_map.json` связывает константу с полем/структурой или нативным ABI-источником. Обфусцированные имена служат подсказками; обновление подтверждается типом, положением поля и нативными fingerprints.

Код игры не подбирает offsets. Live-проверки class identity, backlinks, Unity identity и диапазонов значений являются write/read guards для уже заданной цепочки.
