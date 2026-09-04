# tools/offsets — переопределение оффсетов после апдейта игры

Три скрипта, которыми обновлялись оффсеты в последний раз. Полный отчёт по
конкретным константам — в `OFFSETS_UPDATE.md` (раздел 5 — процедура,
раздел 7 — что менялось в каждом апдейте).

## Порядок действий

```bash
# 1. распаковать новые дампы (из рабочего дерева) и старые (из коммита до аплоада)
tools/offsets/extract_dumps.sh /tmp/new
tools/offsets/extract_dumps.sh /tmp/old <коммит-с-прошлыми-дампами>

# 2. какие структуры вообще поменяли раскладку
python3 tools/offsets/il2cpp_layout.py --old /tmp/old/il2cpp.h --new /tmp/new/il2cpp.h diff

# 3. по каждой изменившейся — полный список полей и ручное сопоставление
python3 tools/offsets/il2cpp_layout.py --old /tmp/old/il2cpp.h --new /tmp/new/il2cpp.h \
        show Oxide_LootObject_Fields

# 4. RVA глобальных TypeInfo (меняются ВСЕГДА)
python3 tools/offsets/typeinfo_rva.py --so /tmp/new/libil2cpp.so --script /tmp/new/script.json \
        Oxide.PlayerManager Oxide.GameControllerBase Mirror.NetworkClient
# контроль: та же команда на /tmp/old должна выдать значения, которые сейчас в git
```

## Почему именно так

* **Имена классов и полей переобфусцируются каждый билд** (`fvp`→`pmi`,
  `wK`→`ij`, `Mo`→`sR`). Diff по именам покажет «ничего не изменилось» и
  проглотит все переименованные поля. `il2cpp_layout.py diff` сравнивает
  **позиционно**: последовательность (смещение → тип), обфусцированные типы
  нормализуются в `OBF_o`.
* **Переименованный класс ищется по форме**, а не по имени:
  `il2cpp_layout.py find --new /tmp/new/il2cpp.h "WeaponBase_o*" "Oxide_WeaponPiece_o" "UnityEngine_Transform_o*"`
  — так был найден `sR_Fields` вместо старого `Mo_Fields`. Аргументы —
  обычные подстроки, не регэкспы.
* **`dump.cs` этого дампера не содержит смещений полей** — только значения
  enum'ов (`public static literal ... = 13;`). Смещения живут в `il2cpp.h`
  в комментариях `/* 0xNN */`.
* **`ScriptMetadata` в `script.json` пустой**, поэтому RVA типов берутся
  дизассемблированием: `adrp/ldr` → addend релокации `R_AARCH64_RELATIVE` →
  слот в `.data`; оставляем только те, что затем разыменовываются как
  `ldr x8,[klass,#0xB8]` (`Il2CppClass::static_fields`) — ровно так к ним
  ходит `game.cpp`.
* `libil2cpp.7z` сжат LZMA2 + ARM64 BCJ; `py7zr` такой фильтр не умеет,
  поэтому `extract_dumps.sh` вынимает упакованный поток и гонит его через
  `xz --format=raw --arm64`.

Зависимости: `py7zr` и `capstone` (`pip install py7zr capstone --break-system-packages`),
`xz` из состава xz-utils.
