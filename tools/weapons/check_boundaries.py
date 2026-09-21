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
menu = (root / 'jni/src/menu.cpp').read_text()
assert '##fastloot' not in menu
assert 'const bool nr = vm.no_recoil, ns = vm.no_spread;' in menu
assert 'vm.no_recoil = nr; vm.no_spread = ns;' in menu
print('weapons: heap-only adapter and retired fast-loot writer boundaries: OK')
