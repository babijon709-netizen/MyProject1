#!/bin/sh
# Перекраска белого набора иконок в чёрный глиф с белой обводкой (ImageMagick).
# usage: tab_icons_from_png.sh <папка с PNG> <папка для выхода>
# Исходники: target, visuals, other, copy, keybinds -> tab_aim, tab_esp, tab_misc, tab_cfg, tab_opt.
set -e
SRC=$1
OUT=$2
mkdir -p "$OUT"
for pair in target:tab_aim visuals:tab_esp other:tab_misc copy:tab_cfg keybinds:tab_opt; do
    s=${pair%%:*}; d=${pair##*:}
    convert "$SRC/$s.png" -background none -filter Lanczos -resize 192x192 PNG32:"$OUT/${d}_src.png"
    convert "$OUT/${d}_src.png" -alpha extract -threshold 35% "$OUT/${d}_mask.png"
    convert "$OUT/${d}_mask.png" -morphology Dilate Disk:3 "$OUT/${d}_halo.png"
    convert -size 192x192 xc:white "$OUT/${d}_halo.png" -compose CopyOpacity -composite "$OUT/${d}_white.png"
    convert -size 192x192 xc:black "$OUT/${d}_mask.png" -compose CopyOpacity -composite "$OUT/${d}_black.png"
    convert "$OUT/${d}_white.png" "$OUT/${d}_black.png" -background none -compose over -flatten PNG32:"$OUT/${d}.png"
done
