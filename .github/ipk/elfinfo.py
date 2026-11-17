#!/usr/bin/env python3
"""ELF dependency report without binutils.

For every ELF file below the given paths: SONAME, NEEDED, RPATH/RUNPATH, the
GLIBC_ versions it requires (and which symbols need the highest one), other
required symbol versions (OPENSSL_1_1_0, ...). Writes a text manifest and,
with --json, a machine readable one.

usage: elfinfo.py [--json out.json] [--max-glibc 2.19] [--bundled-dir DIR] path...
  --max-glibc  exit status 1 when a file requires a newer GLIBC_ version
  --bundled-dir  directory of the bundled libraries; NEEDED entries not found
                 there (by file name) are listed as "not bundled"
"""
import json
import os
import struct
import sys


def vkey(v):
    out = []
    for p in v.split('.'):
        try:
            out.append(int(p))
        except ValueError:
            out.append(0)
    return tuple(out)


def cstr(data, off):
    end = data.find(b'\0', off)
    return data[off:end].decode('latin-1')


def parse(path):
    with open(path, 'rb') as f:
        d = f.read()
    if d[:4] != b'\x7fELF':
        return None
    is64 = d[4] == 2
    en = '<' if d[5] == 1 else '>'
    if is64:
        (e_type, e_machine, _, _, _, e_shoff, _, _, _, _, e_shentsize, e_shnum,
         e_shstrndx) = struct.unpack_from(en + 'HHIQQQIHHHHHH', d, 16)
    else:
        (e_type, e_machine, _, _, _, e_shoff, _, _, _, _, e_shentsize, e_shnum,
         e_shstrndx) = struct.unpack_from(en + 'HHIIIIIHHHHHH', d, 16)
    secs = []
    for i in range(e_shnum):
        o = e_shoff + i * e_shentsize
        if is64:
            name, typ, flags, addr, off, size, link, info, align, entsize = struct.unpack_from(en + 'IIQQQQIIQQ', d, o)
        else:
            name, typ, flags, addr, off, size, link, info, align, entsize = struct.unpack_from(en + 'IIIIIIIIII', d, o)
        secs.append(dict(name=name, type=typ, addr=addr, off=off, size=size, link=link, info=info, entsize=entsize))
    info = dict(type={2: 'EXEC', 3: 'DYN'}.get(e_type, str(e_type)), machine=e_machine,
                soname='', needed=[], rpath='', runpath='', versions={}, glibc_max='', glibc_max_symbols=[])
    dyn = [s for s in secs if s['type'] == 6]  # SHT_DYNAMIC
    if dyn:
        s = dyn[0]
        strtab = secs[s['link']]['off']
        esz = 16 if is64 else 8
        fmt = en + ('qQ' if is64 else 'iI')
        for i in range(s['size'] // esz):
            tag, val = struct.unpack_from(fmt, d, s['off'] + i * esz)
            if tag == 0:
                break
            if tag == 1:
                info['needed'].append(cstr(d, strtab + val))
            elif tag == 14:
                info['soname'] = cstr(d, strtab + val)
            elif tag == 15:
                info['rpath'] = cstr(d, strtab + val)
            elif tag == 29:
                info['runpath'] = cstr(d, strtab + val)
    # version needs: index -> (file, version)
    vidx = {}
    for s in secs:
        if s['type'] != 0x6ffffffe:  # SHT_GNU_verneed
            continue
        strtab = secs[s['link']]['off']
        o = s['off']
        for _ in range(s['info']):
            vn_version, vn_cnt, vn_file, vn_aux, vn_next = struct.unpack_from(en + 'HHIII', d, o)
            fname = cstr(d, strtab + vn_file)
            a = o + vn_aux
            for _ in range(vn_cnt):
                vna_hash, vna_flags, vna_other, vna_name, vna_next = struct.unpack_from(en + 'IHHII', d, a)
                vname = cstr(d, strtab + vna_name)
                vidx[vna_other] = (fname, vname)
                info['versions'].setdefault(vname, set()).add(fname)
                if not vna_next:
                    break
                a += vna_next
            if not vn_next:
                break
            o += vn_next
    glibc = sorted((v for v in info['versions'] if v.startswith('GLIBC_') and v[6:7].isdigit()),
                   key=lambda v: vkey(v[6:]))
    if glibc:
        info['glibc_max'] = glibc[-1][6:]
        # symbols that need the highest version
        dsym = [s for s in secs if s['type'] == 11]  # SHT_DYNSYM
        vsym = [s for s in secs if s['type'] == 0x6fffffff]  # SHT_GNU_versym
        if dsym and vsym:
            ds, vs = dsym[0], vsym[0]
            strtab = secs[ds['link']]['off']
            esz = 24 if is64 else 16
            for i in range(ds['size'] // esz):
                if is64:
                    st_name, st_info, st_other, st_shndx, st_value, st_size = struct.unpack_from(en + 'IBBHQQ', d, ds['off'] + i * esz)
                else:
                    st_name, st_value, st_size, st_info, st_other, st_shndx = struct.unpack_from(en + 'IIIBBH', d, ds['off'] + i * esz)
                ver = struct.unpack_from(en + 'H', d, vs['off'] + i * 2)[0] & 0x7fff
                if st_shndx == 0 and ver in vidx and vidx[ver][1] == glibc[-1]:
                    info['glibc_max_symbols'].append(cstr(d, strtab + st_name))
    info['versions'] = {k: sorted(v) for k, v in info['versions'].items()}
    return info


def main():
    args = sys.argv[1:]
    jout = None
    maxg = None
    bdir = None
    paths = []
    while args:
        a = args.pop(0)
        if a == '--json':
            jout = args.pop(0)
        elif a == '--max-glibc':
            maxg = args.pop(0)
        elif a == '--bundled-dir':
            bdir = args.pop(0)
        else:
            paths.append(a)
    files = []
    for p in paths:
        if os.path.isdir(p):
            for root, dirs, names in os.walk(p):
                dirs.sort()
                for n in sorted(names):
                    files.append(os.path.join(root, n))
        else:
            files.append(p)
    report = {}
    seen = set()
    for f in files:
        if os.path.islink(f):
            continue
        real = os.path.realpath(f)
        if real in seen:
            continue
        seen.add(real)
        try:
            i = parse(f)
        except Exception as e:  # noqa: BLE001
            print('%s: not parsed (%s)' % (f, e), file=sys.stderr)
            continue
        if i:
            report[f] = i
    bundled = set()
    if bdir:
        bundled = set(os.listdir(bdir))
    bad = []
    unbundled = {}
    overall = ''
    for f, i in report.items():
        print('== %s (%s)' % (f, i['type']))
        if i['soname']:
            print('   SONAME   %s' % i['soname'])
        print('   NEEDED   %s' % ' '.join(i['needed']))
        if i['rpath']:
            print('   RPATH    %s' % i['rpath'])
        if i['runpath']:
            print('   RUNPATH  %s' % i['runpath'])
        print('   GLIBC    max %s%s' % (i['glibc_max'] or '-',
              ('  (symbols: %s)' % ' '.join(i['glibc_max_symbols'][:12])) if i['glibc_max_symbols'] else ''))
        other = sorted(v for v in i['versions'] if not v.startswith('GLIBC_'))
        if other:
            print('   VERSIONS %s' % ' '.join(other))
        if i['glibc_max'] and (not overall or vkey(i['glibc_max']) > vkey(overall)):
            overall = i['glibc_max']
        if maxg and i['glibc_max'] and vkey(i['glibc_max']) > vkey(maxg):
            bad.append(f)
        for n in i['needed']:
            if n not in bundled:
                unbundled.setdefault(n, []).append(os.path.basename(f))
    print()
    print('highest GLIBC version of all files: %s' % (overall or '-'))
    if bdir:
        print('NEEDED libraries not bundled (must come from the image):')
        for n in sorted(unbundled):
            print('   %-22s used by %s' % (n, ' '.join(sorted(set(unbundled[n])))))
    if jout:
        with open(jout, 'w') as f:
            json.dump(dict(files=report, glibc_max=overall,
                           not_bundled={k: sorted(set(v)) for k, v in unbundled.items()}),
                      f, indent=1, sort_keys=True)
    if bad:
        print('ERROR: GLIBC newer than %s required by: %s' % (maxg, ' '.join(bad)))
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
