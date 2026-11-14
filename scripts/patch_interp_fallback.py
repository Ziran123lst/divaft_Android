#!/usr/bin/env python3
# divaft_Android — 插值回退补丁（阶段3b 轻量方案）
#
# 背景：Adreno 732 实测（官方驱动 + Turnip gen8 v5）均无
# VK_KHR_fragment_shader_barycentric / VK_AMD_shader_explicit_vertex_parameter。
# 上游 shadPS4 在 V_INTERP_MOV_F32 用 ASSERT 挡住无扩展路径，但代码里已有
# Flat 回退分支（else: interp.primary=Flat）。Bachata S4（shadPS4 系安卓 PS4
# 模拟器）实证 DIVA FTDX 在该设备上可正常游玩——放宽 ASSERT 走现成回退即可，
# 无需重型 SSBO 拉取方案（后者降级为画质备选，见 docs/interp-fallback.md）。
#
# 用法: python3 scripts/patch_interp_fallback.py [shadps4_root]
# 幂等：已打过补丁则跳过。CI 在 fetch-shadps4 之后调用。
import re
import sys
from pathlib import Path

root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path('third_party/shadps4')
p = root / 'src/shader_recompiler/frontend/translate/vector_interpolation.cpp'
s = p.read_text()
if 'divaft: interp fallback' in s:
    print('patch_interp_fallback: already applied')
    sys.exit(0)
pat = re.compile(
    r'ASSERT\(attr\.is_flat \|\| src_select == 2 \|\|\s*'
    r'profile\.supports_amd_shader_explicit_vertex_parameter \|\|\s*'
    r'profile\.supports_fragment_shader_barycentric\);')
m = pat.search(s)
if not m:
    print('patch_interp_fallback: ASSERT pattern not found (upstream changed?)', file=sys.stderr)
    sys.exit(1)
s = pat.sub(
    '// divaft: interp fallback — 无 barycentric 扩展设备（Adreno 732 实测）\n'
    '    // 不再 ASSERT，走下方现成的 Flat 回退分支（Bachata S4 实证可玩）。\n'
    '    // 原 ASSERT(attr.is_flat || src_select == 2 || supports_amd || supports_bary)',
    s, count=1)
p.write_text(s)
print('patch_interp_fallback: applied to', p)
