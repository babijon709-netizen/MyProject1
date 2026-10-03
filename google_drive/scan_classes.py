import struct, time
t0 = time.time()
blob = open('benzhack_blob.bin','rb').read()
BLOB = 0x6df636d000
TD0 = 0x1721bbc; TDSZ = 82; NTD = 31704
POOL = 0xe7d14; PSZ = 3284884
td_names = []
for i in range(NTD):
    o = TD0 + i*TDSZ
    n, ns = struct.unpack_from('<2I', blob, o)
    td_names.append((n, ns))
expected = {}
for ti in range(NTD):
    expected[BLOB + TD0 + ti*TDSZ] = ti
found = {}
for f, base in (('benzhack_ptr1.bin', 0x6dd0000000), ('benzhack_ptr3.bin', 0x6d800eb738)):
    data = open(f,'rb').read()
    L = len(data)
    for p in range(0, L - 0x98, 8):
        v = struct.unpack_from('<Q', data, p + 0x90)[0]
        ti = expected.get(v)
        if ti is None: continue
        nma = struct.unpack_from('<Q', data, p + 0x80)[0] & 0x00FFFFFFFFFFFFFF
        nsa = struct.unpack_from('<Q', data, p + 0x88)[0] & 0x00FFFFFFFFFFFFFF
        n, ns = td_names[ti]
        if nma != BLOB + POOL + n: continue
        if nsa != BLOB + POOL + ns: continue
        if ti not in found:
            found[ti] = (f, base + p)
print(str(t0) + ' ' + str(time.time()-t0) + 's; class count: ' + str(len(found)), flush=True)
NL = chr(10); TAB = chr(9); NUL = b'\x00'
with open('classes_found.txt','w') as fo:
    for ti in sorted(found):
        f, addr = found[ti]
        n, ns = td_names[ti]
        se = blob.find(NUL, POOL + ns)
        e2 = blob.find(NUL, POOL + n)
        name = blob[POOL+ns:se].decode('utf-8','replace') + '.' + blob[POOL+n:e2].decode('utf-8','replace')
        fo.write(str(ti) + TAB + name + TAB + f + TAB + '0x' + format(addr, 'x') + NL)
print('classes_found.txt: ' + str(len(found)))
