import struct, json, time, sys
t0 = time.time()

# --- blob (identical to run 7598) ---
blob = open('benzhack_blob.bin','rb').read()
POOL = 0xe7d14; PSZ = 3284884; NMD = 284191
BLOB_NEW = 0x6dfaec8000  # run 7619

def md_name(md):
    ni = struct.unpack_from('<I', blob, 0x4bd338 + md*0x20)[0]
    e = blob.find(b'\x00', POOL + ni)
    return blob[POOL+ni:e]
names = [md_name(i) for i in range(NMD)]

m2code = {}   # md -> (code_va, file_off or None)
pos_of = {}   # md -> (region, off_in_region)  first hit
checked = dup = 0
stats = {}

def scan_region(name, base, data):
    global checked, dup
    n = 0
    L = len(data)
    # u32 view
    mv = memoryview(data)
    arr = mv.cast('I')
    for i in range(4, len(arr)):
        v = arr[i]
        if (v & 0xFF000000) != 0x06000000:
            continue
        md = v & 0xffffff
        if md >= NMD:
            continue
        o2 = i * 4 - 0x20
        if o2 + 0x58 > L:
            continue
        nma = struct.unpack_from('<Q', data, o2 + 0x48)[0] & 0x00FFFFFFFFFFFFFF
        na = nma - BLOB_NEW - POOL
        if na < 0 or na + 1 > PSZ:
            continue
        e = blob.find(b'\x00', POOL + na)
        if blob[POOL+na:e] != names[md]:
            continue
        checked += 1
        code = struct.unpack_from('<Q', data, o2 + 0x30)[0] & 0x00FFFFFFFFFFFFFF
        if md in m2code:
            dup += 1
            # keep better: prefer nonzero code
            old_c, old_f = m2code[md]
            if old_c == 0 and code != 0:
                m2code[md] = (code, code2off(code))
            continue
        n += 1
        m2code[md] = (code, code2off(code))
        pos_of[md] = (name, base + o2)
    stats[name] = n
    print(f'  {name}: +{n} (checked {checked})', flush=True)

def code2off(a):
    # run 7598 chunk constants (from m2code.json): C1=0x6e0f8e3230, C2=0x6e0f927230
    # run 7619: shift unknown yet; use placeholders filled after shift detection
    for C in CHUNKS:
        f = a - C
        if 0 <= f < FSZ:
            return f
    return None

FSZ = 235607232
CHUNKS = []  # filled later; for first pass we still record code VAs

def load_sparse(path):
    d = open(path,'rb').read()
    magic, base, size, count = struct.unpack_from('<QQQQ', d, 0)
    assert magic == 0x36224245, hex(magic)
    dense = bytearray(size)
    for k in range(1, count):
        p = 4104 * k
        off = struct.unpack_from('<Q', d, p)[0]
        if off % 4096 or off + 4096 > size:
            continue
        dense[off:off+4096] = d[p+8:p+4104]
    return base, bytes(dense)

# dense regions (priority order)
DENSE = [
    ('ptr1', 0x6dd0000000, open('meta1/benzhack_ptr1.bin','rb').read()),
    ('ptr3', 0x6d800eb658, open('meta1/benzhack_ptr3.bin','rb').read()),
    ('ptr2', 0x6e31f9c000, open('meta1/benzhack_ptr2.bin','rb').read()),
    ('meta', 0x6de0000000, open('meta1/benzhack_meta.bin','rb').read()),
    ('hidden1', 0x72795000, open('meta1/benzhack_hidden1.bin','rb').read()),
    ('hidden3', 0x6c60800000, open('meta1/benzhack_hidden3.bin','rb').read()),
    ('imagedata', 0x6e1daae000, open('meta1/benzhack_imagedata.bin','rb').read()),
    ('zero1', 0x6dfaec8000, open('meta1/benzhack_zero1.bin','rb').read()),
]
for name, base, data in DENSE:
    scan_region(name, base, data)

# sparse heaps (dense-expand; skip page 0 = header)
for i in list(range(1, 8)) + list(range(8, 16)):
    path = f'meta{i<=7 and 1 or 2}/benzhack_heap{i}.bin'
    base, data = load_sparse(path)
    scan_region(f'heap{i}', base, data)
    del data

json.dump({str(k): list(v) for k, v in m2code.items()}, open('m2code3.json','w'))
json.dump({str(k): list(v) for k, v in pos_of.items()}, open('poolpos3.json','w'))
print(f'{time.time()-t0:.0f}s; checked={checked} new={len(m2code)} dup={dup}')
print('по регионам:', stats)
nz = sum(1 for v in m2code.values() if v[0] != 0)
print(f'с кодом (a!=0): {nz}; без кода: {len(m2code)-nz}')
