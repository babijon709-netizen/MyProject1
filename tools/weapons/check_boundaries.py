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
assert 'instruction_supported' not in controller

# Быстрый сбор (heap-only): контроллер в loot_memory.h пишет только 0.0f в
# float-поле темпа UniTask-обещания через тот же гейт write_heap, что и
# weapon_tuning. Пины фиксируют: один контроллер, одна точка записи,
# никакой записи в код библиотек, вайринг в адаптере.
loot = (root / 'jni/src/loot_memory.h').read_text()
assert 'namespace fast_loot' in loot
assert not re.search(r'\bio_\.write\s*\(', loot)
assert not re.search(r'\bio_\.patch\s*\(', loot)
assert loot.count('write_heap(') == 1  # единственная точка записи: 0.0f в delayFrameTimeSpan
assert 'fast_loot::Controller<HeapMemory> loot(heap_memory);' in adapter
assert 'loot.tick(' in adapter and 'status.fast_loot = loot.status;' in adapter
for gone in ('jni/src/game_loot.cpp', 'jni/src/loot_tap_memory.h',
             'jni/include/loot.h', 'tools/loot'):
    assert not (root / gone).exists(), gone
main = (root / 'jni/src/main.cpp').read_text()
assert 'esp_loot_tick(' not in main and 'esp_loot_diagnostics(' not in main
assert 'esp_loot_reset(' not in main and 'esp_loot_log_path(' not in main
game = (root / 'jni/src/game.cpp').read_text()
config = (root / 'jni/src/config.cpp').read_text()
assert 'esp_loot_reset(' not in game and 'esp_loot_reset(' not in config
menu = (root / 'jni/src/menu.cpp').read_text()
assert '##instantloot' not in menu
assert '##fastloot' in menu
assert 'esp_loot_tick(' not in menu
# Тумблер быстрого сбора применяет настройки сразу; статусов под ним нет.
assert 'if (oldFastLoot != vm.fast_loot) esp_viewmodel_tick(vm);' in menu
# The FOV/hands reset button must keep the object-data and pickup flags.
assert 'const bool nr = vm.no_recoil, ns = vm.no_spread, fl = vm.fast_loot;' in menu
assert 'vm.no_recoil = nr; vm.no_spread = ns; vm.fast_loot = fl;' in menu
print('weapons: heap-only adapter and retired fast-loot writer boundaries: OK')
print('loot: fast pickup via heap-only UniTask controller: OK')
