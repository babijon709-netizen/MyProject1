#!/usr/bin/env python3
# daemon_emu510.py — эмуляция инсталлятора драйвера 5.10.sh (inst510) на Unicorn.
# Цель: довести его до расшифровки встроенного модуля и перехватить .ko
# (init_module / finit_module / запись файла), либо увидеть его socket-протокол.
import sys, struct, os, time

from unicorn import *
from unicorn.arm64_const import *

BASE    = 0x100000          # база загрузки ET_DYN
STACK   = 0x7f0000000000
STACKSZ = 0x1000000         # 16 МБ
MMAP_BASE = 0x500000000000  # аллокатор mmap сверху вниз
HEAP    = 0x300000000000    # brk

# фейковое окружение
PPID = 173
KERNEL_RELEASE = b"5.10.240-Mahiru-v4.4-Quince-RenXMiyako+\n"
ARGV0 = b"/data/local/tmp/driver:deadbeefcafe"
ENVIRON = b"PATH=/sbin:/vendor/bin:/system/sbin:/system/bin:/system/xbin\x00" \
          b"ANDROID_ROOT=/system\x00" \
          b"ANDROID_DATA=/data\x00" \
          b"ANDROID_STORAGE=/storage\x00" \
          b"TERM=xterm-256color\x00" \
          b"HOME=/data/local/tmp\x00" \
          b"SHELL=/system/bin/sh\x00" \
          b"TMPDIR=/data/local/tmp\x00" \
          b"ANDROID_ART_ROOT=/apex/com.android.art\x00" \
          b"ANDROID_I18N_ROOT=/apex/com.android.i18n\x00" \
          b"ANDROID_TZDATA_ROOT=/apex/com.android.tzdata\x00" \
          b"BOOTCLASSPATH=/apex/com.android.art/javalib/core-oj.jar\x00" \
          b"DEX2OATBOOTCLASSPATH=/apex/com.android.art/javalib/core-oj.jar\x00" \
          b"SYSTEMSERVERCLASSPATH=/system/framework/services.jar\x00" \
          b"DOWNLOAD_CACHE=/data/cache\x00" \
          b"EXTERNAL_STORAGE=/sdcard\x00\x00"

DRIVER_PRESENT = len(sys.argv) > 2 and sys.argv[2] == 'present'
DRIVER_FAM = 12

