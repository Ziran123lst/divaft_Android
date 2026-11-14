// divaft_Android — ARM64 兼容的 FlattenExtendedUserdataPass。
// shadPS4 原版（flatten_extended_userdata_pass.cpp）用 xbyak 生成 x86 SRT walker 机器码，
// 整体包在 #ifdef ARCH_X86_64 内，ARM64 落到 UNREACHABLE stub。
//
// SRT（Shader Resource Table）walker 仅在 shader 含 ReadConst/ReadConstBuffer 资源表访问时需要；
// 不含资源表的 shader，原版 GenerateSrtProgram 在 srt_roots.empty() 时早退，根本不生成 walker 码。
// 据此对齐 ARM64 行为：
//   - 无 ReadConst/ReadConstBuffer：直接 RefreshFlatBuf（与 x86 无 SRT 路径等价，最简 shader 可完整翻译）。
//   - 含 ReadConst/ReadConstBuffer：SRT walker 尚未移植到 ARM64，标记 translation_failed 并告警。
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/ir/basic_block.h"
#include "shader_recompiler/ir/opcodes.h"
#include "shader_recompiler/info.h"
#include "common/logging/log.h"

namespace Shader::Optimization {

void FlattenExtendedUserdataPass(IR::Program& program) {
    bool has_readconst = false;
    for (IR::Block* block : program.post_order_blocks) {
        for (IR::Inst& inst : *block) {
            const auto op = inst.GetOpcode();
            if (op == IR::Opcode::ReadConst || op == IR::Opcode::ReadConstBuffer) {
                has_readconst = true;
                break;
            }
        }
        if (has_readconst) {
            break;
        }
    }

    if (has_readconst) {
        LOG_WARNING(Render_Recompiler,
                    "FlattenExtendedUserdataPass: SRT resource shader not yet supported on ARM64 "
                    "(x86 walker JIT unported); marking translation_failed");
        program.info.translation_failed = true;
        return;
    }

    program.info.RefreshFlatBuf();
}

}  // namespace Shader::Optimization