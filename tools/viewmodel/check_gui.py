#!/usr/bin/env python3
"""Regression guards for the menu.

1. Tab 3 ("Разное") is organized modularly into card blocks (similar to Autofarm),
   without duplicate toggles, sliders, or controls directly on the tab.
2. Settings and toggles reside inside their respective popover windows in widgets.cpp.
3. Misc toggles must animate from the same state used by the row:
   Tick(g_state.<anim>, g_state.viewmodel.<setting>, dt).
4. Fast pickup is a heap-data feature (fast_loot::Controller in loot_memory.h):
   the toggle must drive esp_viewmodel_tick. Retired loot-module APIs stay banned.
"""
from pathlib import Path
import re

root = Path(__file__).resolve().parents[2]
menu = (root / 'jni/src/menu.cpp').read_text()
widgets = (root / 'jni/src/widgets.cpp').read_text()

# Verify that Tab 3 ("Разное") contains modular block headers
assert 'CollapsibleHeader("##fnfarm", XS("Автофарм"), 5);' in menu
assert 'CollapsibleHeader("##fnwpn", XS("Оружие"), 6);' in menu
assert 'CollapsibleHeader("##fnvm", XS("Вид и руки"), 7);' in menu
assert 'CollapsibleHeader("##fnloot", XS("Быстрый сбор"), 8);' in menu
assert 'CollapsibleHeader("##fnfps", XS("Лимит FPS"), 9);' in menu
assert 'CollapsibleHeader("##fnxray", XS("Иксрей"), 10);' in menu
assert 'CollapsibleHeader("##fnfc", XS("Фрикам"), 11);' in menu
assert 'CollapsibleHeader("##fnworld", XS("Мир"), 12);' in menu

# Verify that Tab 3 does NOT duplicate the functional controls
for dup in ['ToggleRow("##farmon"', 'ToggleRow("##nosway"', 'ToggleRow("##instantads"',
            'ToggleRow("##norecoil"', 'ToggleRow("##nospread"', 'ToggleRow("##vmfov"',
            'ToggleRow("##vmhands"', 'ToggleRow("##fastloot"', 'fpsChip(',
            'ToggleRow("##xr0"', 'ToggleRow("##fc0"', 'ToggleRow("##wd0"']:
    assert dup not in menu, f'duplicate control found directly in menu.cpp: {dup}'

# Popover toggles in widgets.cpp and per-frame Tick animations in menu.cpp
for setting, anim in [('fov_enabled', 'a_custom_fov'),
                      ('hands_enabled', 'a_custom_hands'),
                      ('no_recoil', 'a_no_recoil'),
                      ('no_spread', 'a_no_spread'),
                      ('no_sway', 'a_no_sway'),
                      ('instant_ads', 'a_instant_ads'),
                      ('fast_loot', 'a_fast_loot')]:
    assert re.search(r'FgToggleRow\([^;]*&g_state\.viewmodel\.' + setting +
                     r'\s*,\s*g_state\.' + anim + r'\b', widgets), setting
    assert re.search(r'Tick\(\s*g_state\.' + anim +
                     r'\s*,\s*g_state\.viewmodel\.' + setting +
                     r'\s*,\s*dt\s*\)', menu), f'missing animation update: {setting}'

pickup = widgets.split('secId == 8', 1)[1].split('else if (secId == 9', 1)[0]
# Быстрый сбор (heap-данные, loot_memory.h): тумблер в окне блока обязан тикать контроллер
assert 'FgToggleRow(XS("Быстрый сбор"), &g_state.viewmodel.fast_loot' in pickup, 'fastloot toggle must stay in popover'
assert 'esp_viewmodel_tick' in pickup, 'the toggle must drive the controller tick'
assert 'esp_viewmodel_diagnostics' not in pickup, 'loot status/diag lines must stay removed'
for banned in ('esp_loot_tick', 'lootStatus', 'LootReasonText', 'LootKindText',
               'esp_loot_diagnostics', 'esp_loot_refusal'):
    assert banned not in pickup, 'retired loot-module API must not come back: %s' % banned

# The FPS popover section must spell out the selected mode and whether it stuck
fps = widgets.split('secId == 9', 1)[1].split('else if (secId == 10', 1)[0]
assert 'esp_viewmodel_fps_ui()' in fps, 'the fps status line is missing'
assert 'viewmodel::Status::Active' in fps and 'viewmodel::Status::Failed' in fps, 'apply status not rendered'
assert 'field_value' in fps, 'the real game field content is not shown'
assert 'fpsChip(' in fps, 'chips not drawn'
assert '"90 FPS"' in fps and '"120 FPS"' in fps and '"144 FPS"' in fps and 'XS("Сброс")' in fps, 'mode buttons are gone'
assert 'g_state.a_fps_90' in menu and 'Tick(g_state.a_fps_90' in menu, 'selection fill animation is not ticked'

print('misc tabs: modular blocks and popovers: OK')
print('misc toggles: popover row state and per-frame animation bindings: OK')
print('fast pickup drives the heap controller; retired loot APIs absent: OK')

