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
constexpr size_t kVeneerSize = 16;
constexpr size_t kGuestStack = 1 * 1024 * 1024;  // 每次 CallGuest 的私有 guest 栈

static void EmitVeneer(uint8_t* p, uint32_t id) {
  p[0] = 0x49; p[1] = 0x89; p[2] = 0xCA;
  p[3] = 0xB8;
  std::memcpy(p + 4, &id, 4);
  p[8] = 0x0F; p[9] = 0x05;
  p[10] = 0xC3;
}

#ifdef DIVAFT_WITH_FEX

namespace {
thread_local FEXCore::Core::InternalThreadState* t_active_thread = nullptr;
thread_local sigjmp_buf t_exit_jmp;
constexpr uint8_t kHlt = 0xF4;

// guest 以 HLT 结束执行（直接 HLT 或经返回蹦床 ret 到 exit_hlt）；故障信号里检测 HLT 跳出。
void HostSignalHandler(int sig, siginfo_t* info, void* ucontext) {
  auto* thread = t_active_thread;
  if (thread) {
    const uint64_t rip = thread->CurrentFrame->State.rip;
    if (rip && reinterpret_cast<const uint8_t*>(rip)[0] == kHlt) {
      siglongjmp(t_exit_jmp, 1);
    }
    siglongjmp(t_exit_jmp, 2);
  }
  _exit(128 + sig);
}
}  // namespace

class GuestSignalDelegator final : public FEXCore::SignalDelegator {
public:
  bool Install() {
    struct sigaction act {};
    act.sa_sigaction = &HostSignalHandler;
    act.sa_flags = SA_SIGINFO;
    sigemptyset(&act.sa_mask);
    for (int sig : {SIGSEGV, SIGILL, SIGTRAP, SIGBUS}) {
      if (sigaction(sig, &act, nullptr) != 0) return false;
    }
    return true;
  }
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
    auto it = by_id_->find(id);
    if (it == by_id_->end()) return 0;
    // SysV 函数参数 RDI/RSI/RDX/RCX/R8/R9；veneer 在 syscall 前 mov r10,rcx，
    // 因 syscall 清 RCX（存返回 RIP），故第 4 参从 R10 读（syscall ABI）。
    uint64_t int_args[14] = {
        gregs[FEXCore::X86State::REG_RDI], gregs[FEXCore::X86State::REG_RSI],
        gregs[FEXCore::X86State::REG_RDX], gregs[FEXCore::X86State::REG_R10],
        gregs[FEXCore::X86State::REG_R8],  gregs[FEXCore::X86State::REG_R9]};
    // 第 7..14 整数参在调用者栈上：[rsp]=veneer 返回地址，[rsp+8] 起为栈参。
    const uint64_t rsp = gregs[FEXCore::X86State::REG_RSP];
    if (rsp) std::memcpy(&int_args[6], reinterpret_cast<const void*>(rsp + 8),
                         8 * sizeof(uint64_t));
    // 浮点参 XMM0..7 低 64 位（参照 uncharted：st.xmm.sse.data[i][0]）。
    double fp_args[8];
    for (int i = 0; i < 8; ++i) {
      uint64_t bits = Frame->State.xmm.sse.data[i][0];
      std::memcpy(&fp_args[i], &bits, 8);
    }
    HleReturn hret {};
    uint64_t ret = it->second.fn(int_args, 14, fp_args, &hret);
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
    return {base_, size_, false};
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
  uint32_t next_id = 1;       // id 0 保留给 exit_hlt
  std::unordered_map<uint32_t, Veneer> by_id;
  bool initialized = false;
#ifdef DIVAFT_WITH_FEX
  fextl::unique_ptr<FEXCore::Context::Context> ctx;
  GuestSignalDelegator signals;
  fextl::unique_ptr<VeneerSyscallHandler> syscalls;
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
  // 返回蹦床：veneer 空间首 16 字节放 HLT（id 0 保留）。guest 函数 ret 到此即退出。
  impl_->exit_hlt = vbase;
  std::memset(impl_->veneer_space, 0, kVeneerSize);
  impl_->veneer_space[0] = 0xF4;  // hlt

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
  impl_->ctx->SetSignalDelegator(&impl_->signals);
  impl_->ctx->SetSyscallHandler(impl_->syscalls.get());
  if (!impl_->signals.Install()) return false;
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
  // per-call 私有 guest 栈：并发 guest 线程互不干扰（pthread_create 前提）。
  uint8_t* stack = static_cast<uint8_t*>(mmap(nullptr, kGuestStack, PROT_READ | PROT_WRITE,
                                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  if (stack == MAP_FAILED) return 0;
  const uint64_t stack_top = reinterpret_cast<uint64_t>(stack) + kGuestStack;
  // 顶部留 4KB 白（参照 uncharted CallOutermost 用 stack_top-4096）：
  // HandleSyscall 会读 [rsp+8..rsp+72] 栈参，rsp 贴栈顶会越界并扰动 guest 寄存器。
  const uint64_t rsp_top = stack_top - 4096;
  // 返回蹦床 + 超过 6 个的栈参（参照 uncharted CallOutermost）。
  uint64_t rsp = (rsp_top & ~uint64_t(15)) - 8 * (count > 6 ? ((count - 6 + 1) & ~1u) : 0);
  for (unsigned i = 6; i < count; ++i)
    *reinterpret_cast<uint64_t*>(rsp + 8 * (i - 6)) = args[i];
  rsp -= 8;
  *reinterpret_cast<uint64_t*>(rsp) = impl_->exit_hlt;  // 返回地址 = HLT

  auto* thread = impl_->ctx->CreateThread(entry, stack_top);
  if (!thread) { munmap(stack, kGuestStack); return 0; }
  auto& state = thread->CurrentFrame->State;
  // per-call GDT（局部数组，执行期内有效），避免多线程共享段表竞态。
  FEXCore::Core::CPUState::gdt_segment gdt[32] {};
  state.segment_arrays[FEXCore::Core::CPUState::SEGMENT_ARRAY_INDEX_GDT] = gdt;
  state.segment_arrays[FEXCore::Core::CPUState::SEGMENT_ARRAY_INDEX_LDT] = gdt;
  state.cs_idx = FEXCore::Core::CPUState::DEFAULT_USER_CS << 3;
  auto* cs = FEXCore::Core::CPUState::GetSegmentFromIndex(state, state.cs_idx);
  FEXCore::Core::CPUState::SetGDTBase(cs, 0);
  FEXCore::Core::CPUState::SetGDTLimit(cs, 0xFFFFF);
  cs->L = 1; cs->D = 0;
  state.cs_cached = FEXCore::Core::CPUState::CalculateGDTBase(*cs);
  state.mxcsr = 0x9fc0;  // PS4 默认 FTZ+DAZ
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
  t_active_thread = thread;
  uint64_t ret = 0;
  if (sigsetjmp(t_exit_jmp, 1) == 0) {
    impl_->ctx->ExecuteThread(thread);
  } else {
    ret = state.gregs[FEXCore::X86State::REG_RAX];
  }
  t_active_thread = nullptr;
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
#ifdef DIVAFT_WITH_FEX
  impl_->ctx.reset();
#endif
  impl_->initialized = false;
}

}  // namespace divaft