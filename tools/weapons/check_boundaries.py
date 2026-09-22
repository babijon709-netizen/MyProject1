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

# Instant pickup (loot_memory.h / game_loot.cpp) is heap-only: the code patch
# of libil2cpp.so was REMOVED (owner decision: the one-tick whole-window drain
# is a server-side telemetry giveaway, and .text modification is surface for
# integrity CRCs). The checks below keep the no-code-write property strict.
loot_controller = (root / 'jni/src/loot_memory.h').read_text()
loot_adapter = (root / 'jni/src/game_loot.cpp').read_text()
assert not (root / 'jni/src/fast_loot_memory.h').exists()
assert 'fast_loot::' not in loot_controller and 'fast_loot::' not in loot_adapter
assert not re.search(r'\bio\.(?:write|patch)\s*\(', loot_controller)
# The only heap field ever written is the awaiter's elapsed timer.
targets = re.findall(r'io\.write_heap\(([^,]+),', loot_controller)
assert targets and all(t.strip() == 'promise_ + kPromiseElapsed' for t in targets), targets
# NO code writes at all — in either file, outside comments.
controller_code = re.sub(r'//[^\n]*', '', loot_controller)
adapter_code = re.sub(r'//[^\n]*', '', re.sub(r'/\*.*?\*/', '', loot_adapter, flags=re.S))
for clean in (controller_code, adapter_code):
    assert 'write_code' not in clean, 'the code-patch path stays deleted'
    assert 'kInstantX' not in clean, 'patch words stay deleted'
    assert 'mprotect' not in clean, 'no permission flips on foreign mappings'
    assert '__builtin___clear_cache' not in clean
# The table keeps the two delay-signature words only as READ-ONLY identity
# marks: the flow must look stock, otherwise the flow is refused.
assert 'flag != flow.flag_word || call != flow.call_word' in loot_controller
assert loot_controller.count('flag_word') >= 2 and loot_controller.count('call_word') >= 2
assert 'word != flow.delay_word' in loot_controller and loot_controller.count('delay_word') >= 3
# The controller must never invent a hook: no detours, no foreign loaders.
for forbidden in ('trampoline', 'detour', 'inline_hook', 'instruction_supported'):
    assert forbidden not in loot_controller, forbidden
for forbidden in ('trampoline', 'detour', 'inline_hook', 'ptrace', 'dlopen', 'memcpy'):
    assert forbidden not in loot_adapter, forbidden
# The heap writer: fresh /proc maps gate, anonymous-heap check, one writer.
loot_heap = loot_adapter.split('struct Memory {', 1)[1].split('Memory memory;', 1)[0]
assert 'if (!object_data::heap_writable(text, address, sizeof(T))) {' in loot_heap
assert 'note_refusal(text, address);' in loot_heap
assert 'std::ifstream file("/proc/"+std::to_string(g_pid)+"/maps")' in loot_heap
assert loot_heap.count('g_mem.write(') == 1      # the heap writer only
# The DI window detection is held to the same read-only contract: fixed
# layout constants from il2cpp.h (смещения одинаковы release↔beta).
for needle in ('kDiProjCtxInstance  = 0x20', 'kDiProjCtxContainer = 0x60',
               'kDiContainerProviders = 0x18', 'kDiDictEntries   = 0x18',
               'kDiDictCount     = 0x20', 'kDiBindStride    = 0x20',
               'kDiBindKeyType   = 0x08', 'kDiBindValue     = 0x18',
               'kDiTypeImpl      = 0x10', 'kDiListItems     = 0x10',
               'kDiListSize      = 0x18', 'kDiProviderInfo  = 0x20',
               'kDiProvInstances = 0x18', 'kDiWinStride     = 0x18',
               'kDiWinKey        = 0x08', 'kDiServiceDictA  = 0x50',
               'kDiServiceDictB  = 0x58', 'kDiIdsCount      = 161',
               'kDiMaxBindings   = 4096'):
    assert needle in loot_controller, needle
# Детект окна — только диагностика: сервис ищется через интерфейс, провайдер
# singleton-а и класс инстанса сверяются по имени, записей нет (см. выше).
assert 'layout_->iface_name' in loot_controller and 'layout_->service_name' in loot_controller
assert '"CachedProvider", "Zenject"' in loot_controller

menu = (root / 'jni/src/menu.cpp').read_text()
assert '##fastloot' not in menu
assert '##instantloot' in menu
# The FOV/hands reset button must keep the object-data and pickup flags.
assert 'const bool nr = vm.no_recoil, ns = vm.no_spread, fl = vm.fast_loot;' in menu
assert 'vm.no_recoil = nr; vm.no_spread = ns; vm.fast_loot = fl;' in menu
print('weapons: heap-only adapter and retired fast-loot writer boundaries: OK')
print('loot: heap-only (no code writes anywhere), one heap field, patched images refused: OK')
