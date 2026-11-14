#!/usr/bin/env python3
# divaft_Android — 诊断补丁：在首次 SsaRewritePass 后加 IR dump，验证 v0 wiring。
# 注：prologue-before-translate 改动已回退（破坏 SRT readconst 断言）；保留原 EmitPrologue 位置。
import sys

p = 'third_party/shadps4/src/shader_recompiler/recompiler.cpp'
s = open(p).read()

needle = 'Shader::Optimization::SsaRewritePass(program);'
if needle in s:
    s = s.replace(needle, needle + '\n    Shader::IR::DumpProgram(program, info, "post-first-ssa.");', 1)
    open(p, 'w').write(s)
    print("patched: post-first-ssa dump only (prologue reorder reverted)")
else:
    print("WARN: SsaRewritePass needle not found", file=sys.stderr)