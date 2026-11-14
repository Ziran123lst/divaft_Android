// divaft_Android — ARM64 移植版 FlattenExtendedUserdataPass（含解释执行 SRT walker）。
//
// shadPS4 原版（flatten_extended_userdata_pass.cpp）整体包在 #ifdef ARCH_X86_64：
// 用 xbyak 把 SRT（Shader Resource Table）walker JIT 成 x86 机器码。ARM64 无此实现。
//
// 本移植：
//   1) 可移植部分（readconst 收集/回溯、GVN 去重、flatbuf 偏移分配、SRT 根判定）照搬原版。
//   2) walker 不再 JIT，改为**解释执行**：编译期把偏移计算建成 OffExpr 表达式树、把 SRT 遍历建成
//      WalkerNode 树；运行期 RefreshFlatBuf 调 walker_func 时解释这棵树，从 guest 内存读资源 dword
//      拷入扁平缓冲。语义对齐原版 PushPtr/VisitPointer/ComputeOffset（含 48 位指针掩码）。
//   3) walker_func 是纯函数指针无上下文，用 trampoline 池 + 全局程序表关联每个 shader 的 walker 程序。
//
// 已知差异（TODO）：原版 SimplifyReadConstAddressAdd 优化未移植（不影响正确性，仅少一类地址合并）；
// 原版有 SRT walker 缺页信号处理，这里以 null 指针守护代替（真实 guest 地址有效时正常）。
#include <atomic>
#include <array>
#include <utility>
#include <cstring>
#include <memory>
#include <queue>
#include <unordered_map>
#include <vector>
#include <algorithm>
#include <boost/container/flat_map.hpp>
#include <boost/container/small_vector.hpp>
#include <magic_enum/magic_enum.hpp>
#include "common/types.h"
#include "common/assert.h"
#include "common/logging/log.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/resource.h"
#include "shader_recompiler/ir/basic_block.h"
#include "shader_recompiler/ir/opcodes.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/ir/reg.h"
#include "shader_recompiler/ir/value.h"
#include "shader_recompiler/ir/srt_gvn_table.h"
#include "shader_recompiler/ir/passes/srt.h"

namespace Shader::Optimization {
namespace {

// ---- 可移植辅助（照搬原版）----
static inline u16 GetFlatbufOffset(const IR::Inst* inst) {
    if (inst->GetOpcode() == IR::Opcode::ReadConstBuffer) {
        return inst->Flags<IR::BufferInstInfo>().flatbuf_off_dw;
    }
    if (inst->GetOpcode() == IR::Opcode::ReadConst) {
        return inst->Flags<u16>();
    }
    UNREACHABLE_MSG("Instruction not supported");
}

static inline void SetFlatbufOffset(IR::Inst* inst, u16 offset) {
    if (inst->GetOpcode() == IR::Opcode::ReadConstBuffer) {
        auto info = inst->Flags<IR::BufferInstInfo>();
        info.flatbuf_off_dw.Assign(offset);
        inst->SetFlags(info);
        return;
    }
    if (inst->GetOpcode() == IR::Opcode::ReadConst) {
        inst->SetFlags(offset);
        return;
    }
    UNREACHABLE_MSG("Instruction not supported");
}

static bool IsAllowedOffsetInstruction(const IR::Inst* inst) {
    switch (inst->GetOpcode()) {
    case IR::Opcode::GetUserData:
    case IR::Opcode::ReadConst:
    case IR::Opcode::ReadConstBuffer:
    case IR::Opcode::IAdd32:
    case IR::Opcode::ISub32:
    case IR::Opcode::IMul32:
    case IR::Opcode::ShiftLeftLogical32:
    case IR::Opcode::ShiftRightLogical32:
    case IR::Opcode::BitwiseAnd32:
    case IR::Opcode::BitwiseOr32:
    case IR::Opcode::BitwiseXor32:
    case IR::Opcode::BitwiseNot32:
    case IR::Opcode::UMin32:
    case IR::Opcode::UMax32:
    case IR::Opcode::BitFieldUExtract:
        return true;
    default:
        return false;
    }
}

static bool IsReadConstSource(const IR::Value base) {
    auto* inst = base.TryInst();
    if (!inst) {
        return false;
    }
    return inst->GetOpcode() == IR::Opcode::GetUserData ||
           inst->GetOpcode() == IR::Opcode::ReadConst ||
           inst->GetOpcode() == IR::Opcode::ReadConstBuffer;
}

struct PassInfo {
    struct PointerListCompare {
        bool operator()(const IR::Value& a, const IR::Value& b) const {
            if (a.IsImmediate() && b.IsImmediate()) {
                return a.U32() < b.U32();
            } else if (!a.IsImmediate() && !b.IsImmediate()) {
                auto a_inst = a.Inst();
                auto b_inst = b.Inst();
                auto a_block = a_inst->GetParent();
                auto b_block = b_inst->GetParent();
                if (a_block != b_block) {
                    return a_block < b_block;
                }
                auto it = a_block->Instructions().iterator_to(*a_inst);
                auto end = a_block->Instructions().end();
                for (; it != end; ++it) {
                    if (&*it == b_inst) {
                        return true;
                    }
                }
                return false;
            } else {
                return a.IsImmediate();
            }
        }
    };
    using PtrUserList =
        boost::container::flat_map<IR::Value, Shader::IR::Inst*, PointerListCompare>;

