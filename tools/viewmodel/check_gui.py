#!/usr/bin/env python3
"""Regression guards for the menu.

1. Misc toggles must animate from the same state used by the row: ToggleRow
   changes the bool but draws the supplied animation value, and forgetting its
   per-frame Tick leaves a functional toggle looking permanently disabled.
2. Instant pickup draws only the toggle row in the menu: the user asked for
   NO diagnostic lines under it. The strict refusal diagnostics stay in the
   loot::Diagnostics structure for tooling; the guard asserts the thin menu.
"""
from pathlib import Path
import re

root = Path(__file__).resolve().parents[2]
menu = (root / 'jni/src/menu.cpp').read_text()
for setting, anim in [('fov_enabled', 'a_custom_fov'),
                      ('hands_enabled', 'a_custom_hands'),
                      ('no_recoil', 'a_no_recoil'),
                      ('no_spread', 'a_no_spread'),
                      ('fast_loot', 'a_fast_loot')]:
    assert re.search(r'ToggleRow\([^;]*&vm\.' + setting +
                     r'\s*,\s*g_state\.' + anim + r'\b', menu), setting
    assert re.search(r'Tick\(\s*g_state\.' + anim +
                     r'\s*,\s*g_state\.viewmodel\.' + setting +
                     r'\s*,\s*dt\s*\)', menu), f'missing animation update: {setting}'
pickup = menu.split('SHdr(XS("Подбор"))', 1)[1].split('SHdr(', 1)[0]
assert 'ToggleRow("##instantloot"' in pickup and 'esp_loot_tick' in pickup, 'the toggle must still tick'
for banned in ('lootStatus', 'LootReasonText', 'LootKindText', 'esp_loot_diagnostics', 'esp_loot_refusal'):
    assert banned not in pickup, 'diagnostics must not be rendered under the toggle: %s' % banned
# The FPS section must spell out the selected mode and whether it stuck:
# a highlighted button alone used to read as applied when nothing was.
fps = menu.split('SHdr(XS("Лимит FPS"))', 1)[1].split('SHdr(', 1)[0]
assert 'esp_viewmodel_fps_ui()' in fps, 'the fps status line is missing'
assert 'viewmodel::Status::Active' in fps and 'viewmodel::Status::Failed' in fps, 'apply status not rendered'
assert 'field_value' in fps, 'the real game field content is not shown'
assert 'fpsChip(' in fps, 'chips not drawn'
assert '##fps90' in fps and '##fps120' in fps and '##fps144' in fps and '##fpsrst' in fps, 'mode buttons are gone'
assert 'WasTappedHere' in fps, 'chips must use the menu touch pattern, not bare ImGui::Button'
assert 'ImGui::Button(' not in fps, 'chips must use custom-coded buttons'
assert 'g_state.a_fps_90' in menu and 'Tick(g_state.a_fps_90' in menu, 'selection fill animation is not ticked'

print('misc toggles: row state and per-frame animation bindings: OK')
print('instant pickup menu draws only the toggle; fps mode+apply status is rendered: OK')