class Emu:
    def __init__(self, path):
        self.elf = open(path, 'rb').read()
        self.uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        self.setup_segments()
        self.setup_stack()
        self.fds = {0: (b'', 'pipe'), 1: (b'', 'pipe'), 2: (b'', 'pipe')}
        self.next_fd = 10
        self.next_mmap = MMAP_BASE
        self.brk = HEAP
        self.log = []
        self.out = b''
        self.dead = False
        self.memfiles = {}      # путь -> bytes (для /proc)
        self.init_proc()
        self.libc_init()
        self.uc.hook_add(UC_HOOK_INTR, self.hook_intr)
        self.uc.hook_add(UC_HOOK_MEM_INVALID, self.hook_mem_invalid)
        self.uc.hook_add(UC_HOOK_MEM_READ, self.hook_mem_read)
        for _a in self.CMP_SITES:
            self.uc.hook_add(UC_HOOK_CODE, self.hook_cmp_130504, begin=_a, end=_a)
        if os.environ.get('TRACE_POLL'):
            self.uc.hook_add(UC_HOOK_CODE, self.hook_instr_trace, begin=0x12e000, end=0x148000)
        self.uc.hook_add(UC_HOOK_CODE, self.hook_patch_verdict, begin=0x12feb0, end=0x12feb0)
        self.blocks = []
        self.watch_blocks = {0x11bfa8}
        self.status_acc = []
        self.cmp_log = []
        self.uc.hook_add(UC_HOOK_BLOCK, self.hook_block)

    # ---------- память ----------
    def setup_segments(self):
        d = self.elf
        e_phoff, = struct.unpack_from('<Q', d, 0x20)
        e_phentsize, e_phnum = struct.unpack_from('<HH', d, 0x36)
        self.entry, = struct.unpack_from('<Q', d, 0x18)
        self.phdrs = []
        for i in range(e_phnum):
            off = e_phoff + i*e_phentsize
            p = struct.unpack_from('<IIQQQQQQ', d, off)
            self.phdrs.append(p)
        # суммарный размер образа
        maxv = 0
        for p in self.phdrs:
            if p[0] == 1:
                maxv = max(maxv, p[3] + p[6])
        imgsz = (maxv + 0xffff) & ~0xffff
        self.uc.mem_map(BASE, imgsz, UC_PROT_ALL)
        for p in self.phdrs:
            if p[0] == 1:  # PT_LOAD
                vaddr, off, filesz = BASE + p[3], p[2], p[5]
                self.uc.mem_write(vaddr, d[off:off+filesz])
        self.entry += BASE

    def setup_stack(self):
        self.uc.mem_map(STACK, STACKSZ, UC_PROT_ALL)
        sp = STACK + STACKSZ - 0x10000
        # argv/envp/auxv
        vec = []
        vec.append(ARGV0 + b'\x00')
        env = ENVIRON
        self.rand = os.urandom(16)
        # пишем строки
        def push(b):
            nonlocal sp
            sp -= len(b)
            sp &= ~0xf
            self.uc.mem_write(sp, b)
            return sp
        argv0 = push(ARGV0 + b'\x00')
        nullstr = push(b'\x00')
        enva = push(env)
        randa = push(self.rand)
        # execname для AT_EXECFN
        execfn = argv0
        sp &= ~0xf
        # auxv
        auxv = [
            (3, BASE + 0x40),      # AT_PHDR
            (4, 56),               # AT_PHENT
            (5, len([p for p in self.phdrs if True])),  # AT_PHNUM
            (6, 4096),             # AT_PAGESZ
            (7, self.entry),       # AT_BASE? нет — 7 = AT_BASE (интерпретатора) = 0
            (9, self.entry),       # AT_ENTRY
            (11, 1000), (12, 1000), (13, 1000), (14, 1000),  # uid/euid/gid/egid
            (16, 0), (17, 100),    # platform/hwcap-ish
            (25, randa),           # AT_RANDOM
            (31, execfn),          # AT_EXECFN
            (0, 0),
        ]
        auxv = [(3, BASE + 0x40), (4, 56), (5, 4), (6, 4096), (7, 0),
                (9, self.entry), (11, 0), (12, 0), (13, 0), (14, 0),
                (25, randa), (31, execfn), (0, 0)]
        total = (2 + 1) * 8 + (env.count(b'\x00')) * 8 + 8 + len(auxv) * 8 + 8
        sp -= total
        sp &= ~0xf
        self.initial_sp = sp
        self.initial_argv0 = argv0
        self.initial_envp = enva
        p = sp
        def wq(v):
            nonlocal p
            self.uc.mem_write(p, struct.pack('<Q', v))
            p += 8
        wq(argv0); wq(0)                       # argv
        envptrs = []
        es = enva
        for part in env.split(b'\x00'):
            if not part: continue
            envptrs.append(es)
            es += len(part) + 1
        for e_ in envptrs: wq(e_)
        wq(0)                                   # envp end
        for a, b in auxv: wq(a); wq(b)

    def init_proc(self):
        maps = (
            f"{BASE:#x}-{BASE+0xa000:#x} r--p 00000000 00:00 0 {ARGV0.decode()}\n"
        ).encode()
        self.memfiles = {
            b'/proc/self/exe': self.elf,
            b'/proc/self/environ': ENVIRON.rstrip(b'\x00') + b'\x00',
            b'/proc/self/cmdline': ARGV0 + b'\x00',
            b'/proc/self/maps': maps,
            b'/proc/self/status': (
                b"Name:\t" + ARGV0.split(b'/')[-1] + b"\n"
                b"Umask:\t0077\n"
                b"State:\tS (sleeping)\n"
                b"Tgid:\t1234\n"
                b"Ngid:\t0\n"
                b"Pid:\t1234\n"
                b"PPid:\t" + str(PPID).encode() + b"\n"
                b"TracerPid:\t0\n"
                b"Uid:\t0\t0\t0\t0\n"
                b"Gid:\t0\t0\t0\t0\n"
                b"FDSize:\t128\n"
                b"Groups:\t0 3003 3004 9997 \n"
                b"VmPeak:\t  12340 kB\n"
                b"VmSize:\t  12340 kB\n"
                b"VmLck:\t     0 kB\n"
                b"VmPin:\t     0 kB\n"
                b"VmHWM:\t   2340 kB\n"
                b"VmRSS:\t   2340 kB\n"
                b"RssAnon:\t   1800 kB\n"
                b"RssFile:\t    540 kB\n"
                b"RssShmem:\t      0 kB\n"
                b"VmData:\t   5678 kB\n"
                b"VmStk:\t    128 kB\n"
                b"VmExe:\t    512 kB\n"
                b"VmLib:\t   1024 kB\n"
                b"VmPTE:\t     64 kB\n"
                b"VmSwap:\t      0 kB\n"
                b"CoreDumping:\t0\n"
                b"THP_enabled:\t1\n"
                b"Threads:\t4\n"
                b"SigQ:\t1/15236\n"
                b"SigPnd:\t0000000000000000\n"
                b"ShdPnd:\t0000000000000000\n"
                b"SigBlk:\t0000000000000000\n"
                b"SigIgn:\t0000000000000000\n"
                b"SigCgt:\t0000000000000000\n"
                b"CapInh:\t0000000000000000\n"
                b"CapPrm:\t000001fffffffffb\n"
                b"CapEff:\t000001fffffffffb\n"
                b"CapBnd:\t000001fffffffffb\n"
                b"CapAmb:\t0000000000000000\n"
                b"NoNewPrivs:\t0\n"
                b"Seccomp:\t0\n"
                b"Seccomp_filters:\t0\n"
                b"Speculation_Store_Bypass:\tvulnerable\n"
                b"SpeculationIndirectBranch:\tunknown\n"
                b"Cpus_allowed:\tff\n"
                b"Cpus_allowed_list:\t0-7\n"
                b"Mems_allowed:\t1\n"
                b"Mems_allowed_list:\t0\n"
                b"voluntary_ctxt_switches:\t124\n"
                b"nonvoluntary_ctxt_switches:\t36\n"),
            b'/proc/version': b"Linux version " + KERNEL_RELEASE,
            b'/proc/' + str(PPID).encode() + b'/environ': ENVIRON.rstrip(b'\x00') + b'\x00',
            b'/proc/' + str(PPID).encode() + b'/status': b"Name:\tsh\nState:\tS (sleeping)\nPid:\t" + str(PPID).encode() + b"\n",
            b'/system/bin/sh': b"#!/system/bin/sh\n",
            b'/system/bin/linker64': open('/tmp/drv/fake_linker64','rb').read(),
        }

    def hook_block(self, uc, addr, size, user_data):
        self.blocks.append(addr)
        if len(self.blocks) > 400000:
            del self.blocks[:200000]
        if addr == 0x12fc1c:
            self.poll_mark = getattr(self, 'poll_mark', 0) + 1
            if self.poll_mark == 2:
                self.poll_blocks = list(self.blocks)
            elif self.poll_mark == 3 and hasattr(self, 'poll_blocks'):
                lo = self.poll_blocks.index(0x12fc1c)
                with open('/tmp/drv/poll_blocks.txt', 'w') as f:
                    f.write('\n'.join(hex(b) for b in self.poll_blocks))
                self.L(f'БЛОКИ ОДНОГО ОПРОСА: {len(self.poll_blocks) - lo} -> poll_blocks.txt')
                uc.emu_stop()
        if addr in self.watch_blocks:
            x29 = uc.reg_read(UC_ARM64_REG_X29)
            try:
                v = struct.unpack('<Q', bytes(uc.mem_read(x29 - 0xa0, 8)))[0]
            except Exception:
                v = 0
            self.L(f'WATCH {addr:#x}: [x29-0xa0]={v:#x} x0={uc.reg_read(UC_ARM64_REG_X0):#x} '
                   f'x1={uc.reg_read(UC_ARM64_REG_X1):#x} x2={uc.reg_read(UC_ARM64_REG_X2):#x} '
                   f'x8={uc.reg_read(UC_ARM64_REG_X8):#x}')

    def set_errno(self, e):
        try:
            self.uc.mem_write(self.errno_addr, struct.pack('<I', e))
        except Exception:
            pass

    # ---------- потоки (кооперативный планировщик) ----------
    def threads_init(self):
        self.watch_blocks = {0x11bfa8}
        import unicorn.arm64_const as ac
        self.REGS = [(getattr(ac, f'UC_ARM64_REG_X{i}')) for i in range(31)]
        self.REGS += [ac.UC_ARM64_REG_SP, ac.UC_ARM64_REG_PC]
        self.THREAD_EXIT = 0x710000000000
        try:
            self.uc.mem_map(self.THREAD_EXIT, 0x1000, UC_PROT_ALL)
            self.uc.mem_write(self.THREAD_EXIT, b'\xc0\x03\x5f\xd6')  # ret
        except UcError:
            pass
        self.threads = []
        self.cur_thread = 0
        self.yield_req = False

    def snapshot_regs(self):
        return [self.uc.reg_read(r) for r in self.REGS]

    def restore_regs(self, regs):
        for r, v in zip(self.REGS, regs):
            self.uc.reg_write(r, v)

    def thread_yield(self):
        # вернуть управление планировщику: PC=LR (после вызова), x0 уже записан hook'ом
        lr = self.uc.reg_read(UC_ARM64_REG_LR)
        self.uc.reg_write(UC_ARM64_REG_PC, lr)
        self.yield_req = True
        self.uc.emu_stop()

    def spawn_thread(self, start, arg):
        stack = self.alloc(0x400000) + 0x400000  # 4МБ стек
        t = {'alive': True, 'regs': None, 'entry': start,
             'sp': stack - 0x4000, 'arg': arg}
        self.threads.append(t)
        return len(self.threads)  # tid

    # ---------- лог ----------
    def L(self, msg):
        t = getattr(self, 'cur_thread', '?')
        line = f"[T{t} {self.uc.reg_read(UC_ARM64_REG_PC):#x}] {msg}"
        self.log.append(line)
        print(line, file=sys.stderr)

    # ---------- хуки ----------
    CMP_SITES = [0x12fc1c, 0x12feb0, 0x12fefc, 0x14591c, 0x146b2c,
                 0x130504, 0x130e80, 0x130f28, 0x13fa2c, 0x13fb60, 0x13fc30,
                 0x13fdd4, 0x13fe58, 0x13ff28, 0x1400e4, 0x140178, 0x142f08, 0x142f20,
                 0x147894, 0x147b04, 0x147fe0]

    def hook_patch_verdict(self, uc, address, size, user_data):
        x10 = uc.reg_read(UC_ARM64_REG_X10)
        x11 = uc.reg_read(UC_ARM64_REG_X11)
        self.v178 = getattr(self, 'v178', []) 
        if len(self.v178) < 400:
            self.v178.append((x10, x11))
        if x10 == 0x80 and x11 == 0:
            uc.reg_write(UC_ARM64_REG_X10, 0)
            self.patched_verdicts = getattr(self, 'patched_verdicts', 0) + 1

    def hook_instr_trace(self, uc, address, size, user_data):
        import unicorn.arm64_const as _ac
        if address == 0x12fc1c:
            self.poll_count = getattr(self, 'poll_count', 0) + 1
            if self.poll_count == 2:
                self.instr_trace = []
                self.tracing = True
            elif self.poll_count == 3:
                self.tracing = False
                uc.emu_stop()
        if getattr(self, 'tracing', False):
            regs = tuple(uc.reg_read(getattr(_ac, f'UC_ARM64_REG_X{i}')) for i in range(8, 16))
            self.instr_trace.append((address, regs))
            if len(self.instr_trace) > 3_000_000:
                self.tracing = False
                uc.emu_stop()

    def hook_cmp_130504(self, uc, address, size, user_data):
        import unicorn.arm64_const as _ac
        regs = {}
        for i in (9, 10, 11, 12, 13, 14):
            regs[i] = uc.reg_read(getattr(_ac, f'UC_ARM64_REG_X{i}')) & 0xffffffffffffffff
        self.cmp_log.append((address, regs))
        pass

    def hook_mem_read(self, uc, access, address, size, value, user_data):
        if getattr(self, 'trace_reads', False) and getattr(self, 'status_buf', None):
            if self.status_buf <= address < self.status_buf + 0x400:
                off = address - self.status_buf
                if size == 1 and len(self.status_acc) < 120:
                    regs = [uc.reg_read(getattr(__import__('unicorn.arm64_const', fromlist=['x']), f'UC_ARM64_REG_X{i}')) & 0xffffffffffffffff for i in (0,1,2,3,8,9,10,11,12,13)]
                    self.status_acc.append((off, size, uc.reg_read(UC_ARM64_REG_PC), regs))
                elif size != 1:
                    self.status_acc.append((off, size, uc.reg_read(UC_ARM64_REG_PC), None))
        return True

    def hook_mem_invalid(self, uc, access, address, size, value, user_data):
        # Лениво отображаем любую тронутую страницу (пекер ставит свой TLS/стеки
        # по фиксированным адресам).
        if access == 21:  # FETCH unmapped — дикий прыжок
            self.L(f"FETCH_INVALID {address:#x}; последние блоки: " +
                   ' '.join(hex(b) for b in self.blocks[-12:]))
            if not self.stage2_started and self.try_start_stage2():
                return False   # emu остановится, python перезапустит с entry stage2
            return False
        page = address & ~0xfff
        try:
            uc.mem_map(page, 0x2000, UC_PROT_ALL)
            self.L(f"AUTOMAP {page:#x} (access={access})")
            return True
        except UcError:
            return False

    def hook_intr(self, uc, intno, user_data):
        x8 = uc.reg_read(UC_ARM64_REG_X8)
        x16 = uc.reg_read(UC_ARM64_REG_X16)
        x0 = uc.reg_read(UC_ARM64_REG_X0)
        x1 = uc.reg_read(UC_ARM64_REG_X1)
        x2 = uc.reg_read(UC_ARM64_REG_X2)
        x3 = uc.reg_read(UC_ARM64_REG_X3)
        x4 = uc.reg_read(UC_ARM64_REG_X4)
        x5 = uc.reg_read(UC_ARM64_REG_X5)
        # fake-libc: код в x16
        if 0xF000 <= x16 < 0xF000 + len(self.libc_syms):
            ret = self.libc_call(x16 - 0xF000, x0, x1, x2, x3, x4, x5)
            if ret is not None:
                uc.reg_write(UC_ARM64_REG_X0, ret & 0xffffffffffffffff)
            return
        ret = self.syscall(x8, x0, x1, x2, x3, x4, x5)
        if ret is not None:
            uc.reg_write(UC_ARM64_REG_X0, ret & 0xffffffffffffffff)

    # ---------- строка из памяти ----------
    def cstr(self, addr, maxn=512):
        if addr == 0: return b''
        out = b''
        while len(out) < maxn:
            chunk = self.uc.mem_read(addr + len(out), 1)
            if chunk == b'\x00': break
            out += bytes(chunk)
        return out

    def write_fd(self, fd, data):
        if fd in (1, 2):
            self.out += data
            sys.stdout.write(data.decode('utf-8', 'replace'))
        else:
            fn = f'/tmp/drv/write_{fd}_{self.next_fd}.bin'
            # дописываем в словарь
            if fd in self.fds and self.fds[fd][1] == 'file':
                path, pos, blob = self.fds[fd][0], self.fds[fd][2], self.fds[fd][3]
                blob = blob[:pos] + data + blob[pos+len(data):]
                self.fds[fd] = (path, 'file', pos + len(data), blob)
            else:
                self.fds[fd] = (b'<write>', 'file', len(data), data)
            # сохранить на диск сразу
            _, _, _, blob = self.fds[fd]
            p = self.fds[fd][0]
            if p and p != b'<write>':
                open('/tmp/drv/ko_dump/' + p.decode().replace('/', '_'), 'wb').write(blob)

    # ---------- stage 2: запуск распакованного демона ----------
    def try_start_stage2(self):
        import struct as _st
        uc = self.uc
        # stage2 замаплен на 0x100000?
        try:
            if bytes(uc.mem_read(0x100000, 4)) != b'\x7fELF':
                return False
        except UcError:
            return False
        blob = bytes(uc.mem_read(0x100000, 0xa89e0))
        e_phoff, = _st.unpack_from('<Q', blob, 0x20)
        e_phentsize, e_phnum = _st.unpack_from('<HH', blob, 0x36)
        e_entry, = _st.unpack_from('<Q', blob, 0x18)
        bias = 0x100000
        # найти PT_DYNAMIC
        dynv = None
        for i in range(e_phnum):
            p = _st.unpack_from('<IIQQQQQQ', blob, e_phoff + i*e_phentsize)
            if p[0] == 2:
                dynv = p[3]
                break
        if dynv is None:
            self.L('stage2: PT_DYNAMIC не найден')
            return False
        dyn = {}
        o = dynv
        while True:
            tag, val = _st.unpack_from('<qQ', blob, o)
            if tag == 0: break
            dyn.setdefault(tag, val)
            o += 16
        strtab = dyn[5]; symtab = dyn[6]; syment = dyn.get(11, 24)
        nbucket, nchain = _st.unpack_from('<II', blob, dyn[4])
        def sym_name(idx):
            if idx == 0: return None
            so = symtab + idx*syment
            st_name = _st.unpack_from('<I', blob, so)[0]
            end = blob.index(b'\x00', strtab + st_name)
            return blob[strtab+st_name:end].decode('utf-8', 'replace')
        def sym_defval(idx):
            so = symtab + idx*syment
            _, _, _, shndx, st_value, _sz = _st.unpack_from('<IBBHQQ', blob, so)
            if shndx == 0: return None
            return bias + st_value

        # трамплины fake-libc
        TL = 0x700000000000
        try:
            uc.mem_map(TL, 0x10000, UC_PROT_ALL)
        except UcError:
            pass
        syms = self.libc_syms
        idx_of = {s: i for i, s in enumerate(syms)}
        for i in range(len(syms)):
            o_ = TL + i*12
            uc.mem_write(o_, _st.pack('<III', 0xD2800010 | ((0xF000+i) << 5), 0xD4000001, 0xD65F03C0))
        def libc_addr(name):
            i = idx_of.get(name)
            if i is None:
                self.L(f'stage2: НЕТ fake-libc символа {name}!')
                return 0
            return TL + i*12

        # применяем релокации
        rel_tables = []
        if 7 in dyn and 8 in dyn:
            rel_tables.append((dyn[7], dyn[8], 'rela'))
        if 23 in dyn and 2 in dyn:
            rel_tables.append((dyn[23], dyn[2], 'jmprel'))
        nrel = 0
        for base, size, kind in rel_tables:
            for off in range(base, base + size, 24):
                r_offset, r_info, r_addend = _st.unpack_from('<QQq', blob, off)
                rtype = r_info & 0xffffffff
                rsym = r_info >> 32
                addr = bias + r_offset
                if rtype == 1027 or rtype == 1032:      # RELATIVE / IRELATIVE
                    val = (bias + r_addend) & 0xFFFFFFFFFFFFFFFF
                elif rtype in (257, 1025, 1026):        # ABS64 / GLOB_DAT / JUMP_SLOT
                    val = sym_defval(rsym)
                    if val is None:
                        nm = sym_name(rsym)
                        val = libc_addr(nm) if nm else 0
                    val = (val + r_addend) & 0xFFFFFFFFFFFFFFFF
                else:
                    continue
                uc.mem_write(addr, _st.pack('<Q', val))
                nrel += 1
        self.L(f'stage2: применено релокаций {nrel}, entry {bias + e_entry:#x}')

        # свежий стек для stage2
        sp = STACK + 0x800000
        self.argv_ptr = sp          # argv[0] по sp
        self.envp_ptr = sp + 16
        # argc=1, argv[0], NULL, envp..., NULL, auxv
        uc.mem_write(sp, _st.pack('<QQQ', 1, self.initial_argv0, 0))
        # envp сразу после: указатель
        self.envp_ptr = sp + 24
        uc.mem_write(sp + 24, _st.pack('<Q', self.initial_envp))
        self.stage2_sp = sp
        self.stage2_entry = bias + e_entry
        self.stage2_started = True
        self.stage2_blob = blob
        uc.emu_stop()
        return True

    def run_stage2(self):
        uc = self.uc
        self.threads_init()
        uc.reg_write(UC_ARM64_REG_SP, self.stage2_sp)
        self.L(f'=== СТАРТ STAGE2 @ {self.stage2_entry:#x} ===')
        main = {'alive': True, 'regs': None, 'entry': self.stage2_entry,
                'sp': self.stage2_sp, 'arg': 0}
        self.threads.append(main)
        guard = 0
        while not self.dead:
            alive = [i for i, t in enumerate(self.threads) if t['alive']]
            if not alive:
                break
            # round-robin
            nxt = None
            for off in range(1, len(self.threads) + 1):
                i = (self.cur_thread + off) % len(self.threads)
                if self.threads[i]['alive']:
                    nxt = i; break
            self.cur_thread = nxt
            t = self.threads[nxt]
            if t['regs'] is None:
                uc.reg_write(UC_ARM64_REG_SP, t['sp'])
                uc.reg_write(UC_ARM64_REG_X0, t['arg'])
                for i in range(1, 31):
                    uc.reg_write(self.REGS[i], 0)
                uc.reg_write(UC_ARM64_REG_LR, self.THREAD_EXIT)
                start_pc = t['entry']
            else:
                self.restore_regs(t['regs'])
                start_pc = t['regs'][-1]
            self.yield_req = False
            guard += 1
            if guard > 100000:
                self.L('ПЛАНИРОВЩИК: слишком много переключений, стоп')
                break
            try:
                uc.emu_start(start_pc, self.THREAD_EXIT,
                             timeout=20*1000000, count=3_000_000)
            except UcError as e:
                pc = uc.reg_read(UC_ARM64_REG_PC)
                self.L(f'ПОТОК {nxt} UC ERROR: {e} (pc={pc:#x})')
                self.L('ПОСЛЕДНИЕ БЛОКИ: ' + ' '.join(hex(b) for b in self.blocks[-16:]))
                t['alive'] = False
                continue
            t['regs'] = self.snapshot_regs()
            pc = t['regs'][-1]
            if pc == self.THREAD_EXIT:
                self.L(f'ПОТОК {nxt} завершился, возврат x0={t["regs"][0]:#x}')
                self.L('БЛОКИ ПОТОКА ПЕРЕД ВЫХОДОМ: ' +
                       ' '.join(hex(b) for b in self.blocks[-24:]))
                t['alive'] = False
        self.L(f'STOP: живых потоков {sum(1 for t in self.threads if t["alive"])}, dead={self.dead}')
        self.L(f'ПАТЧЕЙ ВЕРДИКТА: {getattr(self, "patched_verdicts", 0)}')
        from collections import Counter
        self.L('ЗНАЧЕНИЯ 0x12feb0: ' + repr(Counter((a, b) for a, b in getattr(self, 'v178', [])).most_common(12)))
        tr = getattr(self, 'instr_trace', None)
        if tr:
            import pickle
            with open('/tmp/drv/poll_trace.pkl', 'wb') as f:
                pickle.dump(tr, f)
            self.L(f'ТРАССА ОПРОСА: {len(tr)} инструкций -> poll_trace.pkl')
        self.L('ПОСЛЕДНИЕ БЛОКИ: ' + ' '.join(hex(b) for b in self.blocks[-24:]))
        sys.stdout.write(self.out.decode('utf-8', 'replace'))

    # ---------- fake libc ----------
    def libc_init(self):
        self.libc_syms = open('/tmp/drv/fake_syms.txt').read().split()
        self.sym_idx = {s: i for i, s in enumerate(self.libc_syms)}
        # арена malloc
        self.arena = 0x600000000000
        try:
            self.uc.mem_map(self.arena, 0x2000000, UC_PROT_ALL)
        except UcError:
            pass
        self.arena_pos = 0x1000
        self.arena_alloc = {}   # ptr -> size
        # ячейка errno
        self.errno_addr = self.arena + 0x100 - 8
        self.stage2_started = False
        self.stage2_entry = 0
        self.stage2_sp = 0

    def arena_alloc_mem(self, n):
        n = (n + 15) & ~15
        p = self.arena + self.arena_pos
        self.arena_pos += n
        if self.arena_pos > 0x2000000 - 0x10000:
            try:
                self.uc.mem_map(self.arena + 0x2000000, 0x2000000, UC_PROT_ALL)
                self.arena_pos = 0
                self.arena += 0x2000000
                p = self.arena + self.arena_pos
                self.arena_pos += n
            except UcError:
                pass
        self.arena_alloc[p] = n
        return p

    def reg_args_iter(self, vals):
        it = iter(vals)
        def next_arg(kind):
            try: return next(it)
            except StopIteration: return 0
        return next_arg

    def va_args_iter(self, vap):
        # aarch64 va_list: {void*__stack; void*__gr_top; void*__vr_top; int __gr_offs; int __vr_offs;}
        import struct as _s
        d = bytes(self.uc.mem_read(vap, 32))
        stack, gr_top, vr_top = _s.unpack('<QQQ', d[:24])
        gr_offs, vr_offs = _s.unpack('<ii', d[24:32])
        st = {'stack': stack, 'gr': gr_offs, 'vr': vr_offs}
        def next_arg(kind):
            uc = self.uc
            if kind == 'v':
                if st['vr'] < 0:
                    addr = vr_top + st['vr']; st['vr'] += 16
                    return _s.unpack('<d', bytes(uc.mem_read(addr, 8)))[0]
                addr = (st['stack'] + 7) & ~7
                st['stack'] = addr + 8
                return _s.unpack('<d', bytes(uc.mem_read(addr, 8)))[0]
            if st['gr'] < 0:
                addr = gr_top + st['gr']; st['gr'] += 8
            else:
                addr = st['stack']; st['stack'] = addr + 8
            return _s.unpack('<Q', bytes(uc.mem_read(addr, 8)))[0]
        return next_arg

    def libc_printf(self, fmt_addr, next_arg):
        fmt = self.cstr(fmt_addr, 4096)
        out = b''
        i = 0
        while i < len(fmt):
            c = fmt[i:i+1]
            if c == b'%' and i + 1 < len(fmt):
                j = i + 1
                spec = b'%'
                while j < len(fmt) and fmt[j:j+1] in b'-+ #0123456789.lhz':
                    spec += fmt[j:j+1]; j += 1
                conv = fmt[j:j+1]
                v = next_arg('v' if conv in b'fge' else 'g')
                if conv == b's':
                    out += self.cstr(v, 4096)
                elif conv in b'di':
                    sv = v if v < 2**63 else v - 2**64
                    out += b'%d' % sv
                elif conv in b'xX':
                    out += b'%x' % v
                elif conv in b'fge':
                    out += b'%f' % v
                elif conv in b'uc':
                    out += b'%u' % v if conv == b'u' else bytes([v & 0xff])
                elif conv == b'p':
                    out += b'0x%x' % v
                elif conv == b'%':
                    out += b'%'
                else:
                    out += spec + conv
                i = j + 1
                continue
            out += c
            i += 1
        return out

    def libc_call(self, idx, x0, x1, x2, x3, x4, x5):
        uc = self.uc
        name = self.libc_syms[idx]
        if name in ('memcpy', 'memmove'):
            uc.mem_write(x0, bytes(uc.mem_read(x1, x2)))
            return x0
        if name == 'memset':
            uc.mem_write(x0, bytes([x1 & 0xff]) * x2)
            return x0
        if name in ('memcmp', 'bcmp'):
            a = bytes(uc.mem_read(x0, x2)); b = bytes(uc.mem_read(x1, x2))
            return 0 if a == b else 1
        if name == 'strlen':
            return len(self.cstr(x0, 1 << 20))
        if name == 'strcmp':
            return 0 if self.cstr(x0) == self.cstr(x1) else 1
        if name == 'strncmp':
            return 0 if self.cstr(x0)[:x2] == self.cstr(x1)[:x2] else 1
        if name == 'strcpy':
            s = self.cstr(x1) + b'\x00'
            uc.mem_write(x0, s)
            return x0
        if name == 'strchr':
            s = self.cstr(x0, 1 << 20)
            ch = x1 & 0xff
            p = s.find(bytes([ch]))
            return x0 + p if p >= 0 else 0
        if name == 'strdup':
            s = self.cstr(x0)
            p = self.arena_alloc_mem(len(s) + 1)
            uc.mem_write(p, s + b'\x00')
            return p
        if name in ('malloc',):
            return self.arena_alloc_mem(max(x0, 1))
        if name == 'calloc':
            p = self.arena_alloc_mem(x0 * x1)
            uc.mem_write(p, b'\x00' * (x0 * x1))
            return p
        if name == 'realloc':
            p = self.arena_alloc_mem(x1)
            if x0 in self.arena_alloc:
                n = min(self.arena_alloc[x0], x1)
                uc.mem_write(p, bytes(uc.mem_read(x0, n)))
            return p
        if name == 'free':
            self.arena_alloc.pop(x0, None)
            return 0
        if name in ('puts',):
            s = self.cstr(x0, 1 << 16)
            self.out += s + b'\n'
            sys.stdout.write(s.decode('utf-8', 'replace') + '\n')
            return 0
        if name in ('printf', 'vprintf'):
            vals = [x1, x2, x3, x4, x5,
                    uc.reg_read(UC_ARM64_REG_X6), uc.reg_read(UC_ARM64_REG_X7)]
            out = self.libc_printf(x0, vals)
            self.out += out
            sys.stdout.write(out.decode('utf-8', 'replace'))
            return 0
        if name in ('sprintf', 'snprintf'):
            na = self.reg_args_iter([x2, x3, x4, x5,
                    uc.reg_read(UC_ARM64_REG_X6), uc.reg_read(UC_ARM64_REG_X7)])
            out = self.libc_printf(x1, na)
            if name == 'snprintf':
                out = out[:max(0, x2 - 1)]
            uc.mem_write(x0, out + b'\x00')
            return len(out)
        if name in ('fprintf', 'vfprintf'):
            if name == 'vfprintf':
                na = self.va_args_iter(x2)
            else:
                na = self.reg_args_iter([x2, x3, x4, x5,
                        uc.reg_read(UC_ARM64_REG_X6), uc.reg_read(UC_ARM64_REG_X7)])
            out = self.libc_printf(x1, na)
            self.out += out
            sys.stdout.write(out.decode('utf-8', 'replace'))
            sys.stdout.flush()
            return 0
        if name == '__errno_location':
            return self.errno_addr
        if name == 'strerror':
            p = self.arena_alloc_mem(64)
            uc.mem_write(p, b'errno\x00')
            return p
        if name in ('exit', '_exit', 'abort', 'exit_group'):
            self.L(f"libc {name}({x0})")
            try:
                bs = self.blocks[-40:]
                self.L('ПОСЛЕДНИЕ БЛОКИ: ' + ' '.join(hex(b) for b in bs))
                import capstone
                md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_LITTLE_ENDIAN)
                lo = max(0x100000, min(bs) - 0x300); hi = min(0x1a8000, max(bs) + 0x100)
                code = bytes(uc.mem_read(lo, hi - lo))
                bsset = set(bs)
                txt = [f'{ins.address:#x}: {ins.mnemonic} {ins.op_str}' + ('  <<<' if ins.address in bsset else '')
                       for ins in md.disasm(code, lo)]
                open('/tmp/drv/abort_window.txt', 'w').write('\n'.join(txt))
                self.L(f'окно {lo:#x}..{hi:#x} -> abort_window.txt')
            except Exception as e:
                self.L(f'диагностика не удалась: {e}')
            self.dead = True
            uc.emu_stop()
            return 0
        if name in ('open', 'open64'):
            r = self.syscall(56, -100, x0, x1, 0, 0, 0)
            return r if r <= 2**63 else r - 2**64
        if name == 'openat':
            r = self.syscall(56, x0, x1, x2, 0, 0, 0)
            return r if r <= 2**63 else r - 2**64
        if name == 'close':
            return self.syscall(57, x0, 0, 0, 0, 0, 0)
        if name == 'read':
            return self.syscall(63, x0, x1, x2, 0, 0, 0)
        if name in ('write',):
            return self.syscall(64, x0, x1, x2, 0, 0, 0)
        if name == 'lseek':
            if x0 in self.fds and self.fds[x0][1] == 'mem':
                _, _, pos, blob = self.fds[x0]
                np = pos if x1 == 0 else (len(blob) if x1 == 2 else x1)
                self.fds[x0] = (self.fds[x0][0], 'mem', np, blob)
                return np
            return 0
        if name == 'mmap':
            return self.syscall(222, x0, x1, x2, x3, x4, x5)
        if name == 'munmap':
            return self.syscall(215, x0, x1, 0, 0, 0, 0)
        if name == 'mprotect':
            return 0
        def kret(v):
            # ядро вернуло -errno -> libc: вернуть -1 и записать errno
            if v >= 2**63:
                self.set_errno(2**64 - v)
                return 2**64 - 1
            return v
        if name == 'socket':
            return kret(self.syscall(198, x0, x1, x2, 0, 0, 0))
        if name == 'getsockopt':
            return kret(self.syscall(209, x0, x1, x2, x3, x4, 0))
        if name == 'setsockopt':
            return kret(self.syscall(208, x0, x1, x2, x3, x4, 0))
        if name == 'connect':
            return kret(self.syscall(203, x0, x1, x2, 0, 0, 0))
        if name == 'sendto':
            return kret(self.syscall(211, x0, x1, x2, x3, x4, 0))
        if name == 'getpid':
            return 1234
        if name == 'getppid':
            return PPID
        if name == 'prctl':
            self.L(f"libc prctl({x0}, {x1:#x})")
            return 0
        if name == 'kill':
            return self.syscall(129, x0, x1, 0, 0, 0, 0)
        if name in ('pthread_mutex_lock', 'pthread_mutex_unlock', 'pthread_mutex_init',
                    'pthread_mutex_destroy', 'pthread_mutex_trylock',
                    'pthread_cond_signal', 'pthread_cond_broadcast', 'pthread_cond_init',
                    'pthread_cond_destroy', 'pthread_attr_init', 'pthread_attr_destroy',
                    'pthread_attr_setstacksize', 'pthread_attr_setdetachstate',
                    'pthread_mutexattr_init', 'pthread_mutexattr_destroy'):
            return 0
        if name == 'pthread_join':
            return 0
        if name == 'pthread_self':
            return 0x7000
        if name in ('clock_gettime', 'clock_gettime64'):
            return self.syscall(113, x0, x1, 0, 0, 0, 0)
        if name in ('gettimeofday',):
            return 0
        if name in ('usleep', 'nanosleep', 'sleep', 'pthread_cond_wait',
                    'pthread_cond_timedwait', 'sched_yield'):
            self.thread_yield()
            return 0
        if name == 'pthread_join':
            self.thread_yield()
            return 0
        if name == 'getenv':
            key = self.cstr(x0)
            env = ENVIRON
            for part in env.split(b'\x00'):
                if part.startswith(key + b'='):
                    p = self.arena_alloc_mem(len(part) + 1)
                    uc.mem_write(p, part + b'\x00')
                    return p
            return 0
        if name == 'syscall':
            return self.syscall(x0, x1, x2, x3, x4, uc.reg_read(UC_ARM64_REG_X6), 0)
        if name == 'ioctl':
            self.L(f"ioctl({x0}, {x1:#x})")
            return 0
        if name == 'fork' or name == 'vfork':
            self.L(f"{name} -> -1")
            return -1
        if name in ('system', 'popen'):
            self.L(f"{name}({self.cstr(x0)!r}) -> -1")
            return -1
        if name in ('dlopen', 'dlsym', 'dlclose'):
            self.L(f"{name}({self.cstr(x0) if x0 else 0!r}) -> 0")
            return 0
        if name == '__system_property_get':
            self.L(f"__system_property_get({self.cstr(x0)!r})")
            if x1:
                uc.mem_write(x1, b'mahiru\x00')
            return 6
        if name == 'fopen':
            r = self.syscall(56, -100, x0, 0, 0, 0, 0)
            return r if r <= 2**63 else r - 2**64
        if name == 'fclose':
            self.fds.pop(x0, None)
            return 0
        if name == 'fwrite':
            # fwrite(ptr, size, nmemb, stream): x0=ptr, x1=size, x2=nmemb, x3=FILE*
            n = x1 * x2
            if n and n < (1 << 20):
                data = bytes(uc.mem_read(x0, n))
                self.out += data
                sys.stdout.write(data.decode('utf-8', 'replace'))
                sys.stdout.flush()
                if x3 not in (1, 2) and x3 != self.arena + 0x300:
                    self.L(f'fwrite(файл {x3:#x}): {data[:120]!r}')
            return x2
        if name == 'fflush':
            return 0
        if name == 'isatty':
            return 1
        if name == '__libc_init':
            # bionic: __libc_init(raw_args, onexit, main, structors) -> main(x0..x2)
            main = x2
            self.L(f'__libc_init: main={main:#x} — прыгаем')
            uc.reg_write(UC_ARM64_REG_X0, 1)                    # argc
            uc.reg_write(UC_ARM64_REG_X1, self.argv_ptr)        # argv
            uc.reg_write(UC_ARM64_REG_X2, self.envp_ptr)        # envp
            uc.reg_write(UC_ARM64_REG_PC, main)
            return None
        if name == 'getauxval':
            a = {3: 0x100040, 4: 56, 5: 10, 6: 4096, 9: self.stage2_entry,
                 11: 0, 12: 0, 13: 0, 14: 0, 23: 0, 25: self.arena + 0x200,
                 31: self.initial_argv0, 16: 0, 17: 0}
            return a.get(x0, 0)
        if name == '__errno':
            return self.errno_addr
        if name in ('__sF',):
            return self.arena + 0x300
        if name == '__stack_chk_fail':
            self.L('__stack_chk_fail!')
            self.dead = True
            uc.emu_stop()
            return 0
        if name == 'strerror_r':
            uc.mem_write(x1, b'errno\x00')
            return 0
        if name == 'vasprintf':
            # int vasprintf(char **strp, const char *fmt, va_list ap)
            na = self.va_args_iter(x2)
            out = self.libc_printf(x1, na)
            p = self.arena_alloc_mem(len(out) + 1)
            uc.mem_write(p, out + b'\x00')
            uc.mem_write(x0, struct.pack('<Q', p))
            self.L(f'vasprintf -> {out!r}')
            return len(out)
        if name == 'pthread_once':
            if x0:
                v = struct.unpack('<I', bytes(uc.mem_read(x0, 4)))[0]
                if v == 0:
                    uc.mem_write(x0, struct.pack('<I', 1))
                    # вызвать init(x1)
                    uc.reg_write(UC_ARM64_REG_PC, x1)
                    return None
            return 0
        if name == 'dl_iterate_phdr':
            # один объект: сам демон
            info = self.arena + 0x400
            blob = self.stage2_blob
            uc.mem_write(info, struct.pack('<QQQI', 0x100000, 0, 0x100040, 10) + b'\x00'*8)
            self.L(f'dl_iterate_phdr(cb={x0:#x})')
            uc.reg_write(UC_ARM64_REG_X0, x0)
            uc.reg_write(UC_ARM64_REG_X1, info)
            uc.reg_write(UC_ARM64_REG_X2, 120)
            uc.reg_write(UC_ARM64_REG_X3, x1)
            uc.reg_write(UC_ARM64_REG_PC, x0)
            return None
        if name == 'memchr':
            data = bytes(uc.mem_read(x0, x2))
            p = data.find(bytes([x1 & 0xff]))
            return x0 + p if p >= 0 else 0
        if name in ('syslog', 'openlog', 'closelog', 'android_set_abort_message',
                    '__cxa_atexit', 'getrlimit', 'setrlimit', 'sigemptyset'):
            if name == 'sigemptyset' and x0:
                uc.mem_write(x0, b'\x00' * 8)
            return 0
        if name == 'pthread_create':
            tid = self.spawn_thread(x2, x3)
            if x0: uc.mem_write(x0, struct.pack('<Q', tid))
            self.L(f'pthread_create(start={x2:#x}, arg={x3:#x}) -> tid {tid}')
            return 0
        if name == 'sigaction':
            return 0
        if name == 'fputc':
            ch = bytes([x0 & 0xff])
            self.out += ch
            sys.stdout.write(ch.decode('utf-8', 'replace'))
            sys.stdout.flush()
            return x0
        # прочее — лог и 0
        self.L(f"fake-libc {name}(x0={x0:#x}, x1={x1:#x}, x2={x2:#x}) -> 0")
        return 0

    # ---------- syscalls ----------
    def syscall(self, n, x0, x1, x2, x3, x4, x5):
        uc = self.uc
        n = n & 0xffffffff
        if n == 93:   # exit
            self.L(f"exit({x0})")
            self.dead = True
            uc.emu_stop()
            return 0
        if n == 94:   # exit_group
            self.L(f"exit_group({x0})")
            self.dead = True
            uc.emu_stop()
            return 0
        if n == 56:   # openat
            path = self.cstr(x1)
            self.L(f"openat({x0}, {path!r}, flags={x2:#x})")
            if b'status' in path:
                self.status_opens = getattr(self, 'status_opens', 0) + 1
                self.trace_reads = False
                self.L(f'OPEN-CTX /proc/self/status #{self.status_opens}')
                if self.status_opens == 2:
                    self.L('ТРАССА МЕЖДУ ЧТЕНИЯМИ: ' +
                           ' '.join(hex(b) for b in self.blocks[-150:]))
                    with open('/tmp/drv/status_trace.txt', 'w') as f:
                        f.write('\n'.join(hex(b) for b in self.blocks[-400:]))
            if path in self.memfiles:
                fd = self.next_fd; self.next_fd += 1
                self.fds[fd] = (path, 'mem', 0, self.memfiles[path])
                return fd
            # файлы для записи (инсталлятор пишет .ko)
            if b'/data/local/tmp/' in path or path.endswith(b'.ko'):
                fd = self.next_fd; self.next_fd += 1
                self.fds[fd] = (path, 'file', 0, b'')
                os.makedirs('/tmp/drv/ko_dump', exist_ok=True)
                return fd
            if path == b'/dev/urandom':
                fd = self.next_fd; self.next_fd += 1
                self.fds[fd] = (path, 'urandom', 0, b'')
                return fd
            self.L(f"  -> ENOENT {path!r}")
            return -2
        if n == 57:   # close
            self.fds.pop(x0, None)
            return 0
        if n == 63:   # read
            if x0 in self.fds:
                kind = self.fds[x0][1]
                if kind == 'mem' and b'status' in self.fds[x0][0]:
                    self.status_buf = x1
                    self.status_reads = getattr(self, 'status_reads', 0) + 1
                    if self.status_reads >= 3:
                        self.trace_reads = False
                if kind == 'mem':
                    _, _, pos, blob = self.fds[x0]
                    chunk = blob[pos:pos+x2]
                    uc.mem_write(x1, chunk)
                    self.fds[x0] = (self.fds[x0][0], 'mem', pos + len(chunk), blob)
                    self.L(f"read(fd={x0}, off={pos}, n={x2}) -> {len(chunk)} ({self.fds[x0][0][:40]!r})")
                    return len(chunk)
                if kind == 'urandom':
                    uc.mem_write(x1, os.urandom(x2))
                    return x2
            return 0
        if n == 64:   # write
            if x0 in self.fds or x0 in (1, 2):
                data = bytes(uc.mem_read(x1, x2))
                self.write_fd(x0, data)
                return x2
            return -9
        if n == 222:  # mmap
            addr, len_, prot, flags, fd, off = x0, x1, x2, x3, x4, x5
            if flags & 0x10:  # MAP_FIXED
                if addr == 0:
                    addr = self.alloc(len_)
                try:
                    uc.mem_map(addr, (len_ + 0xfff) & ~0xfff, UC_PROT_ALL)
                except UcError:
                    pass  # уже отображено
                if fd > 0 and fd in self.fds and self.fds[fd][1] == 'mem':
                    _, _, pos, blob = self.fds[fd]
                    chunk = blob[off:off+len_]
                    if chunk:
                        uc.mem_write(addr, chunk)
                self.L(f"mmap FIXED {addr:#x} len={len_:#x} fd={fd} off={off:#x}")
                return addr
            addr = self.alloc(len_)
            try:
                uc.mem_map(addr, (len_ + 0xfff) & ~0xfff, UC_PROT_ALL)
            except UcError:
                pass
            if fd > 0 and fd in self.fds and self.fds[fd][1] == 'mem':
                _, _, pos, blob = self.fds[fd]
                chunk = blob[off:off+len_]
                if chunk:
                    uc.mem_write(addr, chunk)
            self.L(f"mmap anon {addr:#x} len={len_:#x} fd={fd} off={off:#x} prot={prot:#x} flags={flags:#x}")
            return addr
        if n == 215:  # munmap
            try:
                uc.mem_unmap(x0, (x1 + 0xfff) & ~0xfff)
            except UcError:
                pass
            return 0
        if n == 226:  # mprotect
            return 0
        if n == 216:  # mremap
            old, oldsz, newsz = x0, x1, x2
            try:
                data = bytes(uc.mem_read(old, oldsz))
                new = self.alloc(newsz)
                uc.mem_map(new, (newsz + 0xfff) & ~0xfff, UC_PROT_ALL)
                uc.mem_write(new, data)
                self.L(f"mremap {old:#x} {oldsz:#x} -> {new:#x}")
                return new
            except UcError as e:
                self.L(f"mremap fail {e}")
                return -12
        if n == 214:  # brk
            if x0 == 0:
                return self.brk
            if x0 > self.brk:
                try:
                    uc.mem_map(self.brk, (x0 - self.brk + 0xfff) & ~0xfff, UC_PROT_ALL)
                except UcError:
                    pass
                self.brk = x0
            return self.brk
        if n == 167:  # prctl
            self.L(f"prctl({x0}, {x1:#x}, {x2:#x}) name={self.cstr(x1) if x0 == 15 else ''!r}")
            return 0
        if n == 172:  # getpid
            return 1234
        if n == 173:  # getppid
            return PPID
        if n == 174:  # getuid: 174 getuid? фактически 174 = getuid
            return 0
        if n in (175, 176, 177):  # euid...
            return 0
        if n == 129:  # kill
            self.L(f"kill({x0}, {x1})")
            if x1 == 9 and x0 == 1234:  # getpid(): самоубийство демона
                self.dead = True
                try:
                    img = bytes(uc.mem_read(0x100000, 0xa89e0))
                    open('/tmp/drv/memdump.bin', 'wb').write(img)
                    # стек и арену тоже
                    open('/tmp/drv/memdump_stack.bin', 'wb').write(
                        bytes(uc.mem_read(0x7f0000000000, 0x100000)))
                    open('/tmp/drv/memdump_arena.bin', 'wb').write(
                        bytes(uc.mem_read(self.arena, 0x100000)))
                    self.L('МЕМДАМП сохранён (image/stack/arena)')
                except Exception as e:
                    self.L(f'мемдамп не удался: {e}')
                uc.emu_stop()
            return 0
        if n == 198:  # socket
            fam, typ, proto = x0, x1, x2
            self.L(f"socket(fam={fam}, type={typ}, proto={proto})")
            if DRIVER_PRESENT:
                if fam == DRIVER_FAM and typ == 6:
                    return -0xffffffff & 0xffffffffffffffff if False else (-1338) & 0xffffffffffffffff
                if fam == DRIVER_FAM and typ == 4:
                    fd = self.next_fd; self.next_fd += 1
                    self.fds[fd] = (b'<drvsock>', 'sock', 0, b'')
                    return fd
                return (-97) & 0xffffffffffffffff  # EAFNOSUPPORT
            return (-97) & 0xffffffffffffffff
        if n in (209, 208):  # setsockopt/getsockopt
            name = 'setsockopt' if n == 208 else 'getsockopt'
            self.L(f"{name}(fd={x0}, level={x1}, optname={x2:#x}, optval={x3:#x}, optlen={x4:#x})")
            if x4:
                ln = struct.unpack('<I', bytes(uc.mem_read(x4, 4)))[0]
                if 0 < ln <= 0x200:
                    blob = bytes(uc.mem_read(x3, ln))
                    self.L(f"  optval[{ln}] = {blob.hex()}")
            return 0
        if n == 64+1:  # unused
            pass
        if n == 160:  # uname
            fields = [KERNEL_RELEASE.strip(), b'localhost', b'6.1.99', b'#1 SMP', b'aarch64', b'(none)']
            a = x0
            for i, f in enumerate([b'Linux', b'localhost', KERNEL_RELEASE.strip(), b'#1 SMP', b'aarch64', b'(none)']):
                uc.mem_write(a + i*65, f + b'\x00')
            return 0
        if n == 113:  # clock_gettime — ВИРТУАЛЬНЫЕ часы (почти стоят)
            self.virt_ns = getattr(self, 'virt_ns', 17_000_000_000) + 50_000
            uc.mem_write(x1, struct.pack('<qq', self.virt_ns // 10**9, self.virt_ns % 10**9))
            return 0
        if n == 278:  # getrandom
            uc.mem_write(x0, os.urandom(x1))
            return x1
        if n == 98:   # futex
            if (x1 & 0x7f) in (0, 9):  # WAIT / WAIT_BITSET
                self.thread_yield()
            return 0
        if n == 135:  # rt_sigprocmask
            return 0
        if n == 134:  # rt_sigaction
            return 0
        if n == 133:  # sigaltstack? на деле 133 = tgkill? нет, 131=exit; 133=tgkill
            pass
        if n == 220:  # clone
            self.L(f"clone(flags={x0:#x}) -> ENOSYS (потоков нет)")
            return -38
        if n == 435:  # clone3
            self.L("clone3 -> ENOSYS")
            return -38
        if n == 221:  # execve
            fn = self.cstr(x0)
            self.L(f"execve({fn!r}, argv={x1:#x})")
            # аргументы
            if x1:
                for i in range(8):
                    p = struct.unpack('<Q', bytes(uc.mem_read(x1 + i*8, 8)))[0]
                    if p == 0: break
                    self.L(f"  argv[{i}] = {self.cstr(p)!r}")
            self.dead = True
            uc.emu_stop()
            return 0
        if n == 169:  # init_module(blob, len, params)
            self.L(f"init_module(blob={x0:#x}, len={x1}, params={self.cstr(x2)!r})")
            blob = bytes(uc.mem_read(x0, min(x1, 0x100000)))
            os.makedirs('/tmp/drv/ko_dump', exist_ok=True)
            open('/tmp/drv/ko_dump/module_init.bin', 'wb').write(blob)
            self.L(f"  -> МОДУЛЬ СОХРАНЁН ({len(blob)} байт)")
            self.dead = True
            uc.emu_stop()
            return 0
        if n == 313:  # finit_module(fd, params, flags)
            self.L(f"finit_module(fd={x0}, params={self.cstr(x1)!r}, flags={x2:#x})")
            if x0 in self.fds and self.fds[x0][1] == 'file':
                open('/tmp/drv/ko_dump/module_finit.bin', 'wb').write(self.fds[x0][3])
                self.L(f"  -> МОДУЛЬ СОХРАНЁН из fd ({len(self.fds[x0][3])} байт)")
            self.dead = True
            uc.emu_stop()
            return 0
        if n == 48:   # faccessat
            return 0
        if n in (79, 80):  # fstat/newfstatat
            uc.mem_write(x1, b'\x00' * 128)
            return 0
        if n == 29:   # ioctl (на сокетах)
            self.L(f"ioctl(fd={x0}, cmd={x1:#x})")
            return 0
        if n == 57+1000:
            pass
        # всё прочее логируем один раз
        self.L(f"syscall #{n}(x0={x0:#x}, x1={x1:#x}, x2={x2:#x}) -> ENOSYS")
        return -38

    def alloc(self, len_):
        self.next_mmap -= (len_ + 0xfff) & ~0xfff
        self.next_mmap &= ~0xfff
        return self.next_mmap

    def run(self):
        self.uc.reg_write(UC_ARM64_REG_SP, self.initial_sp)
        t0 = time.time()
        try:
            self.uc.emu_start(self.entry, 0, timeout=120*1000000, count=200_000_000)
        except UcError as e:
            self.L(f"UC ERROR: {e} (pc={self.uc.reg_read(UC_ARM64_REG_PC):#x})")
        print(f"\n=== стоп за {time.time()-t0:.1f}с, вывод процесса ===", file=sys.stderr)
        sys.stdout.write(self.out.decode('utf-8', 'replace'))
        if self.stage2_started and self.stage2_entry:
            self.run_stage2()
        open('/tmp/drv/emu.log', 'w').write('\n'.join(self.log))

if __name__ == '__main__':
    os.makedirs('/tmp/drv/ko_dump', exist_ok=True)
    emu = Emu(sys.argv[1])
    emu.run()
