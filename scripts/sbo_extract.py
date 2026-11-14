#!/usr/bin/env python3
# divaft_Android — DIVA FTDX shader.sbo 容器解析/提取器（原创逆向分析成果）
#
# SBO 布局（2026-11-14 逆向确认，PC/PS4 两版同构）：
#   header(32B): u32 version=1, u32 count, u32 headersize=32, u32 pad,
#                u64 table_end, u64 pad
#   entry table [headersize, table_end)：变长记录
#     shader 记录: u32 blob1_off, [u32 blob2_off 若 <0x80000000], u32 ids[]（>=0x80000000 延续）
#     name 记录:   u32 name_off, u32 0, u32 0, u32 0（16B）
#     每个材质 = 3 条 shader 记录（档位）+ 1 条 name 记录；每条 shader 记录含 1-2 个数据块，
#     每数据块 = 顶点+片段 两个 shader。
#   name 字符串表：table_end 之后，NUL 结尾材质名。
#   数据块 subheader: u32 nsec, u32 ver=2, u32 sec_ids[nsec], u32 first_shader_size, u32 pad,
#     PS4 额外: u8 hash[8] + u32 f[7]；PC 无（PC 用 DXBC 魔数定位）。
#   PS4: "Shdr" 容器（version 0x20007）。hdr[2]&0xFF: 1=vertex 2=fragment。
#     code_off = u32@+24 + 16，交叉验证 = (60|80)+4*u32@+56（vertex|fragment），
#     已对全部 262 个 Shdr 验证。代码以 S_MOV_B32 vcc_hi(0xBEEB03FF)+literal 开头，
#     至首个 S_ENDPGM(0xBF810000) 结束。GCN ISA（shadPS4 shader_recompiler 可直接消费）。
#   PC: DXBC（RDEF/ISGN/OSGN/SHEX/STAT），OSGN 含 SV_Target → fragment，否则 vertex。
#
# 用法: python3 sbo_extract.py <shader.sbo[.txt]> <outdir>
# 输出: <outdir>/<material>/<tier>_<blob>_<stage>.gcn.bin|.dxbc + manifest.json
# 注意: 提取产物是游戏资产，仅本地使用，禁止提交仓库。
import json
import struct
import sys
from pathlib import Path

S_ENDPGM = 0xBF810000
PROLOGUE = 0xBEEB03FF  # s_mov_b32 vcc_hi, <literal>


def u32(d, o):
    return struct.unpack_from('<I', d, o)[0]


def parse_table(d):
    ver, cnt, hsz = struct.unpack_from('<III', d, 0)
    assert ver == 1 and hsz == 32, f'unsupported SBO header ver={ver} hsz={hsz}'
    tbl_end = struct.unpack_from('<Q', d, 16)[0]
    pos, recs = hsz, []
    while pos < tbl_end:
        a, b = struct.unpack_from('<II', d, pos)
        if b == 0:
            recs.append({'kind': 'name', 'pos': pos, 'name_off': a})
            pos += 16
            continue
        blobs = [a]
        p = pos + 8
        if b >= 0x80000000:
            p = pos + 4  # 无第二数据块，b 本身是 id
        else:
            blobs.append(b)
        ids = []
        while p < tbl_end and u32(d, p) >= 0x80000000:
            ids.append(u32(d, p))
            p += 4
        recs.append({'kind': 'shader', 'pos': pos, 'blobs': blobs, 'ids': ids})
        pos = p
    assert pos == tbl_end, f'table walk mismatch: {pos:#x} != {tbl_end:#x}'
    return recs, cnt


def read_names(d, recs):
    """扫描整个字符串区（table_end → 首个数据块），而非只看被引用的偏移。"""
    tbl_end = struct.unpack_from('<Q', d, 16)[0]
    first_data = min(r['blobs'][0] for r in recs if r['kind'] == 'shader')
    names, pos = {}, tbl_end
    while pos < first_data:
        if d[pos] == 0:
            pos += 1
            continue
        e = d.index(b'\0', pos)
        names[pos] = d[pos:e].decode('ascii')
        pos = e + 1
    return names


def group_materials(recs, names):
    """shader 记录按 name 记录分组；首材质 name 记录可能缺省，用未引用名补齐。"""
    groups, cur = [], []
    for r in recs:
        if r['kind'] == 'shader':
            cur.append(r)
        else:
            groups.append({'name_off': r['name_off'], 'shaders': cur})
            cur = []
    if cur:
        groups.append({'name_off': None, 'shaders': cur})
    referenced = {g['name_off'] for g in groups if g['name_off'] is not None}
    unused = [o for o in names if o not in referenced]
    for g in groups:
        if g['name_off'] is None:
            assert len(unused) == 1, f'cannot infer missing name: unused={unused}'
            g['name_off'] = unused.pop()
        g['name'] = names[g['name_off']]
    return groups


