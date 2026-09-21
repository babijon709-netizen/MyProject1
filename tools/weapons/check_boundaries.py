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

# Instant pickup (loot_memory.h / game_loot.cpp) replaced the retired
# instruction-patch fast loot. It must keep the same boundary: one heap-only
# writer, no library writes, and the retired patch still detected and refused.
loot_controller = (root / 'jni/src/loot_memory.h').read_text()
loot_adapter = (root / 'jni/src/game_loot.cpp').read_text()
assert not (root / 'jni/src/fast_loot_memory.h').exists()
assert 'fast_loot::' not in loot_controller and 'fast_loot::' not in loot_adapter
assert not re.search(r'\bio\.(?:write|patch)\s*\(', loot_controller)
# The only field ever written is the awaiter's elapsed timer.
targets = re.findall(r'io\.write_heap\(([^,]+),', loot_controller)
assert targets and all(t.strip() == 'promise_ + kPromiseElapsed' for t in targets), targets
assert 'instruction_supported' not in loot_controller
for forbidden in ('mprotect', 'ptrace', 'dlopen', 'process_vm_writev', 'memcpy', 'sys/mman.h'):
    assert forbidden not in loot_adapter, forbidden
loot_heap = loot_adapter.split('struct Memory {', 1)[1].split('Memory memory;', 1)[0]
assert 'object_data::heap_writable(text, address, sizeof(T)) &&' in loot_heap
assert 'std::ifstream file("/proc/"+std::to_string(g_pid)+"/maps")' in loot_heap
assert loot_heap.count('g_mem.write(') == 1
# A process that still carries the retired delay patch must be refused, so the
# guard words stay in the table and stay checked before anything is written.
assert loot_controller.count('delay_word') >= 3
assert 'word != flow.delay_word' in loot_controller

menu = (root / 'jni/src/menu.cpp').read_text()
assert '##fastloot' not in menu
assert '##instantloot' in menu
# The FOV/hands reset button must keep the object-data and pickup flags.
assert 'const bool nr = vm.no_recoil, ns = vm.no_spread, fl = vm.fast_loot;' in menu
assert 'vm.no_recoil = nr; vm.no_spread = ns; vm.fast_loot = fl;' in menu
print('weapons: heap-only adapter and retired fast-loot writer boundaries: OK')
print('loot: instant pickup is heap-only, one elapsed field, patched images refused: OK')
