#!/usr/bin/env python3
# divaft_Android — 诊断补丁：
# 1) recompiler.cpp 首次 SsaRewritePass 后加 IR dump。
# 2) ssa_rewrite_pass.cpp 给 Set/GetVectorRegister 加日志（块指针+是否命中定义），钉死 v0 读序。
import sys

# --- recompiler.cpp: post-first-ssa dump ---
p = 'third_party/shadps4/src/shader_recompiler/recompiler.cpp'
s = open(p).read()
needle = 'Shader::Optimization::SsaRewritePass(program);'
if needle in s and 'post-first-ssa' not in s:
    s = s.replace(needle, needle + '\n    Shader::IR::DumpProgram(program, info, "post-first-ssa.");', 1)
    open(p, 'w').write(s)
    print("patched recompiler.cpp: post-first-ssa dump")

# --- recompiler.cpp: 解码日志（钉死每条 GCN 指令 opcode/src/dst）---
s = open(p).read()
if '[gcn] op=' not in s:
    if '#include <cstdio>' not in s:
        s = s.replace('#include <unordered_map>', '#include <unordered_map>\n#include <cstdio>', 1)
    dec_old = (
        "    while (!slice.atEnd()) {\n"
        "        program.ins_list.emplace_back(decoder.decodeInstruction(slice));\n"
        "    }"
    )
    dec_new = (
        "    while (!slice.atEnd()) {\n"
        "        auto dec_inst = decoder.decodeInstruction(slice);\n"
        "        std::fprintf(stderr, \"[gcn] op=%u src0field=%u src0=%u dst0=%u\\n\",\n"
        "                     (unsigned)dec_inst.opcode, (unsigned)dec_inst.src[0].field,\n"
        "                     dec_inst.src[0].code, dec_inst.dst[0].code);\n"
        "        program.ins_list.push_back(dec_inst);\n"
        "    }"
    )
    if dec_old in s:
        s = s.replace(dec_old, dec_new, 1)
        open(p, 'w').write(s)
        print("patched recompiler.cpp: GCN decode logging")
    else:
        print("WARN: decode loop pattern not found", file=sys.stderr)

# --- ssa_rewrite_pass.cpp: V0 read/write logging ---
q = 'third_party/shadps4/src/shader_recompiler/ir/passes/ssa_rewrite_pass.cpp'
t = open(q).read()
if '[ssa] SET' not in t:
    t = t.replace('#include <vector>', '#include <vector>\n#include <cstdio>', 1)
    set_old = (
        "    case IR::Opcode::SetVectorRegister:\n"
        "        pass.WriteVariable(IR::RegTag{inst.Arg(0).VectorReg()}, block, inst.Arg(1));\n"
        "        break;"
    )
    set_new = (
        "    case IR::Opcode::SetVectorRegister:\n"
        "        std::fprintf(stderr, \"[ssa] SET v%u blk=%p imm=%d\\n\",\n"
        "                     (unsigned)inst.Arg(0).VectorReg(), (void*)block, (int)inst.Arg(1).IsImmediate());\n"
        "        pass.WriteVariable(IR::RegTag{inst.Arg(0).VectorReg()}, block, inst.Arg(1));\n"
        "        break;"
    )
    get_old = (
        "    case IR::Opcode::GetVectorRegister:\n"
        "        inst.ReplaceUsesWithAndRemove(\n"
        "            pass.ReadVariable(IR::RegTag{inst.Arg(0).VectorReg()}, block));\n"
        "        break;"
    )
    get_new = (
        "    case IR::Opcode::GetVectorRegister: {\n"
        "        IR::Value got = pass.ReadVariable(IR::RegTag{inst.Arg(0).VectorReg()}, block);\n"
        "        std::fprintf(stderr, \"[ssa] GET v%u blk=%p -> empty=%d imm=%d\\n\",\n"
        "                     (unsigned)inst.Arg(0).VectorReg(), (void*)block, (int)got.IsEmpty(), (int)got.IsImmediate());\n"
        "        inst.ReplaceUsesWithAndRemove(got);\n"
        "        break;\n"
        "    }"
    )
    assert set_old in t, "SetVectorRegister pattern not found"
    assert get_old in t, "GetVectorRegister pattern not found"
    t = t.replace(set_old, set_new, 1).replace(get_old, get_new, 1)
    open(q, 'w').write(t)
    print("patched ssa_rewrite_pass.cpp: V0 read/write logging")
else:
    print("ssa logging already present")