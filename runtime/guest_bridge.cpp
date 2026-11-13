// divaft_Android — guest↔host 桥接实现（原创）
// 原理：guest(x86-64) 调系统函数跳入 veneer（x86 桩）触发 FEX 陷入，
// 桥接层把 SysV 参数转 AAPCS64 调原生 HLE，再把返回值搬回 guest。
// FEX 集成以 DIVAFT_WITH_FEX 守卫；未定义时退化为桩（供本机加载器构建）。
//
// 本轮（里程碑23）参照 uncharted 真机验证的 guest_cpu_fex.cpp：
//  - per-call 独立 guest 栈（支持并发 guest 线程，pthread_create 的前提）；
//  - 返回蹦床：把 exit_hlt 地址写进 [rsp] 作返回地址，guest 函数 ret 即落 HLT 干净退出，
//    使"会返回的 guest 函数"（线程例程）可被 CallGuest 调用并取回 RAX；
//  - per-call GDT，避免多线程共享段表竞态。
// SPDX-License-Identifier: GPL-2.0-or-later
#include "divaft/guest_bridge.h"

#include <cstring>
#include <atomic>
#include <sys/mman.h>

#ifdef DIVAFT_WITH_FEX
#include <FEXCore/Core/Context.h>
#include <FEXCore/Core/CoreState.h>
#include <FEXCore/Core/X86Enums.h>
#include <FEXCore/Core/HostFeatures.h>
#include <FEXCore/Core/SignalDelegator.h>
#include <FEXCore/HLE/SyscallHandler.h>
#include <FEXCore/Debug/InternalThreadState.h>
#include <FEXCore/Config/Config.h>
#include <Common/HostFeatures.h>
#include <csignal>
#include <csetjmp>
#include <unistd.h>
#endif

