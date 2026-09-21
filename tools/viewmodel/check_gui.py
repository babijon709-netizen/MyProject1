#!/usr/bin/env python3
"""Regression guards for the menu.

1. Misc toggles must animate from the same state used by the row: ToggleRow
   changes the bool but draws the supplied animation value, and forgetting its
   per-frame Tick leaves a functional toggle looking permanently disabled.
2. Instant pickup must report WHY it refused. The log is compiled out in this
   build (LogLine is a no-op and farmlog is gone from the tree), so the menu
   lines are the only diagnostics that ever reach the device: every
   loot::Reason has to be rendered, with its number, and a heap-gate refusal has
   to show the /proc/<pid>/maps line it choked on.
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
reasons = ['Image', 'Patched', 'Code', 'RunShape', 'Helper', 'Runners',
           'Read', 'Write', 'Timer']
body = menu.split('LootReasonText(loot::Reason reason)', 1)[1].split('\n}', 1)[0]
for reason in reasons:
    assert re.search(r'case loot::Reason::' + reason + r':\s*return XS\("[^"]+"\);',
                     body), f'reason not rendered in the menu: {reason}'
assert 'default:' in body, 'an unknown reason must still print something'
assert re.search(r'lootStatus\.status == loot::Status::Failed.*?LootReasonText\(lootStatus\.reason\)',
                 menu, re.S), 'the refusal line does not name the reason'
assert re.search(r'LootReasonText\(lootStatus\.reason\)', menu)
assert 'lootStatus.detail' in menu, 'the number behind the reason is not shown'
assert 'esp_loot_refusal()' in menu, 'a heap-gate refusal must show its maps line'
assert re.search(r'lootStatus\.flows\b', menu), 'partial flow availability is not shown'
assert 'LootKindText(lootStatus.kind)' in menu, 'the accelerated flow is not named'
print('misc toggles: row state and per-frame animation bindings: OK')
print('instant pickup menu: every refusal reason, its number and the maps line: OK')