def ps4_code(d, shdr, container_end):
    hdr2 = u32(d, shdr + 8)
    stage = {1: 'vertex', 2: 'fragment'}.get(hdr2 & 0xFF)
    assert stage, f'unknown Shdr stage type {hdr2:#x} @ {shdr:#x}'
    # code_off = hdr6+16（已对全部 262 个 Shdr 用 prologue 校验）；
    # 表公式 (60|80)+4*n 仅部分容器成立，不作硬断言。
    code = shdr + u32(d, shdr + 24) + 16
    assert u32(d, code) == PROLOGUE, f'no prologue @ {code:#x} (got {u32(d, code):#x})'
    # 代码终点：首个 S_ENDPGM 可能是早退路径，分支会跳到其后继续执行。
    # 取「不小于最大分支目标+1 的最小 S_ENDPGM 结尾」为真实代码终点。
    ndwords = (container_end - code) // 4
    endpgms, max_target = [], 0
    for i in range(ndwords):
        w = u32(d, code + 4 * i)
        if w == S_ENDPGM:
            endpgms.append(i + 1)
        elif (w >> 23) == 0x17F:  # SOPP
            op = (w >> 16) & 0x7F
            if op in (2, 4, 5, 6, 7, 8, 9):  # S_BRANCH / S_CBRANCH_*
                simm = w & 0xFFFF
                if simm >= 0x8000:
                    simm -= 0x10000
                max_target = max(max_target, i + 1 + simm)
    ends = [e for e in endpgms if e >= max_target + 1]
    assert ends, f'no S_ENDPGM covering branch targets for Shdr @ {shdr:#x}'
    end = code + 4 * min(ends)
    return stage, d[code:end], {'shdr_off': shdr, 'code_off': code, 'hdr2': hdr2,
                                'hdr4': u32(d, shdr + 16), 'hdr6': u32(d, shdr + 24),
                                'max_branch_target': max_target}


def extract_ps4_blob(d, blob, blob_end, out: Path, tag, manifest):
    nsec = u32(d, blob)
    assert u32(d, blob + 4) == 2, 'bad blob subheader version'
    # subheader 变长，用 Shdr 魔数+版本定位（与 PC 同策）。
    data = -1
    pos = blob
    while pos < blob + 160:
        i = d.find(b'Shdr', pos, blob + 160)
        if i < 0:
            break
        if u32(d, i + 4) == 0x20007:
            data = i
            break
        pos = i + 4
    assert data >= 0, f'no Shdr @ blob {blob:#x}'
    x = struct.unpack_from('<Q', d, blob + 16 + 4 * nsec)[0]
    shdrs = []
    pos = data
    while pos < blob_end - 4:
        i = d.find(b'Shdr', pos, blob_end)
        if i < 0:
            break
        if u32(d, i + 4) == 0x20007:
            shdrs.append(i)
        pos = i + 4
    assert len(shdrs) == 2, f'expected 2 Shdr per blob, got {len(shdrs)} @ {blob:#x}'
    for k, s in enumerate(shdrs):
        cend = shdrs[k + 1] if k + 1 < len(shdrs) else blob_end
        stage, code, meta = ps4_code(d, s, cend)
        fn = f'{tag}_{stage}.gcn.bin'
        (out / fn).write_bytes(code)
        manifest.append({'file': fn, 'stage': stage, 'platform': 'ps4',
                         'code_bytes': len(code), 'code_dwords': len(code) // 4,
                         'blob_off': blob, 'first_size_field': x, **meta})


def extract_pc_blob(d, blob, blob_end, out: Path, tag, manifest):
    assert u32(d, blob + 4) == 2, 'bad blob subheader version'
    # PC subheader 变长（nsec 语义与 PS4 略异），直接用魔数定位首个 DXBC。
    data = d.find(b'DXBC', blob, blob + 128)
    assert data >= 0, f'no DXBC near blob @ {blob:#x}'
    pos = data
    while pos < blob_end - 4:
        i = d.find(b'DXBC', pos, blob_end)
        if i < 0:
            break
        total = u32(d, i + 24)
        nch = u32(d, i + 28)
        stage = 'vertex'
        for c in range(nch):
            co = u32(d, i + 32 + 4 * c)
            if d[i + co:i + co + 4] == b'OSGN':
                osz = u32(d, i + co + 4)
                if b'SV_Target' in d[i + co + 8:i + co + 8 + osz]:
                    stage = 'fragment'
        fn = f'{tag}_{stage}.dxbc'
        (out / fn).write_bytes(d[i:i + total])
        manifest.append({'file': fn, 'stage': stage, 'platform': 'pc',
                         'code_bytes': total, 'blob_off': blob, 'dxbc_off': i})
        pos = i + total


def main():
    src, outdir = Path(sys.argv[1]), Path(sys.argv[2])
    d = src.read_bytes()
    platform = 'ps4' if b'Shdr' in d[:0x2000] else 'pc'
    recs, cnt = parse_table(d)
    names = read_names(d, recs)
    groups = group_materials(recs, names)
    nsh = sum(len(g['shaders']) for g in groups)
    print(f'platform={platform} records={len(recs)} shader_records={nsh} '
          f'materials={len(groups)} names={len(names)} count_field={cnt}')
    outdir.mkdir(parents=True, exist_ok=True)
    manifest = []
    # 数据块边界 = 下一记录的起始（记录内 blob2 之后到下一记录间无空隙）
    all_starts = sorted({b for r in recs if r['kind'] == 'shader' for b in r['blobs']})
    for g in groups:
        gdir = outdir / g['name']
        gdir.mkdir(exist_ok=True)
        for tier, r in enumerate(g['shaders']):
            for bi, blob in enumerate(r['blobs']):
                nxt = [s for s in all_starts if s > blob]
                blob_end = nxt[0] if nxt else len(d)
                tag = f't{tier}_b{bi}'
                if platform == 'ps4':
                    extract_ps4_blob(d, blob, blob_end, gdir, tag, manifest)
                else:
                    extract_pc_blob(d, blob, blob_end, gdir, tag, manifest)
                for m in manifest:
                    if m.get('blob_off') == blob and 'material' not in m:
                        m['material'] = g['name']
                        m['tier'] = tier
                        m['ids'] = r['ids']
    (outdir / 'manifest.json').write_text(json.dumps(manifest, indent=1))
    nv = sum(1 for m in manifest if m['stage'] == 'vertex')
    nf = sum(1 for m in manifest if m['stage'] == 'fragment')
    print(f'extracted={len(manifest)} vertex={nv} fragment={nf} -> {outdir}')
    print('SBO_EXTRACT_OK')


if __name__ == '__main__':
    main()