    SrtGvnTable gvn_table;
    std::unordered_map<IR::Inst*, PtrUserList> pointer_uses;
    boost::container::small_flat_map<IR::ScalarReg, IR::Inst*, 1> srt_roots;
    std::unordered_map<u32, IR::Inst*> vn_to_inst;
    u16 dst_off_dw{};

    PtrUserList* GetUsesAsPointer(IR::Inst* inst) {
        auto it = pointer_uses.find(inst);
        return it != pointer_uses.end() ? &it->second : nullptr;
    }
    IR::Inst* DeduplicateInstruction(IR::Inst* inst) {
        auto it = vn_to_inst.try_emplace(gvn_table.GetValueNumber(inst), inst);
        return it.first->second;
    }
};

// ---- 解释执行 walker：偏移表达式树 ----
struct OffExpr {
    enum class Kind : u8 {
        Imm, UserData, FlatBuf, Add, Sub, Mul, Shl, Shr, And, Or, Xor, Not, UMin, UMax, Bfe
    };
    Kind kind = Kind::Imm;
    u32 imm = 0;   // Imm 值 / UserData 寄存器号 / FlatBuf dword 偏移 / Bfe 宽度
    u32 imm2 = 0;  // Bfe 起始位
    std::unique_ptr<OffExpr> a, b;
};

// 编译期把 IR 偏移计算建成 OffExpr（ReadConst 叶子在此解析为已分配的 flatbuf 偏移）。
static std::unique_ptr<OffExpr> BuildOffExpr(const IR::Value& v, PassInfo& pi) {
    auto e = std::make_unique<OffExpr>();
    if (v.IsImmediate()) {
        e->kind = OffExpr::Kind::Imm;
        e->imm = v.U32();
        return e;
    }
    IR::Inst* inst = v.Inst();
    switch (inst->GetOpcode()) {
    case IR::Opcode::GetUserData:
        e->kind = OffExpr::Kind::UserData;
        e->imm = static_cast<u32>(inst->Arg(0).ScalarReg());
        return e;
    case IR::Opcode::ReadConst:
    case IR::Opcode::ReadConstBuffer: {
        u16 off = GetFlatbufOffset(pi.DeduplicateInstruction(inst));
        if (off == 0) {
            return nullptr;  // 对齐原版：偏移未分配（0）视为失败
        }
        e->kind = OffExpr::Kind::FlatBuf;
        e->imm = off;
        return e;
    }
    case IR::Opcode::IAdd32: e->kind = OffExpr::Kind::Add; break;
    case IR::Opcode::ISub32: e->kind = OffExpr::Kind::Sub; break;
    case IR::Opcode::IMul32: e->kind = OffExpr::Kind::Mul; break;
    case IR::Opcode::ShiftLeftLogical32: e->kind = OffExpr::Kind::Shl; break;
    case IR::Opcode::ShiftRightLogical32: e->kind = OffExpr::Kind::Shr; break;
    case IR::Opcode::BitwiseAnd32: e->kind = OffExpr::Kind::And; break;
    case IR::Opcode::BitwiseOr32: e->kind = OffExpr::Kind::Or; break;
    case IR::Opcode::BitwiseXor32: e->kind = OffExpr::Kind::Xor; break;
    case IR::Opcode::BitwiseNot32:
        e->kind = OffExpr::Kind::Not;
        e->a = BuildOffExpr(inst->Arg(0), pi);
        return e->a ? std::move(e) : nullptr;
    case IR::Opcode::UMin32: e->kind = OffExpr::Kind::UMin; break;
    case IR::Opcode::UMax32: e->kind = OffExpr::Kind::UMax; break;
    case IR::Opcode::BitFieldUExtract:
        e->kind = OffExpr::Kind::Bfe;
        e->a = BuildOffExpr(inst->Arg(0), pi);
        e->imm2 = inst->Arg(1).U32();
        e->imm = inst->Arg(2).U32();
        return e->a ? std::move(e) : nullptr;
    default:
        LOG_ERROR(Render_Recompiler, "ARM64 SRT: unexpected offset opcode {}",
                  magic_enum::enum_name(inst->GetOpcode()));
        return nullptr;
    }
    e->a = BuildOffExpr(inst->Arg(0), pi);
    e->b = BuildOffExpr(inst->Arg(1), pi);
    return (e->a && e->b) ? std::move(e) : nullptr;
}

static u32 EvalOff(const OffExpr* e, const u32* ud, u32* dst) {
    switch (e->kind) {
    case OffExpr::Kind::Imm: return e->imm;
    case OffExpr::Kind::UserData: return ud[e->imm];
    case OffExpr::Kind::FlatBuf: return dst[e->imm];
    case OffExpr::Kind::Add: return EvalOff(e->a.get(), ud, dst) + EvalOff(e->b.get(), ud, dst);
    case OffExpr::Kind::Sub: return EvalOff(e->a.get(), ud, dst) - EvalOff(e->b.get(), ud, dst);
    case OffExpr::Kind::Mul: return EvalOff(e->a.get(), ud, dst) * EvalOff(e->b.get(), ud, dst);
    case OffExpr::Kind::Shl: return EvalOff(e->a.get(), ud, dst) << (EvalOff(e->b.get(), ud, dst) & 31);
    case OffExpr::Kind::Shr: return EvalOff(e->a.get(), ud, dst) >> (EvalOff(e->b.get(), ud, dst) & 31);
    case OffExpr::Kind::And: return EvalOff(e->a.get(), ud, dst) & EvalOff(e->b.get(), ud, dst);
    case OffExpr::Kind::Or: return EvalOff(e->a.get(), ud, dst) | EvalOff(e->b.get(), ud, dst);
    case OffExpr::Kind::Xor: return EvalOff(e->a.get(), ud, dst) ^ EvalOff(e->b.get(), ud, dst);
    case OffExpr::Kind::Not: return ~EvalOff(e->a.get(), ud, dst);
    case OffExpr::Kind::UMin: return std::min(EvalOff(e->a.get(), ud, dst), EvalOff(e->b.get(), ud, dst));
    case OffExpr::Kind::UMax: return std::max(EvalOff(e->a.get(), ud, dst), EvalOff(e->b.get(), ud, dst));
    case OffExpr::Kind::Bfe:
        return (EvalOff(e->a.get(), ud, dst) >> e->imm2) & ((1u << e->imm) - 1u);
    }
    return 0;
}

// ---- SRT 遍历树 ----
struct WalkerCopy {
    std::unique_ptr<OffExpr> src_off;
    u16 dst_off = 0;
};
struct WalkerNode {
    std::unique_ptr<OffExpr> ptr_off;  // 解引用得到本节点 guest 指针的偏移
    std::vector<WalkerCopy> copies;
    std::vector<WalkerNode> children;
};
struct WalkerProgram {
    std::vector<WalkerNode> roots;
};

// 对齐原版 VisitPointer：建节点（拷贝 + 递归子指针），同时分配 flatbuf 偏移。
static bool BuildNode(const IR::Value& off_dw, IR::Inst* subtree, PassInfo& pi, WalkerNode& node,
                      u16& dst_off) {
    if ((subtree->GetOpcode() == IR::Opcode::ReadConst && subtree->Flags<u16>() == 0) ||
        (subtree->GetOpcode() == IR::Opcode::ReadConstBuffer &&
         subtree->Flags<IR::BufferInstInfo>().flatbuf_off_dw == 0)) {
        return false;
    }
    node.ptr_off = BuildOffExpr(off_dw, pi);
    if (!node.ptr_off) {
        LOG_ERROR(Render_Recompiler, "ARM64 SRT: failed to build pointer offset");
        return false;
    }
    PassInfo::PtrUserList* use_list = pi.GetUsesAsPointer(subtree);
    if (!use_list) {
        return false;
    }
    for (auto& [src_off_dw, use] : *use_list) {
        WalkerCopy c;
        c.src_off = BuildOffExpr(src_off_dw, pi);
        c.dst_off = dst_off;
        SetFlatbufOffset(use, dst_off);
        dst_off++;
        node.copies.push_back(std::move(c));
    }
    for (auto& [src_off_dw, use] : *use_list) {
        if (pi.GetUsesAsPointer(use)) {
            WalkerNode child;
            if (BuildNode(src_off_dw, use, pi, child, dst_off)) {
                node.children.push_back(std::move(child));
            }
        }
    }
    return true;
}

// 运行期解释：对齐 PushPtr（解引用 + 48 位掩码）+ 拷贝 + 递归。
static void WalkNode(const WalkerNode& node, const u32* base, const u32* ud, u32* dst) {
    u32 off = EvalOff(node.ptr_off.get(), ud, dst);
    u64 ptr = *reinterpret_cast<const u32*>(reinterpret_cast<const u8*>(base) + static_cast<size_t>(off) * 4);
    ptr &= 0xFFFFFFFFFFFFULL;
    if (ptr == 0) {
        return;  // null 守护（替代原版缺页信号处理）
    }
    const u32* node_mem = reinterpret_cast<const u32*>(static_cast<uintptr_t>(ptr));
    for (const auto& c : node.copies) {
        if (!c.src_off) {
            continue;
        }
        u32 src = EvalOff(c.src_off.get(), ud, dst);
        dst[c.dst_off] = node_mem[src];
    }
    for (const auto& child : node.children) {
        WalkNode(child, node_mem, ud, dst);
    }
}

static void WalkProgram(const WalkerProgram& p, const u32* ud, u32* dst) {
    for (const auto& root : p.roots) {
        WalkNode(root, ud, ud, dst);  // 根节点 base = user_data
    }
}

// ---- walker_func 关联：trampoline 池 + 全局程序表 ----
constexpr size_t kMaxWalkers = 512;
std::shared_ptr<WalkerProgram> g_programs[kMaxWalkers];
std::atomic<size_t> g_next_walker{0};

template <size_t N>
void PS4_SYSV_ABI SrtTrampoline(const u32* ud, u32* dst) {
    if (g_programs[N]) {
        WalkProgram(*g_programs[N], ud, dst);
    }
}

template <size_t... Is>
constexpr std::array<PFN_SrtWalker, sizeof...(Is)> MakePool(std::index_sequence<Is...>) {
    return {&SrtTrampoline<Is>...};
}
const auto g_pool = MakePool(std::make_index_sequence<kMaxWalkers>{});

static PFN_SrtWalker AllocWalker(std::shared_ptr<WalkerProgram> prog) {
    size_t idx = g_next_walker.fetch_add(1);
    if (idx >= kMaxWalkers) {
        return nullptr;
    }
    g_programs[idx] = std::move(prog);
    return g_pool[idx];
}

// 对齐原版 GenerateSrtProgram：srt_roots 空则早退；否则建 walker 程序并注册。
static void GenerateSrtProgram(Info& info, PassInfo& pi) {
    if (pi.srt_roots.empty()) {
        return;
    }
    LOG_WARNING(Render_Recompiler, "ARM64 SRT: building interpretive walker, {} root(s)",
                pi.srt_roots.size());
    auto prog = std::make_shared<WalkerProgram>();
    u16 dst_off = NUM_USER_DATA_REGS;
    for (const auto& [sgpr_base, root] : pi.srt_roots) {
        WalkerNode node;
        if (BuildNode(IR::Value(static_cast<u32>(sgpr_base)), root, pi, node, dst_off)) {
            prog->roots.push_back(std::move(node));
        }
    }
    info.srt_info.flattened_bufsize_dw = dst_off;
    PFN_SrtWalker w = AllocWalker(std::move(prog));
    if (!w) {
        LOG_WARNING(Render_Recompiler, "ARM64 SRT: walker pool exhausted; marking translation_failed");
        info.translation_failed = true;
        return;
    }
    info.srt_info.walker_func = w;
}

}  // namespace

void FlattenExtendedUserdataPass(IR::Program& program) {
    auto& post_order = program.post_order_blocks;
    PassInfo pass_info;
    boost::container::small_vector<IR::Inst*, 32> all_readconsts, visited;
    std::queue<IR::Inst*> queue;

    for (auto it = post_order.rbegin(); it != post_order.rend(); it++) {
        IR::Block* block{*it};
        for (IR::Inst& inst : *block) {
            if (inst.GetOpcode() != IR::Opcode::ReadConst &&
                inst.GetOpcode() != IR::Opcode::ReadConstBuffer) {
                continue;
            }
            if (inst.GetOpcode() == IR::Opcode::ReadConstBuffer) {
                auto inst_info = inst.Flags<IR::BufferInstInfo>();
                if (!inst_info.sharp_source) {
                    continue;
                }
            }
            // 注：原版 SimplifyReadConstAddressAdd 优化未移植（不影响正确性）。
            all_readconsts.push_back(&inst);
            auto offset = inst.Arg(1);
            if (offset.IsImmediate()) {
                continue;
            }
            queue.push(offset.Inst());
            while (!queue.empty()) {
                IR::Inst* cur{queue.front()};
                queue.pop();
                if (cur->GetOpcode() == IR::Opcode::ReadConstBuffer) {
                    auto buffer_inst_info = cur->Flags<IR::BufferInstInfo>();
                    if (!buffer_inst_info.sharp_source.Value()) {
                        all_readconsts.push_back(cur);
                        auto off = cur->Arg(1);
                        if (!off.IsImmediate()) {
                            auto arg_inst = off.Inst();
                            if (std::ranges::find(visited, arg_inst) == visited.end()) {
                                visited.push_back(arg_inst);
                                queue.push(arg_inst);
                            }
                        }
                    }
                    continue;
                }
                if (cur->GetOpcode() == IR::Opcode::GetUserData ||
                    cur->GetOpcode() == IR::Opcode::ReadConst ||
                    !IsAllowedOffsetInstruction(cur)) {
                    continue;
                }
                for (size_t arg = cur->NumArgs(); arg--;) {
                    const IR::Value arg_value = cur->Arg(arg);
                    if (arg_value.IsImmediate()) {
                        continue;
                    }
                    IR::Inst* arg_inst = arg_value.Inst();
                    if (std::ranges::find(visited, arg_inst) == visited.end()) {
                        visited.push_back(arg_inst);
                        queue.push(arg_inst);
                    }
                }
            }
        }
    }

    for (auto inst : all_readconsts) {
        if (pass_info.DeduplicateInstruction(inst) != inst) {
            continue;
        }
        IR::Inst* base = inst->Arg(0).Inst();
        if (auto* first = base->Arg(0).TryInst();
            first && first->GetOpcode() == IR::Opcode::ReadFirstLane) {
            continue;
        }
        ASSERT_MSG(IsReadConstSource(base->Arg(0)), "ReadConst base low not from constant memory");
        ASSERT_MSG(IsReadConstSource(base->Arg(1)), "ReadConst base high not from constant memory");

        IR::Inst* ptr_lo = base->Arg(0).Inst();
        ptr_lo = pass_info.DeduplicateInstruction(ptr_lo);
        auto ptr_uses_kv = pass_info.pointer_uses.try_emplace(ptr_lo, PassInfo::PtrUserList{});
        PassInfo::PtrUserList& user_list = ptr_uses_kv.first->second;
        user_list[inst->Arg(1)] = inst;
        if (ptr_lo->GetOpcode() == IR::Opcode::GetUserData) {
            IR::ScalarReg ud_reg = ptr_lo->Arg(0).ScalarReg();
            pass_info.srt_roots[ud_reg] = ptr_lo;
        }
    }

    GenerateSrtProgram(program.info, pass_info);

    for (IR::Inst* readconst : all_readconsts) {
        ASSERT(pass_info.vn_to_inst.contains(pass_info.gvn_table.GetValueNumber(readconst)));
        IR::Inst* original = pass_info.DeduplicateInstruction(readconst);
        SetFlatbufOffset(readconst, GetFlatbufOffset(original));
    }

    program.info.RefreshFlatBuf();
}

}  // namespace Shader::Optimization