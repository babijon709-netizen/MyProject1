#!/usr/bin/env python3
import ctypes
from pathlib import Path
import re
import subprocess
import sys

root=Path(__file__).resolve().parents[2]
src=(root/'experimental/loot_module/module.cpp').read_text()
# There is no injector, writable-code page, or global API/vtable hook.
for token in ('ptrace(', 'mprotect(', 'dlopen(', 'std::thread', 'pwrite(', 'process_vm_writev('):
    assert token not in src, token
assert src.count('__atomic_compare_exchange_n(')==1
assert '.inst 0xd50324df' in src  # own BTI call/jump entry, not a game instruction write
assert 'object_data::heap_writable(maps,address,8)' in src
assert 'action+0x18,original,' in src
assert 'binding.action+0x18,reinterpret_cast<uint64_t>(&arena_loot_entry),binding.original' in src
assert 'r.result==Result::NotReady && r.attempted==0' in src
assert 'if(frame==last_frame) return;' in src
assert 'module.cpp' not in (root/'jni/Android.mk').read_text()
# The batch stays wait-free: the only sleep in the module is the worker's
# periodic re-check, which is never part of a collection pass.
assert not re.search(r'\b(?:sleep|usleep|nanosleep|WaitForSeconds)\s*\(',(root/'experimental/loot_module/batch.h').read_text())
assert src.count('usleep(')==1
# Threads and load-time behavior: exactly one worker start site, and the only
# load-time hook is the explicit ARENA_LOOT_AUTOSTART opt-in.
assert src.count('pthread_create(')==1
assert src.count('__attribute__((constructor))')==1
ctor=re.search(r'__attribute__\(\(constructor\)\) void arena_loot_load\(void\) \{(.*?)\n\}',src,re.S)
assert ctor, 'load-time hook missing'
body=ctor.group(1)
for token in ('memory.', 'scan_for_delegate', 'find_delegate', 'recursive_mutex', 'posix_memalign', 'mmap('):
    assert token not in body, token
assert 'ARENA_LOOT_AUTOSTART' in body and 'start_worker()' in body
assert 'if(mode && mode[0]==' in body
# Writes stay limited to the delegate CAS: one write gate call, one atomic.
assert src.count('exchange_heap(')==4  # declaration + bind + restore + stale-path check

path=sys.argv[1]
dynamic=subprocess.check_output(['readelf','-d',path],text=True)
assert 'NODELETE' in dynamic
lib=ctypes.CDLL(path)
class Report(ctypes.Structure):
    _fields_=[('size',ctypes.c_uint32),('status',ctypes.c_uint32),('batch',ctypes.c_uint32),
              ('inspected',ctypes.c_uint32),('attempted',ctypes.c_uint32),('returned',ctypes.c_uint32),
              ('delegate',ctypes.c_uint64)]
lib.arena_loot_probe_v1.argtypes=[ctypes.c_uint64,ctypes.c_uint64]
lib.arena_loot_install_v1.argtypes=[ctypes.c_uint64,ctypes.c_uint64]
lib.arena_loot_arm_v2.argtypes=[ctypes.c_uint64]
lib.arena_loot_restore_v2.argtypes=[]
lib.arena_loot_report_v1.argtypes=[ctypes.POINTER(Report),ctypes.c_uint32]
r=Report()
assert lib.arena_loot_report_v1(ctypes.byref(r),ctypes.sizeof(r))==0
assert not r.attempted and not r.delegate  # loading the DSO has no gameplay side effects
assert lib.arena_loot_probe_v1(0,0)==2
assert lib.arena_loot_probe_v1(0x10000,0)==2
assert lib.arena_loot_probe_v1(0xffffffffffffffff,0)==2
assert lib.arena_loot_install_v1(0,0)==3  # cannot execute game code on the host
assert lib.arena_loot_disable_v1()==3
assert lib.arena_loot_restore_v2()==0   # nothing bound on the host: no write
assert lib.arena_loot_arm_v2(0)==2      # no il2cpp image in this process
assert lib.arena_loot_arm_v2(0x10000)==2
assert lib.arena_loot_restore_v2()==0
assert lib.arena_loot_report_v1(ctypes.byref(r),1)==2
print('loot module: host DSO load/API, NODELETE, fail-closed guard, inert until armed, no injector, heap-only callback write: OK')