namespace divaft {

constexpr uint64_t kVeneerSpace = 1 * 1024 * 1024;
constexpr size_t kVeneerSize = 32;
constexpr size_t kGuestStack = 1 * 1024 * 1024;  // 每次 CallGuest 的私有 guest 栈

// veneer：guest 调系统函数跳入此桩。syscall 前把第4-6参（rcx→r10, r8, r9）压栈保存，
// 因 FEX-qemu syscall 陷入会清 r8/r9/r10（实测）。HandleSyscall 从栈上读回。
// 栈布局（syscall 时）：[rsp+0]=arg4 [rsp+8]=arg5 [rsp+16]=arg6 [rsp+24]=返回地址 [rsp+32..]=溢出参。
static void EmitVeneer(uint8_t* p, uint32_t id) {
  size_t i = 0;
  p[i++] = 0x49; p[i++] = 0x89; p[i++] = 0xCA;  // mov r10, rcx（arg4）
  p[i++] = 0x41; p[i++] = 0x51;                 // push r9（arg6）
  p[i++] = 0x41; p[i++] = 0x50;                 // push r8（arg5）
  p[i++] = 0x41; p[i++] = 0x52;                 // push r10（arg4）
  p[i++] = 0xB8;                                 // mov eax, id
  std::memcpy(p + i, &id, 4); i += 4;
  p[i++] = 0x0F; p[i++] = 0x05;                 // syscall
  p[i++] = 0x48; p[i++] = 0x83; p[i++] = 0xC4; p[i++] = 0x18;  // add rsp, 24
  p[i++] = 0xC3;                                 // ret
}

#ifdef DIVAFT_WITH_FEX

namespace {
// 崩溃诊断：打印 guest rip / fault / 全寄存器 / rip 处字节（EnableExitOnHLT 下由故障数据触发）。
void LogGuestCrash(FEXCore::Core::InternalThreadState* thread) {
  auto& state = thread->CurrentFrame->State;
  auto& fault = thread->CurrentFrame->SynchronousFaultData;
  const uint64_t rip = state.rip;
  std::fprintf(stderr, "[guest-crash] trap=%u err=%u rip=0x%llx bytes:",
               (unsigned)fault.TrapNo, (unsigned)fault.err_code, (unsigned long long)rip);
  if (rip) {
    for (int i = 0; i < 12; ++i)
      std::fprintf(stderr, " %02x", reinterpret_cast<const uint8_t*>(rip)[i]);
  }
  std::fprintf(stderr, "\n[guest-crash] gregs:");
  for (int i = 0; i < 16; ++i)
    std::fprintf(stderr, " r%d=%llx", i, (unsigned long long)state.gregs[i]);
  std::fprintf(stderr, "\n");
}
}  // namespace

// 参照 uncharted：不再自装 sigaction/siglongjmp，交由 FEX 内部信号处理 + EnableExitOnHLT 干净退出。
// 仅提供回调返回桩（HandleCallback 用）。
class GuestSignalDelegator final : public FEXCore::SignalDelegator {
public:
  uint64_t callback_return = 0;
  uintptr_t GetThunkCallbackRET() const override { return callback_return; }
};

class VeneerSyscallHandler final : public FEXCore::HLE::SyscallHandler {
public:
  VeneerSyscallHandler(std::unordered_map<uint32_t, Veneer>* by_id, uint64_t base, uint64_t size)
      : by_id_(by_id), base_(base), size_(size) {
    OSABI = FEXCore::HLE::SyscallOSABI::OS_GENERIC;
  }
  uint64_t HandleSyscall(FEXCore::Core::CpuStateFrame* Frame,
                         FEXCore::HLE::SyscallArguments* Args) override {
    auto& gregs = Frame->State.gregs;
    uint32_t id = static_cast<uint32_t>(gregs[FEXCore::X86State::REG_RAX]);
    std::fprintf(stderr, "[sys-entry] id=%u rsp=0x%llx rip=0x%llx\n", id,
                 (unsigned long long)gregs[FEXCore::X86State::REG_RSP],
                 (unsigned long long)Frame->State.rip);
    auto it = by_id_->find(id);
    if (it == by_id_->end()) return 0;
    // 诊断：读栈参前先打印函数名（定位读栈崩溃的函数）
    std::fprintf(stderr, "[sys] %s rsp=0x%llx\n", it->second.name.c_str(),
                 (unsigned long long)gregs[FEXCore::X86State::REG_RSP]);
    // SysV 前3参 RDI/RSI/RDX（FEX 保住）；第4-6参由 veneer 压栈保存（FEX-qemu syscall 清 r8/r9/r10）。
    uint64_t int_args[14] = {
        gregs[FEXCore::X86State::REG_RDI], gregs[FEXCore::X86State::REG_RSI],
        gregs[FEXCore::X86State::REG_RDX], 0, 0, 0};
    const uint64_t rsp = gregs[FEXCore::X86State::REG_RSP];
    if (rsp) {
      // veneer 栈布局：[rsp+0]=arg4 [rsp+8]=arg5 [rsp+16]=arg6 [rsp+24]=返回地址 [rsp+32..]=溢出参
      std::memcpy(&int_args[3], reinterpret_cast<const void*>(rsp), 3 * sizeof(uint64_t));
      std::memcpy(&int_args[6], reinterpret_cast<const void*>(rsp + 32), 8 * sizeof(uint64_t));
    }
    // 浮点参 XMM0..7 低 64 位（参照 uncharted：st.xmm.sse.data[i][0]）。
    double fp_args[8];
    for (int i = 0; i < 8; ++i) {
      uint64_t bits = Frame->State.xmm.sse.data[i][0];
      std::memcpy(&fp_args[i], &bits, 8);
    }
    HleReturn hret {};
    uint64_t ret = it->second.fn(int_args, 14, fp_args, &hret);
    std::fprintf(stderr, "[sys-ret] %s a0=0x%llx -> 0x%llx\n", it->second.name.c_str(),
                 (unsigned long long)int_args[0], (unsigned long long)ret);
    gregs[FEXCore::X86State::REG_RAX] = ret;
    if (hret.rdx_set) gregs[FEXCore::X86State::REG_RDX] = hret.rdx;
    if (hret.fp) {
      Frame->State.xmm.sse.data[0][0] = hret.xmm0;
      Frame->State.xmm.sse.data[1][0] = hret.xmm1;
    }
    return ret;
  }
  FEXCore::HLE::ExecutableRangeInfo QueryGuestExecutableRange(
      FEXCore::Core::InternalThreadState*, uint64_t) override {
    // 覆盖整个低 4G（eboot + 自带 libc 等多模块均在其中），参照 uncharted 全地址空间做法。
    return {0, 0x100000000ULL, true};
  }
  std::optional<FEXCore::ExecutableFileSectionInfo> LookupExecutableFileSection(
      FEXCore::Core::InternalThreadState*, uint64_t) override {
    return std::nullopt;
  }
private:
  std::unordered_map<uint32_t, Veneer>* by_id_;
  uint64_t base_, size_;
};

#endif  // DIVAFT_WITH_FEX

struct GuestBridge::Impl {
  uint64_t guest_base = 0;
  uint64_t guest_size = 0;
  uint8_t* veneer_space = nullptr;
  uint64_t exit_hlt = 0;      // 返回蹦床：HLT 桩的 guest 地址
  uint64_t tcb = 0;           // guest TCB（fs/gs 基址，libc TLS 访问需要）
  uint32_t next_id = 1;       // id 0 保留给 exit_hlt
  std::unordered_map<uint32_t, Veneer> by_id;
  bool initialized = false;
#ifdef DIVAFT_WITH_FEX
  fextl::unique_ptr<FEXCore::Context::Context> ctx;
  GuestSignalDelegator signals;
  fextl::unique_ptr<VeneerSyscallHandler> syscalls;
  FEXCore::Core::CPUState::gdt_segment gdt[32] {};  // 持久段表（FEX 执行期引用，须稳定地址）
#endif
};

GuestBridge::GuestBridge() : impl_(new Impl) {}
GuestBridge::~GuestBridge() { Shutdown(); delete impl_; }

bool GuestBridge::Init(uint64_t guest_base, uint64_t guest_size) {
  impl_->guest_base = guest_base;
  impl_->guest_size = guest_size;
  uint64_t vbase = guest_base + guest_size - kVeneerSpace;
  void* p = mmap(reinterpret_cast<void*>(vbase), kVeneerSpace,
                 PROT_READ | PROT_WRITE | PROT_EXEC,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
  if (p == MAP_FAILED) return false;
  impl_->veneer_space = static_cast<uint8_t*>(p);
  // 返回蹦床：veneer 空间首 16 字节放 HLT（id 0 保留）；+16 放 FEX CALLBACKRET(0f 3e)。
  impl_->exit_hlt = vbase;
  std::memset(impl_->veneer_space, 0, kVeneerSize);
  impl_->veneer_space[0] = 0xF4;                 // hlt（返回蹦床）
  impl_->veneer_space[16] = 0x0F; impl_->veneer_space[17] = 0x3E;  // CALLBACKRET

  // guest TCB：libc 经 fs/gs 段访问 TLS/线程指针，需建立 TCB 并设 fs/gs 基址（参照 uncharted/shadPS4）。
  // shadPS4 Tcb 布局：[0]=self [8]=dtv [16]=thread [24/32]=spare [40]=canary [48]=fiber。
  // 低 4G 页；首选地址被占（如自检 bridge 未释放）则顺延，避免静默失败致 fs/gs=0。
  {
    for (uint64_t tcb_addr = 0x48000000; tcb_addr < 0x49000000; tcb_addr += 4096) {
      void* tp = mmap(reinterpret_cast<void*>(tcb_addr), 4096, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
      if (tp != MAP_FAILED) {
        impl_->tcb = tcb_addr;
        std::memset(tp, 0, 4096);
        *reinterpret_cast<uint64_t*>(tcb_addr + 0) = tcb_addr;       // tcb_self
        *reinterpret_cast<uint64_t*>(tcb_addr + 40) = 0x9e3779b97f4a7c15ULL;  // tcb_canary
        break;
      }
    }
    if (!impl_->tcb) std::fprintf(stderr, "[bridge] 警告：TCB 分配失败，fs/gs 将为 0\n");
  }

#ifdef DIVAFT_WITH_FEX
  FEXCore::Config::Initialize();
  FEXCore::Config::Load();
  FEXCore::Config::Set(FEXCore::Config::CONFIG_IS64BIT_MODE, "1");
  FEXCore::Config::ReloadMetaLayer();
  auto features = FEX::FetchHostFeatures();
  // 目标真机（骁龙 7+Gen3）无 SVE；qemu 宿主误报 SVE 会生成非法指令。强制关闭。
  features.SupportsSVE128 = false;
  features.SupportsSVE256 = false;
  features.SupportsSVEBitPerm = false;
  impl_->ctx = FEXCore::Context::Context::CreateNewContext(features);
  if (!impl_->ctx) return false;
  impl_->syscalls = fextl::make_unique<VeneerSyscallHandler>(&impl_->by_id, guest_base, guest_size);
  impl_->signals.callback_return = impl_->exit_hlt + 16;
  impl_->ctx->SetSignalDelegator(&impl_->signals);
  impl_->ctx->SetSyscallHandler(impl_->syscalls.get());
  impl_->ctx->EnableExitOnHLT();  // HLT 干净退出 ExecuteThread（弃用 siglongjmp，参照 uncharted）
  if (!impl_->ctx->InitCore()) return false;
#endif
  impl_->initialized = true;
  return true;
}

uint64_t GuestBridge::ExposeHost(const std::string& name, HostFn fn) {
  if (!impl_->initialized) return 0;
  uint32_t id = impl_->next_id++;
  uint64_t addr = reinterpret_cast<uint64_t>(impl_->veneer_space) + id * kVeneerSize;
  EmitVeneer(impl_->veneer_space + id * kVeneerSize, id);
  impl_->by_id[id] = Veneer{id, addr, name, std::move(fn)};
  return addr;
}

uint64_t GuestBridge::DispatchSyscall(uint32_t veneer_id, const uint64_t* sysv_args,
                                      const double* fp_args) {
  auto it = impl_->by_id.find(veneer_id);
  if (it == impl_->by_id.end()) return 0;
  HleReturn hret {};
  return it->second.fn(sysv_args, 6, fp_args, &hret);
}

uint64_t GuestBridge::CallGuest(uint64_t entry, const uint64_t* args, unsigned count) {
#ifdef DIVAFT_WITH_FEX
  if (!impl_->initialized || !impl_->ctx) return 0;
  // per-call 私有 guest 栈：低 4G（规避 FEX-qemu 高地址指针截断），并发各用独立栈。
  static std::atomic<uint64_t> stack_cursor{0x60000000};
  uint64_t sbase = stack_cursor.fetch_add(kGuestStack);
  uint8_t* stack = static_cast<uint8_t*>(mmap(reinterpret_cast<void*>(sbase), kGuestStack,
                                              PROT_READ | PROT_WRITE,
                                              MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0));
  if (stack == MAP_FAILED) {
    stack = static_cast<uint8_t*>(mmap(nullptr, kGuestStack, PROT_READ | PROT_WRITE,
                                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  }
  if (stack == MAP_FAILED) return 0;
  const uint64_t stack_top = reinterpret_cast<uint64_t>(stack) + kGuestStack;
  // 顶部留 4KB 白：HandleSyscall 读 [rsp+8..rsp+72] 栈参不越界。
  const uint64_t rsp_top = stack_top - 4096;
  uint64_t rsp = (rsp_top & ~uint64_t(15)) - 8 * (count > 6 ? ((count - 6 + 1) & ~1u) : 0);
  for (unsigned i = 6; i < count; ++i)
    *reinterpret_cast<uint64_t*>(rsp + 8 * (i - 6)) = args[i];
  rsp -= 8;
  *reinterpret_cast<uint64_t*>(rsp) = impl_->exit_hlt;  // 返回蹦床：ret 落 HLT 退出

  auto* thread = impl_->ctx->CreateThread(entry, stack_top);
  if (!thread) { munmap(stack, kGuestStack); return 0; }
  auto& state = thread->CurrentFrame->State;
  state.segment_arrays[FEXCore::Core::CPUState::SEGMENT_ARRAY_INDEX_GDT] = impl_->gdt;
  state.segment_arrays[FEXCore::Core::CPUState::SEGMENT_ARRAY_INDEX_LDT] = impl_->gdt;
  state.cs_idx = FEXCore::Core::CPUState::DEFAULT_USER_CS << 3;
  auto* cs = FEXCore::Core::CPUState::GetSegmentFromIndex(state, state.cs_idx);
  FEXCore::Core::CPUState::SetGDTBase(cs, 0);
  FEXCore::Core::CPUState::SetGDTLimit(cs, 0xFFFFF);
  cs->L = 1; cs->D = 0;
  state.cs_cached = FEXCore::Core::CPUState::CalculateGDTBase(*cs);
  state.mxcsr = 0x9fc0;  // PS4 默认 FTZ+DAZ
  if (impl_->tcb) { state.fs_cached = impl_->tcb; state.gs_cached = impl_->tcb; }  // TLS/TCB
  state.gregs[FEXCore::X86State::REG_RSP] = rsp;
  // SysV host→guest 参数：RDI/RSI/RDX/RCX/R8/R9。
  const int regs[6] = {FEXCore::X86State::REG_RDI, FEXCore::X86State::REG_RSI,
                       FEXCore::X86State::REG_RDX, FEXCore::X86State::REG_RCX,
                       FEXCore::X86State::REG_R8,  FEXCore::X86State::REG_R9};
  for (unsigned i = 0; i < count && i < 6; ++i) state.gregs[regs[i]] = args[i];

  // callret 预测栈（带守护页，per-call）。
  const size_t page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
  const size_t cr_size = FEXCore::Core::InternalThreadState::CALLRET_STACK_SIZE;
  auto* cr_alloc = static_cast<uint8_t*>(mmap(nullptr, cr_size + 2 * page, PROT_NONE,
                                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  if (cr_alloc != MAP_FAILED) {
    mprotect(cr_alloc + page, cr_size, PROT_READ | PROT_WRITE);
    thread->CallRetStackBase = cr_alloc + page;
    state.callret_sp = reinterpret_cast<uint64_t>(cr_alloc + page) + cr_size / 4;
  }
  std::fprintf(stderr, "[callguest] entry=0x%llx rsp=0x%llx stackbase=0x%llx tcb=0x%llx\n",
               (unsigned long long)entry, (unsigned long long)rsp,
               (unsigned long long)reinterpret_cast<uint64_t>(stack), (unsigned long long)impl_->tcb);
  // EnableExitOnHLT：guest 落 HLT（含返回蹦床）时 ExecuteThread 干净返回；故障亦返回并置故障数据。
  impl_->ctx->ExecuteThread(thread);
  uint64_t ret = state.gregs[FEXCore::X86State::REG_RAX];
  auto& fault = thread->CurrentFrame->SynchronousFaultData;
  if (fault.FaultToTopAndGeneratedException && state.rip != impl_->exit_hlt) {
    LogGuestCrash(thread);
  }
  impl_->ctx->DestroyThread(thread);
  if (cr_alloc != MAP_FAILED) munmap(cr_alloc, cr_size + 2 * page);
  munmap(stack, kGuestStack);
  return ret;
#else
  (void)entry; (void)args; (void)count;
  return 0;
#endif
}

void GuestBridge::Shutdown() {
  if (impl_->veneer_space) {
    munmap(impl_->veneer_space, kVeneerSpace);
    impl_->veneer_space = nullptr;
  }
  if (impl_->tcb) {
    munmap(reinterpret_cast<void*>(impl_->tcb), 4096);
    impl_->tcb = 0;
  }
#ifdef DIVAFT_WITH_FEX
  impl_->ctx.reset();
#endif
  impl_->initialized = false;
}

}  // namespace divaft