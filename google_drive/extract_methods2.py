import struct, json, time
t0 = time.time()
blob = open('benzhack_blob.bin','rb').read()
def code2off(a):
    a &= 0x00FFFFFFFFFFFFFF
    if 0x6e149a2000 <= a < 0x6e152bc000:
        off = a - 0x6e149a2000 + 0x50bedd0
    elif 0x6e152bd000 <= a < 0x6e1cb2c000:
        off = a - 0x6e152bd000 + 0x5995DD0
    else:
        return None
    Fsz = 235607232
    return off if 0 <= off < Fsz else None
POOL = 0xe7d14; PSZ = 3284884; BLOB = 0x6df636d000
def md_name(mi):
    ni = struct.unpack_from('<I', blob, 0x4bd338 + mi*0x20)[0]
    e = blob.find(b'\x00', POOL + ni)
    return blob[POOL+ni:e].decode('utf-8','replace')
m2code = {}
checked = ok = dup = 0
for f, base in (('benzhack_ptr1.bin', 0x6dd0000000), ('benzhack_ptr3.bin', 0x6d800eb738)):
    data = open(f,'rb').read()
    for i in range(0x20, len(data) - 4, 4):
        v = struct.unpack_from('<I', data, i)[0]
        if (v & 0xFF000000) != 0x06000000: continue
        md = v & 0xffffff
        if md >= 284191: continue
        o2 = i - 0x20
        if o2 + 0x58 > len(data): continue
        nma = struct.unpack_from('<Q', data, o2 + 0x48)[0] & 0x00FFFFFFFFFFFFFF
        na = nma - BLOB - POOL
        if na < 0 or na + 1 > PSZ: continue
        e = blob.find(b'\x00', POOL + na)
        if blob[POOL+na:e].decode('utf-8','replace') != md_name(md): continue
        code = struct.unpack_from('<Q', data, o2 + 0x30)[0] & 0x00FFFFFFFFFFFFFF
        checked += 1
        if md in m2code:
            dup += 1; continue
        ok += 1
        m2code[md] = (code, code2off(code))
print(f'{time.time()-t0:.0f}с; проверено: {checked}; ok: {ok}; дублей: {dup}', flush=True)
nooff = sum(1 for v in m2code.values() if v[1] is None)
zero = sum(1 for v in m2code.values() if v[0] == 0)
print(f'итого: {len(m2code)}; без file-off: {nooff} (null: {zero})')
json.dump({str(k): list(v) for k, v in m2code.items()}, open('m2code.json','w'))
