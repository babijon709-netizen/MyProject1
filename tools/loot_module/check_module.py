#!/usr/bin/env python3
import ctypes
from pathlib import Path
import re
import subprocess
import sys

root=Path(__file__).resolve().parents[2]
src=(root/'experimental/loot_module/module.cpp').read_text()
# There is no injector/autoload, writable-code page, or global API/vtable hook.
for token in ('ptrace(', 'mprotect(', 'dlopen(', 'pthread_create(', 'std::thread', 'pwrite(', 'process_vm_writev('):
    assert token not in src, token
assert src.count('__atomic_compare_exchange_n(')==1
assert '.inst 0xd50324df' in src  # own BTI call/jump entry, not a game instruction write
assert 'object_data::heap_writable(maps,address,8)' in src
assert 'action+0x18,original,' in src
assert 'binding.action+0x18,reinterpret_cast<uint64_t>(&arena_loot_entry),binding.original' in src
assert 'r.result==Result::NotReady && r.attempted==0' in src
assert 'if(frame==last_frame) return;' in src
assert 'module.cpp' not in (root/'jni/Android.mk').read_text()
for p in ('batch.h','module.cpp'):
    code=(root/'experimental/loot_module'/p).read_text()
    assert not re.search(r'\b(?:sleep|usleep|nanosleep|WaitForSeconds)\s*\(',code)

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
lib.arena_loot_report_v1.argtypes=[ctypes.POINTER(Report),ctypes.c_uint32]
r=Report()
assert lib.arena_loot_report_v1(ctypes.byref(r),ctypes.sizeof(r))==0
assert not r.attempted and not r.delegate  # loading the DSO has no gameplay side effects
assert lib.arena_loot_probe_v1(0,0)==2
assert lib.arena_loot_probe_v1(0x10000,0)==2
assert lib.arena_loot_probe_v1(0xffffffffffffffff,0)==2
assert lib.arena_loot_install_v1(0,0)==3  # cannot execute game code on the host
assert lib.arena_loot_disable_v1()==3
assert lib.arena_loot_report_v1(ctypes.byref(r),1)==2
print('loot module: host DSO load/API, NODELETE, fail-closed platform guard, heap-only callback write, no automatic injection: OK')
