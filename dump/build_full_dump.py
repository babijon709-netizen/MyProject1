import struct, json, re, collections, bisect
blob = open('../global-metadata.dat','rb').read()
POOL = 0xe7d14; PSZ = 3284884; TD0 = 0x1721bbc; TDSZ = 82; NTD = 31704
def s32(idx):
    if idx < 0 or idx + 1 > PSZ: return '?'
    e = blob.find(b'\x00', POOL + idx)
    return blob[POOL+idx:e].decode('utf-8','replace')
# --- раскладка TypeDef v39 (эталонная, сверена с runtime) ---
types = []
bounds = []
for i in range(NTD):
    o = TD0 + i*TDSZ
    n, ns, byval, decl, parent = struct.unpack_from('<5I', blob, o)
    gcIdx = struct.unpack_from('<H', blob, o + 0x14)[0]
    flags = struct.unpack_from('<I', blob, o + 0x16)[0]
    fs, ms, es, ps, nts, ifs, vts, ios = struct.unpack_from('<8I', blob, o + 0x1A)
    mc, pc, fc = struct.unpack_from('<HHH', blob, o + 0x3A)
    types.append([s32(n), s32(ns), byval, decl, parent, gcIdx, flags, fs, ms, es, ps, nts, ifs, vts, ios, mc, pc, fc])
    if mc: bounds.append((ms, mc, i))
bounds_sorted = sorted(bounds)
ms_list = [b[0] for b in bounds_sorted]
def ti_of_md(md):
    idx = bisect.bisect_right(ms_list, md) - 1
    if idx < 0: return None
    ms, mc, ti = bounds_sorted[idx]
    return ti if md < ms + mc else None
md_names = [s32(struct.unpack_from('<I', blob, 0x4bd338 + i*0x20)[0]) for i in range(284191)]
img_of = [0]*NTD
for line in open('03_types.txt'):
    m = re.match(r'\[image (\d+)\] typeDef (\d+)', line)
    if m: img_of[int(m.group(2))] = int(m.group(1))
img_names = []
for line in open('02_assemblies_images.txt'):
    m = re.match(r'^\s{2}(\S.*?)\s+version=', line)
    if m: img_names.append(m.group(1))
m2code = {int(k): v for k, v in json.load(open('../google_drive/m2code.json')).items()}
loaded = set()
for fn in ('../google_drive/classes_found.txt',):
    for line in open(fn):
        loaded.add(int(line.split('\t')[0]))
print('loaded:', len(loaded), 'm2code:', len(m2code))
src = open('04_dump.cs').read().splitlines()
out = []
cur_td = None
mdq = collections.deque()
avail = collections.Counter()
in_class = False
n_rvas = 0; loaded_marked = 0; n_methods_seen = 0; n_namefail = 0
rva_by_type = collections.Counter()
cls_re = re.compile(r'^\s+(?:public |private |internal |protected )*(?:static )?(?:abstract )?(?:sealed )?(?:class|interface|struct) \S+')
meth_re = re.compile(r'^\s+(\S.*?)\s\{\}\s*$')
for line in src:
    mh = re.match(r'// Image: (.+?)  TypeDefIndex: (\d+)', line)
    if mh:
        cur_td = int(mh.group(2))
        ms, mc = types[cur_td][8], types[cur_td][15]
        mdq = collections.deque(range(ms, ms + mc)) if mc else collections.deque()
        avail = collections.Counter(md_names[ms:ms+mc]) if mc else collections.Counter()
        in_class = False
        out.append(line)
        continue
    out.append(line)
    if cur_td is None: continue
    if cls_re.match(line):
        in_class = True
        if cur_td in loaded:
            out[-1] = line.rstrip() + '   // [LOADED]'
            loaded_marked += 1
        continue
    if not in_class: continue
    mm = meth_re.match(line)
    if mm:
        n_methods_seen += 1
        rline = line.rstrip()
        assert rline.endswith('{}')
        bod = rline[:-2].rstrip()
        p = bod.rfind(')')
        depth = 0; q = None
        for j in range(p, -1, -1):
            if bod[j] == ')': depth += 1
            elif bod[j] == '(':
                depth -= 1
                if depth == 0: q = j; break
        mname = bod[:q].rstrip().split()[-1] if q else None
        md = None
        if mname and avail.get(mname):
            avail[mname] -= 1
            md = mdq.popleft() if mdq else None
        elif mname:
            n_namefail += 1
        if md is not None and md in m2code:
            code, off = m2code[md]
            if off is not None:
                out.append('            // RVA(file) = 0x' + format(off,'x') + '   run7598 = 0x' + format(code, '012x'))
                n_rvas += 1
                rva_by_type[cur_td] += 1
