#!/usr/bin/env python3
"""Optional ARM64 execution checks against the supported unmodified release image.

Requires unicorn. No game images are checked in. This validates arithmetic,
not live Android ownership, timing, server acceptance or anti-cheat safety.
Usage: check_native.py release/libil2cpp.so
"""
import mmap
import struct
import sys
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_UNMAPPED
from unicorn.arm64_const import (UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X30,
                                UC_ARM64_REG_SP, UC_ARM64_REG_PC, UC_ARM64_REG_S0,
                                UC_ARM64_REG_S1, UC_ARM64_REG_CPACR_EL1)

BUILDS = [
    ('release', 'd85e63cd8dcbd481695012ef2e284f0eeb9ef307',
     0x7348ee0, 0x7342560, 0x73425d8, 0xc891c50, 0xc891d18,
     [0x660c214, 0x73425c8, 0x73425f0, 0x5b5cad4], [0xfd000120,0x1e214c40,0x1e210800,0xbd495100]),
]

def bits(f): return struct.unpack('<I',struct.pack('<f',f))[0]
def number(i): return struct.unpack('<f',struct.pack('<I',i))[0]

class Emulator:
    def __init__(self, data, random_range, random_value):
        assert data[:6] == b'\x7fELF\x02\x01'
        self.data = data
        self.segments = []
        phoff = struct.unpack_from('<Q',data,0x20)[0]
        entsize, count = struct.unpack_from('<HH',data,0x36)
        for i in range(count):
            kind, flags, off, va, pa, filesz, memsz, align = struct.unpack_from('<II6Q',data,phoff+i*entsize)
            if kind == 1: self.segments.append((va,off,filesz,memsz))
        self.uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        self.uc.reg_write(UC_ARM64_REG_CPACR_EL1, 3 << 20)
        self.uc.mem_map(0x30000000,0x10000)
        self.uc.mem_map(0x40000000,0x20000)
        self.uc.hook_add(UC_HOOK_MEM_UNMAPPED,self.unmapped)
        self.uc.hook_add(UC_HOOK_CODE,self.code)
        self.random_range, self.random_value = random_range, random_value
        self.quantile = 0.37
    def at(self,va,n):
        for start,off,filesz,memsz in self.segments:
            if start <= va and va+n <= start+filesz: return self.data[off+va-start:off+va-start+n]
        raise AssertionError(hex(va))
    def unmapped(self,u,access,address,size,value,user):
        page=address & ~0xfff
        if not any(start<=address<start+memsz for start,off,filesz,memsz in self.segments):
            raise AssertionError(f'unexpected native access {address:x}')
        u.mem_map(page,0x1000)
        for start,off,filesz,memsz in self.segments:
            lo,hi=max(start,page),min(start+filesz,page+0x1000)
            if hi>lo: u.mem_write(lo,self.data[off+lo-start:off+hi-start])
        return True
    def code(self,u,address,size,user):
        if address == self.random_range:
            a,b=number(u.reg_read(UC_ARM64_REG_S0)),number(u.reg_read(UC_ARM64_REG_S1))
            u.reg_write(UC_ARM64_REG_S0,bits(a+(b-a)*self.quantile))
            u.reg_write(UC_ARM64_REG_PC,u.reg_read(UC_ARM64_REG_X30))
        elif address == self.random_value:
            u.reg_write(UC_ARM64_REG_S0,bits(self.quantile))
            u.reg_write(UC_ARM64_REG_PC,u.reg_read(UC_ARM64_REG_X30))
    def put(self,a,fmt,*v): self.uc.mem_write(a,struct.pack('<'+fmt,*v))
    def call(self,pc,obj,flag,s0=0.,s1=0.):
        u=self.uc
        u.reg_write(UC_ARM64_REG_X0,obj); u.reg_write(UC_ARM64_REG_X1,flag)
        u.reg_write(UC_ARM64_REG_S0,bits(s0)); u.reg_write(UC_ARM64_REG_S1,bits(s1))
        u.reg_write(UC_ARM64_REG_SP,0x40010000); u.reg_write(UC_ARM64_REG_X30,0x50000000)
        u.emu_start(pc,0x50000000,count=10000)
        assert u.reg_read(UC_ARM64_REG_PC)==0x50000000, 'native call did not return'
        return number(u.reg_read(UC_ARM64_REG_S0)),number(u.reg_read(UC_ARM64_REG_S1))

def check(path,build):
    name,buildid,recoil_fn,dynamic_fn,base_fn,rng,value,sites,words=build
    with open(path,'rb') as file, mmap.mmap(file.fileno(),0,access=mmap.ACCESS_READ) as data:
        assert data[0x318:0x32c].hex()==buildid
        e=Emulator(data,rng,value)
        for site,word in zip(sites,words): assert e.at(site,4)==struct.pack('<I',word)
        obj,rc,wc,dc,calc=0x30000000,0x30001000,0x30002000,0x30003000,0x30004000
        e.put(obj,'QQfi',rc,wc,0.8,1)
        e.put(wc+0x40,'f',0.4)
        e.put(calc+0x10,'QQf',dc,wc,2.)
        e.put(dc+0x18,'10f',0.1,6.,0.2,5.,0.3,0.4,0.8,0.5,0.6,0.7)
        e.put(rc+0x18,'6f',0.4,0.8,2.,3.,0.1,0.2)
        # Nonzero controls ensure the emulation isn't returning zero blindly.
        assert e.call(recoil_fn,obj,1)!=(0.,0.)
        assert e.call(dynamic_fn,calc,0,1.,1.)[0]>0
        assert e.call(base_fn,calc,0)[0]>0
        # Only the exact heap fields used by the controller change.
        e.put(rc+0x18,'6f',*([0.]*6))
        e.put(dc+0x20,'2f',0.,0.)
        e.put(calc+0x20,'f',0.)
        for q in (0.,0.2,0.8,1.):
            e.quantile=q
            for first in (0,1):
                for horizontal,direction in ((0.8,1),(-0.6,-1),(0.,0)):
                    e.put(obj+0x10,'fi',horizontal,direction)
                    assert e.call(recoil_fn,obj,first)==(0.,0.)
            for ads in (0,1):
                for movement in (0.,0.5,8.):
                    for fire in (0.,0.8,50.):
                        assert e.call(dynamic_fn,calc,ads,fire,movement)[0]==0.
                        assert e.call(base_fn,calc,ads)[0]==0.
        print(name+': original native recoil/spread arithmetic with zeroed heap fields: OK')

if __name__=='__main__':
    assert len(sys.argv)==2, __doc__
    for path,build in zip(sys.argv[1:],BUILDS): check(path,build)
