#!/usr/bin/env python3
"""Optional ARM64 ABI evidence on unmodified images, NOT an Android loot test.
Requires Unicorn. AllowInvoke is stubbed true; callback endpoints are test doubles.
Only simulated heap delegate data changes. No image instruction is patched.
Usage: check_native.py release/libil2cpp.so
"""
import mmap
from pathlib import Path
import re
import struct
import sys
from unicorn import UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X30, UC_ARM64_REG_PC
assert len(sys.argv)==2, 'supply exactly the supported release image'
root=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(root/'tools/weapons'))
from check_native import Emulator  # reusable sparse original-ELF mapper

heads=re.findall(r'\{0x([0-9a-f]+), \{((?:0x[0-9a-f]+,?)+)\}\}',
                 (root/'experimental/loot_module/layouts.h').read_text())
assert len(heads)==16
builds=[('release','d85e63cd8dcbd481695012ef2e284f0eeb9ef307',0xc8b89b8,0x5b66400)]
for index,(path,build) in enumerate(zip(sys.argv[1:],builds)):
    name,bid,invoke,stock=build
    with open(path,'rb') as file, mmap.mmap(file.fileno(),0,access=mmap.ACCESS_READ) as data:
        assert data[0x318:0x32c].hex()==bid
        em=Emulator(data,0,0)
        for rva,words in heads[:8]:
            assert em.at(int(rva,16),16)==struct.pack('<4I',*(int(w,16) for w in words.split(',')))
        insn=struct.unpack('<I',em.at(invoke+12,4))[0]
        assert insn>>26==0b100101  # BL AllowInvoke
        imm=insn&0x3ffffff
        if imm&(1<<25): imm-=1<<26
        allow=invoke+12+imm*4
        view,action,call,mi,original_action=0x30000200,0x30000400,0x30000600,0x30000800,0x30000a00
        original_end,module_end,presenter=0x30006000,0x30006100,0x30000c00
        module_dispatch=module_end+0x100
        em.put(call+0x10,'Q',action)
        em.put(action+0x10,'Q',stock)
        em.put(action+0x18,'Q',stock)
        em.put(action+0x20,'Q',view)
        em.put(action+0x28,'Q',mi)
        em.put(action+0x40,'Q',view)
        em.put(view+0x90,'Q',original_action)
        em.put(original_action+0x18,'Q',original_end)
        em.put(original_action+0x28,'Q',mi+0x100)
        em.put(original_action+0x40,'Q',presenter)
        em.put(original_end,'I',0xd65f03c0)
        em.put(module_end,'II',0xd50324df,0x1400003f)  # own entry: BTI jc; B dispatch
        em.put(module_dispatch,'I',0xd65f03c0)
        seen=[]
        def hook(u,pc,size,user):
            if pc==allow:
                u.reg_write(UC_ARM64_REG_X0,1)
                u.reg_write(UC_ARM64_REG_PC,u.reg_read(UC_ARM64_REG_X30))
            elif pc in (original_end,module_dispatch):
                seen.append((pc,u.reg_read(UC_ARM64_REG_X0),u.reg_read(UC_ARM64_REG_X1)))
        em.uc.hook_add(UC_HOOK_CODE,hook)
        em.call(invoke,call,0)
        assert seen==[(original_end,presenter,mi+0x100)]
        seen.clear()
        em.put(action+0x18,'Q',module_end)  # simulated heap redirect, never code
        em.call(invoke,call,0)
        assert seen==[(module_dispatch,view,mi)]
        seen.clear()
        em.put(action+0x18,'Q',stock)
        em.call(invoke,call,0)
        assert seen==[(original_end,presenter,mi+0x100)]
        print(name+': original UnityAction -> (target, MethodInfo), heap redirect and restore, native function fingerprints: OK')
assert len(sys.argv)==2, 'supply exactly the supported release image'
print('NOT validated: real listener discovery, Unity lifecycle/loader, Android execution, requests or server acceptance.')
