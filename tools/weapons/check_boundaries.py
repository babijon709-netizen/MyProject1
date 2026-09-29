#!/usr/bin/env python3
"""Regression checks for heap-only weapon/loot controllers and thin UI."""
from pathlib import Path
import re
root = Path(__file__).resolve().parents[2]
tuning = (root / 'jni/src/weapon_tuning_memory.h').read_text()
speed = (root / 'jni/src/weapon_speed_memory.h').read_text()
loot = (root / 'jni/src/loot_memory.h').read_text()
adapter = (root / 'jni/src/game_viewmodel.cpp').read_text()
viewmodel = (root / 'jni/src/viewmodel_memory.h').read_text()
silent = (root / 'jni/src/silent_aim_core.h').read_text()
game = (root / 'jni/src/game.cpp').read_text()

# Ported controllers allow no live offset resolver or private build/RVA table.
# Addresses must come from go_active release constants.
ported = (tuning, speed, loot, silent, adapter, viewmodel)
for controller in ported:
    assert 'offset_resolver' not in controller
    code = re.sub(r'/\*.*?\*/|//[^\n]*', '', controller, flags=re.S)
    assert not re.search(r'\+\s*0x[0-9a-fA-F]+', code), 'raw field offset in ported controller'
assert 'kLayouts' not in tuning and 'guard_sites' not in tuning and 'build_id' not in tuning
assert 'kLayouts' not in speed and 'build_id' not in speed
for constant in ('NETWORK_BEHAVIOUR_IDENTITY', 'NETWORK_IDENTITY_IS_LOCAL_PLAYER'):
    assert f'go_active::{constant}' in tuning and f'go_active::{constant}' in speed
    assert f'go_active::{constant}' in adapter
for assignment in ('PLAYER_EVENT_HANDLER', 'EVENT_HANDLER_MANAGER_BACKREF',
                   'EVENT_HANDLER_LOOK_DIRECTION', 'SYNC_VALUE_OFFSET',
                   'PLAYER_FP_MANAGER', 'FPMANAGER_CURRENT_OBJECT',
                   'FPOBJECT_PLAYER_BACKREF'):
    assert re.search(r'c\.[a-z_]+_offset\s*=\s*' + assignment, game), assignment

# Controllers must not expose generic process/library writers.
for controller in (tuning, speed, loot):
    assert not re.search(r'\bio\.(?:write|patch)\s*\(', controller)
assert tuning.count('io.write_heap(') == 2       # apply + restore
assert speed.count('io.write_heap(') == 3        # apply + inherited-cache normalize + restore
for live_offset in ('FPWEAPON_SHOT_PERIOD', 'FPWEAPON_EFFECTIVE_RELOAD_PERIOD',
                    'FPHITSCAN_EFFECTIVE_LOOP_RELOAD_PERIOD', 'FPHITSCAN_BURST_WAIT',
                    'WAITFORSECONDS_SECONDS'):
    assert f'go_active::{live_offset}' in speed
assert loot.count('io_.write_heap(') == 1        # exact 0.35f promise cadence
assert 'kCadenceBits = 0x3EB33333u' in loot
assert 'PLAYER_LOOP_HELPER_TYPEINFO_RVA' in loot
assert 'class_is(klass, "PlayerLoopHelper"' in loot
assert 'weapon_speed::Controller<HeapMemory>' in adapter
assert 'kMaxMultiplier = 20.f' in speed
assert 'fire_multiplier' not in speed and 'reload_multiplier' not in speed
assert 'fast_loot::Controller<HeapMemory>' in adapter
assert 'settings.no_spread || esp_silent_wants_no_spread()' in adapter

# All object writes pass the single anonymous-heap /proc/maps gate.
heap = adapter.split('struct HeapMemory {', 1)[1].split('HeapMemory heap_memory;', 1)[0]
assert 'heap_gate_allows(address,sizeof(T)) && g_mem.write(address,&value,sizeof(T))' in heap
assert heap.count('g_mem.write(') == 1

# The gate itself: every accepted address comes from a parsed /proc/<pid>/maps
# range, an unconfirmed address forces a freshly read map, and both the range
# cache and the text snapshot are short-lived and dropped with the process.
gate = adapter.split('bool heap_gate_allows(', 1)[1].split('\n}', 1)[0]
assert gate.count('object_data::heap_writable_range(*text, address, size, lo, hi)') == 2
assert 'maps_snapshot(true)' in gate
assert 'now - gate.at >= kMapsSnapshotTtl' in gate
assert 'kMapsSnapshotTtl = 0.25' in adapter
assert 'read_proc_maps(g_pid, maps_text_cache)' in adapter
assert 'invalidate_maps_snapshot();' in adapter
assert 'for (HeapGate& gate : heap_gates) gate = {};' in adapter

# Retired tap/instruction implementations stay gone; the restored feature is
# the guarded UniTask data controller only.
for gone in ('jni/src/game_loot.cpp', 'jni/src/fast_loot_memory.h',
             'jni/src/loot_tap_memory.h', 'jni/include/loot.h'):
    assert not (root / gone).exists(), gone
assert (root / 'tools/loot/loot_test.cpp').exists()

config = (root / 'jni/src/config.cpp').read_text()
assert '(g_state.fast_fire ? 64 : 0)' in config
assert '(g_state.fast_reload ? 128 : 0)' in config
assert 'g_state.fast_fire = (flags & 64) != 0;' in config
assert 'g_state.fast_reload = (flags & 128) != 0;' in config
menu = (root / 'jni/src/menu.cpp').read_text()
assert '##instantloot' in menu and '##fastfire' in menu and '##fastreload' in menu
assert 'esp_loot_tick(' not in menu  # menu never traverses game memory itself
# The FOV/hands reset button must keep independent object-data settings.
assert 'const bool nr = vm.no_recoil, ns = vm.no_spread, fl = vm.fast_loot;' in menu
assert 'vm.no_recoil = nr; vm.no_spread = ns; vm.fast_loot = fl;' in menu
print('weapons: recoil/spread and speed controllers are heap-only: OK')
print('loot: guarded UniTask cadence controller and thin UI boundary: OK')
