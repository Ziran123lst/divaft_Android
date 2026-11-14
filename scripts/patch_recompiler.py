#!/usr/bin/env python3
# divaft_Android — 诊断/修复补丁：作用于 fetch 后的 shadPS4 recompiler.cpp。
# 1) 修复假设：prologue 改在翻译首块之前发射（原为翻译后插块首，疑迭代器时序致 SSA 读不到 v0 定义）。
# 2) 在首次 SsaRewritePass 后加 IR dump，验证 v0 wiring。
import sys

p = 'third_party/shadps4/src/shader_recompiler/recompiler.cpp'
s = open(p).read()

old = (
    "        block.ir_block = ir_block;\n"
    "        translator.Translate(ir_block, block.begin, block.cond,\n"
    "                             std::span{program.ins_list}.subspan(start, size));\n"
    "        program.blocks.push_back(ir_block);\n"
    "    }\n"
    "    translator.EmitPrologue(program.blocks.front());"
)
new = (
    "        block.ir_block = ir_block;\n"
    "        if (program.blocks.empty()) { translator.EmitPrologue(ir_block); }\n"
    "        translator.Translate(ir_block, block.begin, block.cond,\n"
    "                             std::span{program.ins_list}.subspan(start, size));\n"
    "        program.blocks.push_back(ir_block);\n"
    "    }"
)
if old not in s:
    print("ERROR: prologue pattern not found", file=sys.stderr)
    sys.exit(1)
s = s.replace(old, new, 1)

needle = 'Shader::Optimization::SsaRewritePass(program);'
if needle in s:
    s = s.replace(needle, needle + '\n    Shader::IR::DumpProgram(program, info, "post-first-ssa.");', 1)

open(p, 'w').write(s)
print("patched: prologue before translate + post-first-ssa dump")