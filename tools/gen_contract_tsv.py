#!/usr/bin/env python3
"""gen_contract_tsv.py — 从 eboot 导入分析生成 HLE 契约 TSV。
复现 runtime/data/contract.tsv。依赖 extract_imports.py 的产物 hle_contract.json。
用法: python3 gen_contract_tsv.py <hle_contract.json> <输出.tsv>
"""
import json
import sys
from pathlib import Path


def main():
    src, dst = Path(sys.argv[1]), Path(sys.argv[2])
    c = json.loads(src.read_text())
    lines = ['# DIVA HLE contract: nid\tlib\tname\tsource']
    n = 0
    for lib, fs in c['by_library'].items():
        for f in fs:
            lines.append(f"{f['nid']}\t{lib}\t{f['name']}\t{f['source']}")
            n += 1
    dst.write_text('\n'.join(lines) + '\n')
    print(f'contract: {n} entries -> {dst}')


if __name__ == '__main__':
    main()
