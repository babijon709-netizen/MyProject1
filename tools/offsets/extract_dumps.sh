#!/usr/bin/env bash
# Распаковка дампов из репозитория во временный каталог.
#
# В песочнице нет ни 7z, ни bsdtar, а py7zr не умеет ARM64 BCJ-фильтр, которым
# сжат libil2cpp.7z — поэтому .so достаём напрямую через xz --format=raw.
#
#   tools/offsets/extract_dumps.sh /tmp/dump            # текущие дампы из рабочего дерева
#   tools/offsets/extract_dumps.sh /tmp/olddump HEAD~1  # дампы из прошлого коммита
#
# На выходе: <dir>/{dump.cs,il2cpp.h,script.json,libil2cpp.so}
set -euo pipefail

OUT="${1:?usage: extract_dumps.sh <outdir> [git-ref]}"
REF="${2:-}"
ROOT="$(git -C "$(dirname "$0")" rev-parse --show-toplevel)"
mkdir -p "$OUT"

# Возвращает 1, если файла в этой ревизии нет: дампы заливались разными
# коммитами, и в ревизии со старым dump.7z может не быть libil2cpp.7z.
fetch() { # <файл-в-репо> <куда>
    if [ -n "$REF" ]; then
        git -C "$ROOT" cat-file -e "$REF:$1" 2>/dev/null || return 1
        git -C "$ROOT" cat-file blob "$REF:$1" > "$2"
    else
        [ -f "$ROOT/$1" ] || return 1
        cp "$ROOT/$1" "$2"
    fi
}

python3 -c 'import py7zr' 2>/dev/null || pip install py7zr --break-system-packages -q

echo "== dump.7z -> $OUT (dump.cs / il2cpp.h / script.json)"
fetch dump.7z "$OUT/dump.7z" || { echo "   нет dump.7z в ${REF:-рабочем дереве}"; exit 2; }
python3 - "$OUT" <<'PY'
import sys, py7zr
out = sys.argv[1]
with py7zr.SevenZipFile(f'{out}/dump.7z', 'r') as z:
    z.extractall(path=out)
PY

echo "== libil2cpp.7z -> $OUT/libil2cpp.so (LZMA2 + ARM64 BCJ, через xz)"
if ! fetch libil2cpp.7z "$OUT/libil2cpp.7z"; then
    # Не фатально: .so нужен только для RVA, раскладка структур живёт в il2cpp.h.
    echo "   нет libil2cpp.7z в ${REF:-рабочем дереве} — пропускаю, RVA по нему не посчитать"
    rm -f "$OUT/dump.7z"
    ls -la "$OUT"
    exit 0
fi
read -r OFF SZ DICT < <(python3 - "$OUT" <<'PY'
import sys, py7zr
a = py7zr.SevenZipFile(f'{sys.argv[1]}/libil2cpp.7z')
pi = a.header.main_streams.packinfo
fo = a.header.main_streams.unpackinfo.folders[0]
props = next(c['properties'] for c in fo.coders if c.get('properties'))
p = props[0]                       # код размера словаря LZMA2
dict_sz = (2 | (p & 1)) << (p // 2 + 11)
print(a.afterheader + pi.packpos, pi.packsizes[0], dict_sz)
PY
)
dd if="$OUT/libil2cpp.7z" bs=1M iflag=skip_bytes,count_bytes \
   skip="$OFF" count="$SZ" status=none of="$OUT/pack.bin"
xz --format=raw --arm64 --lzma2=dict="$DICT" -dc "$OUT/pack.bin" > "$OUT/libil2cpp.so"
rm -f "$OUT/pack.bin" "$OUT/dump.7z" "$OUT/libil2cpp.7z"

ls -la "$OUT"
