#!/usr/bin/env python3
"""Regression checks: no legacy library writers or silently enabled v1 flags."""
from pathlib import Path
import re
root = Path(__file__).resolve().parents[2]
controller = (root / 'jni/src/weapon_tuning_memory.h').read_text()
adapter = (root / 'jni/src/game_viewmodel.cpp').read_text()
assert not re.search(r'\bio\.(?:write|patch)\s*\(', controller)
assert controller.count('io.write_heap(') == 2  # apply and restore, same gate
heap = adapter.split('struct HeapMemory {', 1)[1].split('HeapMemory heap_memory;', 1)[0]
assert 'object_data::heap_writable(text,address,sizeof(T)) &&' in heap
assert 'std::ifstream file("/proc/"+std::to_string(g_pid)+"/maps")' in heap
assert heap.count('g_mem.write(') == 1
assert not (root / 'jni/src/fast_loot_memory.h').exists()
assert 'fast_loot::' not in adapter
assert 'instruction_supported' not in controller

# Быстрый подбор удалён по запросу: логики нет, осталась только кнопка-тумблер
# в меню. Пины фиксируют: ни контроллера/адаптера/стендов, ни вызовов из
# main/game/config/menu, и ряд в меню на месте.
for gone in ('jni/src/game_loot.cpp', 'jni/src/loot_memory.h',
             'jni/src/loot_tap_memory.h', 'jni/include/loot.h', 'tools/loot'):
    assert not (root / gone).exists(), gone
main = (root / 'jni/src/main.cpp').read_text()
assert 'esp_loot_tick(' not in main and 'esp_loot_diagnostics(' not in main
assert 'esp_loot_reset(' not in main and 'esp_loot_log_path(' not in main
game = (root / 'jni/src/game.cpp').read_text()
config = (root / 'jni/src/config.cpp').read_text()
assert 'esp_loot_reset(' not in game and 'esp_loot_reset(' not in config
menu = (root / 'jni/src/menu.cpp').read_text()
assert '##fastloot' not in menu
assert '##instantloot' in menu
assert 'esp_loot_tick(' not in menu
# The FOV/hands reset button must keep the object-data and pickup flags.
assert 'const bool nr = vm.no_recoil, ns = vm.no_spread, fl = vm.fast_loot;' in menu
assert 'vm.no_recoil = nr; vm.no_spread = ns; vm.fast_loot = fl;' in menu
print('weapons: heap-only adapter and retired fast-loot writer boundaries: OK')
print('loot: feature removed, menu toggle retained: OK')
