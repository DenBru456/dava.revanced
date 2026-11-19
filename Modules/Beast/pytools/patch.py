import os, shutil, struct, bisect, re

patches = {
    'ernst-64.exe': {'size': 42665984, 'offset': 0x5eeb0, 'orig': b'\x48\x83\xec\x58\x48', 'patch': b'\xb0\x01\xc3'},
    'uvtool-64.exe': {'size': 28335616, 'offset': 0x5700, 'orig': b'\x48\x83\xec\x58\x48', 'patch': b'\xb0\x01\xc3'},
    'filtertool-64.exe': {'size': 7587328, 'offset': 0x18370, 'orig': b'\x48\x83\xec\x58\x48', 'patch': b'\xb0\x01\xc3'},
    'thebeast.exe': {'size': 23266304, 'offset': 0x120710, 'orig': b'\x55\x8b\xec\xa1\xe4', 'patch': b'\xb0\x01\xc2\x04\x00'},
    'thebeast-64.exe': {'size': 23266304, 'offset': 0x120710, 'orig': b'\x55\x8b\xec\xa1\xe4', 'patch': b'\xb0\x01\xc2\x04\x00'},
    'ernst-32.exe': {'size': 32405504, 'offset': 0x23f740, 'orig': b'\x55\x8b\xec\xa1\xa8', 'patch': b'\xb0\x01\xc2\x04\x00'},
    'filtertool-32.exe': {'size': 5916672, 'offset': 0x5f540, 'orig': b'\x55\x8b\xec\xa1\xa8', 'patch': b'\xb0\x01\xc2\x04\x00'},
}

def dynamic_patch_rc64(fn):
    if not os.path.exists(fn): return
    d = bytearray(open(fn, 'rb').read())
    str_offset = d.find(b'License key corrupted\0')
    if str_offset == -1: return

    pe = struct.unpack_from('<I', d, 0x3c)[0]
    nsec = struct.unpack_from('<H', d, pe+6)[0]
    osz = struct.unpack_from('<H', d, pe+20)[0]
    opt = pe + 24
    secs = [struct.unpack_from('<8sIIII', d, opt+osz+i*40) for i in range(nsec)]

    def rva_of(fo):
        for n,vs,va,rs,rp in secs:
            if rp <= fo < rp+rs: return va + fo - rp
        return 0
    def fo_of(rva):
        for n,vs,va,rs,rp in secs:
            if va <= rva < va+max(vs,rs): return rva - va + rp
        return 0

    str_rva = rva_of(str_offset)
    T = [s for s in secs if s[0].startswith(b'.text')][0]
    _,tvs,tva,trs,trp = T

    refs = []
    for i in range(trp, trp+trs-7):
        if d[i] in (0x48, 0x4c) and d[i+1] == 0x8d and (d[i+2] & 0xc7) == 0x05:
            disp = struct.unpack_from('<i', d, i+3)[0]
            if rva_of(i) + 7 + disp == str_rva: refs.append(i)
    if not refs: return

    pd = [s for s in secs if s[0].startswith(b'.pdata')]
    if not pd: return
    _,pvs,pva,prs,prp = pd[0]
    funcs = []
    for i in range(0, pvs-11, 12):
        b,e,u = struct.unpack_from('<III', d, prp+i)
        if b == 0: break
        funcs.append((b,e))
    starts = [f[0] for f in funcs]

    def get_func(ins_rva):
        idx = bisect.bisect_right(starts, ins_rva) - 1
        return funcs[idx][0]
    def get_callers(target_func_rva):
        out = []
        for j in range(trp, trp+trs-5):
            if d[j] == 0xe8:
                disp = struct.unpack_from('<i', d, j+1)[0]
                if rva_of(j) + 5 + disp == target_func_rva:
                    out.append(rva_of(j))
        return out

    f1_rva = get_func(rva_of(refs[0]))
    callers = get_callers(f1_rva)
    if not callers: return
    f2_rva = get_func(callers[0])

    fo = fo_of(f2_rva)
    patch = b'\xb0\x01\xc3'

    if d[fo:fo+3] == patch:
        print(f"Already patched: {fn}")
        return

    # Check that it's a typical prologue (sub rsp, XXX)
    if d[fo] == 0x48 and d[fo+1] == 0x81 and d[fo+2] == 0xec:
        shutil.copy2(fn, fn + '.bak')
        d[fo:fo+3] = patch
        open(fn, 'wb').write(d)
        print(f"SUCCESS: Dynamically patched {fn} (backup saved as {fn}.bak)")
    else:
        print(f"Skipped {fn} (found offset {hex(fo)} but unexpected prologue)")

def dynamic_patch_rc32(fn):
    if not os.path.exists(fn): return
    d = bytearray(open(fn, 'rb').read())
    str_offset = d.find(b'License key corrupted\0')
    if str_offset == -1: return
    
    pe = struct.unpack_from('<I', d, 0x3c)[0]
    nsec = struct.unpack_from('<H', d, pe+6)[0]
    osz = struct.unpack_from('<H', d, pe+20)[0]
    opt = pe + 24
    secs = [struct.unpack_from('<8sIIII', d, opt+osz+i*40) for i in range(nsec)]
    
    def rva_of(fo):
        for n,vs,va,rs,rp in secs:
            if rp <= fo < rp+rs: return va + fo - rp
        return 0
        
    str_rva = rva_of(str_offset)
    imgbase = struct.unpack_from('<I', d, opt+28)[0]
    str_va = imgbase + str_rva

    T = [s for s in secs if s[0].startswith(b'.text')][0]
    _,tvs,tva,trs,trp = T
    
    refs = [trp + m.start() for m in re.finditer(b'\x68' + struct.pack('<I', str_va), d[trp:trp+trs])]
    if not refs: return
    
    start = refs[0]
    while start > trp:
        if d[start-1] in (0xc3, 0xcc) and d[start] != 0xcc: break
        start -= 1
        
    patch = b'\xb0\x01\xc2\x04\x00'
    if d[start:start+5] == patch:
        print(f"Already patched: {fn}")
        return
        
    if d[start] == 0x55 and d[start+1] == 0x8b and d[start+2] == 0xec:
        shutil.copy2(fn, fn + '.bak')
        d[start:start+5] = patch
        open(fn, 'wb').write(d)
        print(f"SUCCESS: Dynamically patched {fn} (backup saved as {fn}.bak)")
    else:
        print(f"Skipped {fn} (found offset {hex(start)} but unexpected prologue)")


print("Applying patches...")
for fn, info in patches.items():
    if not os.path.exists(fn): continue
    size = os.path.getsize(fn)
    if size != info['size']:
        print(f"Skipped {fn} (unexpected size: {size} bytes)")
        continue
    d = bytearray(open(fn, 'rb').read())
    off, orig, patch = info['offset'], info['orig'], info['patch']
    if d[off:off+len(patch)] == patch:
        print(f"Already patched: {fn}")
        continue
    if d[off:off+len(orig)] != orig:
        print(f"Skipped {fn} (original bytes mismatch at {hex(off)})")
        continue
    shutil.copy2(fn, fn + '.bak')
    d[off:off+len(patch)] = patch
    open(fn, 'wb').write(d)
    print(f"SUCCESS: Patched {fn} (backup saved as {fn}.bak)")

dynamic_patch_rc64('rendertoolcontroller-64.exe')
dynamic_patch_rc32('rendertoolcontroller-32.exe')
print("\nDone! You can now use any 40-character key (e.g. 40 zeros).")
