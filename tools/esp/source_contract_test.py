#!/usr/bin/env python3
"""Regression checks for the working old_source visual/attach contracts."""

from pathlib import Path
import re

root = Path(__file__).resolve().parents[2]
main = (root / "jni/src/main.cpp").read_text(encoding="utf-8")
draw = (root / "jni/src/Android_draw/draw.cpp").read_text(encoding="utf-8")
process = (root / "jni/src/process.cpp").read_text(encoding="utf-8")
game = (root / "jni/src/game.cpp").read_text(encoding="utf-8")

assert "initGUI_draw(overlay.surface_side, overlay.surface_side, true)" in main, \
    "EGL overlay must remain square so portrait is not cropped"
assert "DisplaySize = ImVec2(side, side)" in draw, \
    "ImGui canvas must match the square EGL surface"

assert "esp_init(official, true)" in process, \
    "official release process must retain old_source early-loading attach"
assert "esp_init(candidates[i])" in process, \
    "renamed Unity candidates must still require strict release image validation"
assert "esp_init(candidates[i], true)" not in process, \
    "early-loading fallback must never be enabled for an arbitrary Unity process"

fallback = re.search(
    r"if \(!chosen && package_verified_release\) \{(?P<body>.*?)\n\s*\}\n\s*if \(!chosen\)",
    game,
    flags=re.S,
)
assert fallback and "g_mem.bind(pid, candidates[i])" in fallback.group("body"), \
    "old_source-compatible fallback must be guarded by official package verification"

validator = re.search(
    r"static bool validate_player_position_field\([^)]*\) \{(?P<body>.*?)\n\}",
    game,
    flags=re.S,
)
assert validator, "player position validator not found"
body = validator.group("body")
assert "player + PLAYER_POSITION" in body
assert "read_transform_hierarchy_position" not in body, \
    "a plausible native pivot must not override the fixed release player position"

print("visual source contracts: OK")
