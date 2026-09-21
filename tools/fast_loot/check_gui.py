#!/usr/bin/env python3
"""Regression: misc toggles must animate from the same state used by the row.

ToggleRow changes the bool but draws the supplied animation value. Forgetting
its per-frame Tick leaves a functional toggle looking permanently disabled.
"""
from pathlib import Path
import re

root = Path(__file__).resolve().parents[2]
menu = (root / 'jni/src/menu.cpp').read_text()
for setting, anim in [('fov_enabled', 'a_custom_fov'),
                      ('hands_enabled', 'a_custom_hands'),
                      ('fast_loot', 'a_fast_loot')]:
    assert re.search(r'ToggleRow\([^;]*&vm\.' + setting +
                     r'\s*,\s*g_state\.' + anim + r'\b', menu), setting
    assert re.search(r'Tick\(\s*g_state\.' + anim +
                     r'\s*,\s*g_state\.viewmodel\.' + setting +
                     r'\s*,\s*dt\s*\)', menu), f'missing animation update: {setting}'
print('misc toggles: row state and per-frame animation bindings: OK')