print('methods:', n_methods_seen, 'name-fail:', n_namefail, 'RVA:', n_rvas, 'loaded:', loaded_marked)
open('06_full_dump.cs','w').write('\n'.join(out))
script = {}
for md, (code, off) in sorted(m2code.items()):
    if off is None: continue
    ti = ti_of_md(md)
    if ti is None: continue
    t = types[ti]
    full = (t[1] + '.' if t[1] else '') + t[0]
    imgn = img_names[img_of[ti]] if img_of[ti] < len(img_names) else 'image' + str(img_of[ti])
    script['0x' + format(off,'x')] = imgn + '/' + full + '::' + md_names[md]
json.dump(script, open('07_script.json','w'))
print('script.json:', len(script))
# 08
out8 = {}
for md in sorted(m2code):
    code, off = m2code[md]
    ti = ti_of_md(md)
    if ti is None: continue
    nm, ns, ms, mc = types[ti][0], types[ti][1], types[ti][8], types[ti][15]
    full = (ns + '.' if ns else '') + nm
    imgn = img_names[img_of[ti]] if img_of[ti] < len(img_names) else 'image' + str(img_of[ti])
    out8[str(md)] = {'name': md_names[md], 'type': imgn + '/' + full,
                     'runtime_7598': hex(code) if code else None,
                     'file_off': hex(off) if off is not None else None}
json.dump(out8, open('08_methods.json','w'), indent=1)
print('08_methods.json:', len(out8))
cov = 0; full_t = 0
for ti, t in enumerate(types):
    if not t[15] or t[8] >= 0xFFFF0000: continue
    have = sum(1 for md in range(t[8], t[8]+t[15]) if md in m2code)
    if have: cov += 1
    if have == t[15]: full_t += 1
print('типов частично:', cov, 'полностью:', full_t)
open('09_dump_summary.txt','w').write(
'Полный дамп global-metadata.dat (v39) + runtime-информация (ран 7598)\n'
'==========================================================================\n\n'
'Ассембли:             201\n'
'Типов (TypeDef):      31 704\n'
'Загруженных классов:  28 057 (88%; остальные — generic-инстанциации, в захвате нет)\n'
'Методов (метаданные): 284 191\n'
'Методов с кодом:       81 692 (28.7%; у 81 672 — RVA, 20 — null/абстрактные)\n'
'Полей:                140 070 (имена/типы — в 06; offsets — нет)\n\n'
'ФАЙЛЫ\n-----\n'
'06_full_dump.cs  C#-дамп ВСЕХ типов.\n'
'                 [LOADED]     — класс загружен в ране 7598 (28 057 шт).\n'
'                 // RVA(file) — сдвиг тела метода в libil2cpp.so (== ELF RVA:\n'
'                 //              RX-сегмент, off == vaddr); 81 672 метки.\n'
'                 // run7598    — абсолютный адрес метода в ран-тайме рана 7598.\n'
'07_script.json   формат Il2CppDumper: "0x<file_off>" -> "Assembly/Type::Method"\n'
'                 (81 479 записей) — для il2cpp-tamper / дизассемблеров.\n'
'08_methods.json  md-index -> {name, type, runtime_7598, file_off}, 81 692 записи.\n'
'01..05           первичные артефакты парсера (.dat: шапка, ассембли, типы).\n\n'
'ВЕРИФИКАЦИЯ\n-----------\n'
'* Каждая запись RAM-пула проверена дважды: токен 0x06<MD-index> на +0x20 и\n'
'  имя метода (указатель на строку .dat) на +0x48 — 81 692/81 692.\n'
'* Все 81 672 RVA лежат в RX-сегменте .so; выборка 300/300 декодируется в\n'
'  чистый ARM64 (capstone).\n'
'* Блок PlayerManager (784 метода): имена 1:1 с метаданными, RVA у 783/784.\n'
'* Особенность .so (протектор): тела методов склеены в крупные функции;\n'
'  метод-указатель = точка входа ВНУТРИ функции (stub b #.. / прямой вход).\n'
'  Для дизассемблера точка входа корректна (туда прыгает runtime).\n\n'
'ОГРАНИЧЕНИЯ (зачем нужен re-run v2.6)\n-------------------------------------\n'
'1. RVAs у ~71% методов: их пулы лежали вне 64MB-окон ptr1/ptr3. В v2.6 окна\n'
'   снимаются на весь размах + sparse-захват остальных heap-регионов\n'
'   (benzhack_heapN.bin) -> после re-run ожидается ~100%.\n'
'2. Field offsets: нужны benzhack_fo.bin из того же re-run (v2.5+).\n'
'3. Generic-инстанциации (3 647 типов) не загружены в ране 7598.\n')
print('09 готов')
