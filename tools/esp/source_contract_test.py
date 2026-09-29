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

# old_source resolved the player position field (and the native Transform
# hierarchy layout) at runtime instead of trusting one hard-coded constant.
# Freezing them broke every visual on the shipped client, so the adaptive
# resolvers must stay in place.
discovery = re.search(
    r"static bool discover_player_position_offset\([^)]*\) \{(?P<body>.*?)\n\}",
    game,
    flags=re.S,
)
assert discovery, "runtime player position discovery not found"
assert "find_direct_player_position_offset" in discovery.group("body"), \
    "direct position offsets must be probed, not assumed"
assert "discover_transform_hierarchy_layout" in discovery.group("body"), \
    "the Transform hierarchy fallback must stay reachable"

assert "k_known_position_offsets[] = {0x1D0, 0x1C8" in game, \
    "known direct position offsets must stay ordered by trust"
assert "static bool validate_player_position_field" not in game, \
    "the fixed-offset-only validator must not come back"

assert "recheck_direct_player_position" in game, \
    "the hierarchy fallback must keep probing the direct fields"
assert "discover_gameobject_name_offset" in game, \
    "GameObject name offset must be discovered from real bone names"
assert "matrices_indirect" in game, \
    "pointer-indirect hierarchy arrays must stay supported"

markers = (root / "jni/src/game_markers.cpp").read_text(encoding="utf-8")
assert "discover_layout_from_native_transforms" in markers, \
    "markers must be able to learn the layout solo (no players around)"

print("visual source contracts: OK")
