#!/usr/bin/env python3
# divaft_Android — eboot.bin 内嵌 Shdr 容器全量提取器
#
# 背景（2026-11-14 验证）：DIVA FTDX eboot 内嵌 186 个版本校验通过的 Shdr 容器
# （version=0x20007，与 shader_ps4.sbo 同格式），为工具/后处理类 shader。
# 容器布局与 SBO 相同：code_off = u32@+24 + 16，代码以 0xBEEB03FF+literal 开头，
# hdr[2]&0xFF: 1=vertex 2=fragment（其余按 compute 试探）。
#
# 代码终点算法（自洽性检查）：按序枚举 S_ENDPGM 候选终点 E，
# 只统计 [0,E) 内的分支（SOPP op∈{2,4..9}，目标=pc+1+simm16）；
# 取第一个满足「最大分支目标 < E」的 E。避免把数据区伪指令当分支。
#
# 用法: python3 eboot_shaders_extract.py <eboot.bin> <outdir>
# 输出: <outdir>/eboot_XXXX_<stage>.gcn.bin + manifest.json（游戏资产，禁入仓库）
import json
import struct
import sys
from pathlib import Path

PROLOGUE = 0xBEEB03FF
S_ENDPGM = 0xBF810000
MAX_DWORDS = 8192  # 单 shader 扫描上限（工具类 shader 都很小）


def u32(d, o):
    return struct.unpack_from('<I', d, o)[0]


def s16(v):
    return v - 0x10000 if v >= 0x8000 else v


def find_containers(d):
    pos, out = 0, []
    while True:
        i = d.find(b'Shdr', pos)
        if i < 0:
            break
        if i + 32 <= len(d) and u32(d, i + 4) == 0x20007:
            out.append(i)
        pos = i + 4
    return out


def code_end(d, code, limit):
    """自洽终点：第一个覆盖其内部全部分支目标的 S_ENDPGM 结尾。"""
    endpgms, branches = [], []
    for i in range(min(MAX_DWORDS, limit)):
        w = u32(d, code + 4 * i)
        if w == S_ENDPGM:
            endpgms.append(i + 1)
        elif (w >> 23) == 0x17F and ((w >> 16) & 0x7F) in (2, 4, 5, 6, 7, 8, 9):
            branches.append((i, i + 1 + s16(w & 0xFFFF)))
    for e in endpgms:
        max_t = max((t for p, t in branches if p < e), default=-1)
        if max_t < e:
            return e, max_t
    return None, None


def main():
    src, outdir = Path(sys.argv[1]), Path(sys.argv[2])
    d = src.read_bytes()
    outdir.mkdir(parents=True, exist_ok=True)
    containers = find_containers(d)
    manifest, seen_code = [], set()
    stats = {'vertex': 0, 'fragment': 0, 'compute': 0, 'no_end': 0, 'dup': 0, 'no_prologue': 0}
    for idx, s in enumerate(containers):
        hdr2 = u32(d, s + 8)
        stage = {1: 'vertex', 2: 'fragment'}.get(hdr2 & 0xFF, 'compute')
        code = s + u32(d, s + 24) + 16
        if code + 4 > len(d) or u32(d, code) != PROLOGUE:
            stats['no_prologue'] += 1
            continue
        if code in seen_code:
            stats['dup'] += 1
            continue
        seen_code.add(code)
        e, max_t = code_end(d, code, (len(d) - code) // 4)
        if e is None:
            stats['no_end'] += 1
            manifest.append({'container': s, 'code_off': code, 'stage': stage,
                             'status': 'no_self_consistent_end'})
            continue
        fn = f'eboot_{idx:04d}_{stage}.gcn.bin'
        (outdir / fn).write_bytes(d[code:code + 4 * e])
        manifest.append({'file': fn, 'container': s, 'code_off': code, 'stage': stage,
                         'code_dwords': e, 'max_branch_target': max_t, 'hdr2': hdr2,
                         'status': 'ok'})
        stats[stage] += 1
    (outdir / 'manifest.json').write_text(json.dumps(manifest, indent=1))
    print(f'containers={len(containers)} extracted={sum(stats[k] for k in ("vertex","fragment","compute"))} '
          f'vertex={stats["vertex"]} fragment={stats["fragment"]} compute={stats["compute"]} '
          f'dup={stats["dup"]} no_prologue={stats["no_prologue"]} no_end={stats["no_end"]}')
    print('EBOOT_EXTRACT_OK')


if __name__ == '__main__':
    main()
