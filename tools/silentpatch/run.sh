#!/bin/sh
# Стенд патча сайлента — проверка машинного кода трамполина.
#
# Зачем: трамполин в silent_patch.cpp собран руками, инструкция за инструкцией.
# Ошибка в одном бите кода видна не как «чит не работает», а как вылет игры на
# устройстве — и ловить её там себе дороже. Здесь каждый код разбирается
# дизассемблером и сверяется с комментарием рядом с константой: пролог ли
# вытеснен верно, туда ли ведёт cbz, те ли смещения у ldr s0/s1/s2, тот ли
# регистр у br, те ли адреса собирают movz/movk.
#
# Без них стенд не падает, а печатает предупреждение и пропускает проверку.
#
#   sh tools/silentpatch/run.sh
set -e
cd "$(dirname "$0")/../.."

# capstone и unicorn ставятся так:
#   python3 -m pip install --target=.tools/capstone capstone
#   python3 -m pip install --target=.tools/unicorn  unicorn
PY=""
[ -d .tools/capstone ] && PY=".tools/capstone"
[ -d .tools/unicorn ]  && PY="$PY${PY:+:}.tools/unicorn"
export PYTHONPATH="$PY"

python3 tools/silentpatch/encodings.py    # каждый код — дизассемблером
python3 tools/silentpatch/emulate.py      # весь трамполин — на эмуляторе ARM64
