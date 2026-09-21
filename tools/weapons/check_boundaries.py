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

# Instant pickup (loot_memory.h / game_loot.cpp) is now two mechanisms, both
# strictly fenced. The fallback accelerates by writing one heap field; the
# primary mode is the owner's deliberate, minimal code patch — and the checks
# below keep it minimal.
loot_controller = (root / 'jni/src/loot_memory.h').read_text()
loot_adapter = (root / 'jni/src/game_loot.cpp').read_text()
assert not (root / 'jni/src/fast_loot_memory.h').exists()
assert 'fast_loot::' not in loot_controller and 'fast_loot::' not in loot_adapter
assert not re.search(r'\bio\.(?:write|patch)\s*\(', loot_controller)
# The only heap field ever written is the awaiter's elapsed timer.
targets = re.findall(r'io\.write_heap\(([^,]+),', loot_controller)
assert targets and all(t.strip() == 'promise_ + kPromiseElapsed' for t in targets), targets
# Code writes happen ONLY as whole words into the two table sites per flow.
code_targets = re.findall(r'io\.write_code\(([^,]+),\s*([\w.]+)\)', loot_controller)
assert len(code_targets) == 5, code_targets      # apply x2, apply-rollback x1, restore x2
assert all(t.strip() in ('base_ + flow.flag_site', 'base_ + flow.call_site')
           for t, _v in code_targets), code_targets
assert all(v in ('kInstantX0', 'kInstantX1', 'flow.flag_word', 'flow.call_word')
           for _t, v in code_targets), code_targets
# The controller must never invent a hook: patching is a word swap, not a detour.
for forbidden in ('trampoline', 'detour', 'inline_hook', 'instruction_supported'):
    assert forbidden not in loot_controller, forbidden
for forbidden in ('trampoline', 'detour', 'inline_hook', 'ptrace', 'dlopen', 'memcpy'):
    assert forbidden not in loot_adapter, forbidden
# The code writer: fresh /proc maps gate scoped to the r-xp segment of
# libil2cpp.so, then one pwrite through /proc/<pid>/mem — the kernel COWs the
# page (FOLL_FORCE), so no permission state of ours or the target's ever
# changes (RWX cannot appear) and no mprotect/clear_cache may be called on a
# foreign address (both act on OUR address space and are useless-to-fatal:
# ENOMEM on the unmapped guest address was what silently killed the patch).
loot_heap = loot_adapter.split('struct Memory {', 1)[1].split('Memory memory;', 1)[0]
assert 'if (!object_data::heap_writable(text, address, sizeof(T))) {' in loot_heap
assert 'note_refusal(text, address);' in loot_heap
assert 'std::ifstream file("/proc/"+std::to_string(g_pid)+"/maps")' in loot_heap
assert loot_heap.count('g_mem.write(') == 2      # heap writer + code writer
assert 'bool write_code(uint64_t address, uint32_t word) {' in loot_heap
assert 'std::strcmp(perms, "r-xp")' in loot_heap and 'libil2cpp.so' in loot_heap
# Negative checks run on code with comments stripped: comments may (and do)
# explain WHY these calls must not appear.
loot_code = re.sub(r'//[^\n]*', '', re.sub(r'/\*.*?\*/', '', loot_heap, flags=re.S))
assert 'mprotect' not in loot_code, 'permission flips live in the target, not our VA space'
assert '__builtin___clear_cache' not in loot_code, 'icache flush on OUR va is meaningless'
assert 'PROT_READ | PROT_WRITE | PROT_EXEC' not in loot_code  # RWX, на состояние
assert 'return g_mem.write(address, &word, sizeof(word));' in loot_heap
# Guard words of both the retired patch and ours stay in the table and stay
# checked before anything is written.
assert loot_controller.count('delay_word') >= 3
assert 'word != flow.delay_word' in loot_controller
assert loot_controller.count('flag_word') >= 3 and loot_controller.count('call_word') >= 3
assert 'flag != flow.flag_word || call != flow.call_word' in loot_controller

menu = (root / 'jni/src/menu.cpp').read_text()
assert '##fastloot' not in menu
assert '##instantloot' in menu
# The FOV/hands reset button must keep the object-data and pickup flags.
assert 'const bool nr = vm.no_recoil, ns = vm.no_spread, fl = vm.fast_loot;' in menu
assert 'vm.no_recoil = nr; vm.no_spread = ns; vm.fast_loot = fl;' in menu
print('weapons: heap-only adapter and retired fast-loot writer boundaries: OK')
print('loot: instant patch fenced (2 sites/flow, COW write, verify+revert), one heap field, patched images refused: OK')
