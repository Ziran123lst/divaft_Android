// divaft_Android — guest↔host 桥接实现（原创）
// 原理：guest(x86-64) 调系统函数跳入 veneer（x86 桩）触发 FEX 陷入，
// 桥接层把 SysV 参数转 AAPCS64 调原生 HLE，再把返回值搬回 guest。
// FEX 集成以 DIVAFT_WITH_FEX 守卫；未定义时退化为桩（供本机加载器构建）。
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

// guest 以 HLT 结束执行；故障信号里检测 HLT 并跳出 ExecuteThread。
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
    const uint64_t int_args[6] = {
        gregs[FEXCore::X86State::REG_RDI], gregs[FEXCore::X86State::REG_RSI],
        gregs[FEXCore::X86State::REG_RDX], gregs[FEXCore::X86State::REG_RCX],
        gregs[FEXCore::X86State::REG_R8],  gregs[FEXCore::X86State::REG_R9]};
    uint64_t ret = it->second.fn(int_args, 6, nullptr);
    gregs[FEXCore::X86State::REG_RAX] = ret;
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
  uint32_t next_id = 0;
  std::unordered_map<uint32_t, Veneer> by_id;
  bool initialized = false;
#ifdef DIVAFT_WITH_FEX
  fextl::unique_ptr<FEXCore::Context::Context> ctx;
  GuestSignalDelegator signals;
  fextl::unique_ptr<VeneerSyscallHandler> syscalls;
  FEXCore::Core::CPUState::gdt_segment gdt[32] {};
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
  return it->second.fn(sysv_args, 6, fp_args);
}

uint64_t GuestBridge::CallGuest(uint64_t entry, const uint64_t* args, unsigned count) {
#ifdef DIVAFT_WITH_FEX
  if (!impl_->initialized || !impl_->ctx) return 0;
  const uint64_t stack_top = impl_->guest_base + (1 << 20);
  auto* thread = impl_->ctx->CreateThread(entry, stack_top);
  if (!thread) return 0;
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
  state.gregs[FEXCore::X86State::REG_RSP] = stack_top;
  for (unsigned i = 0; i < count && i < 6; ++i) {
    state.gregs[FEXCore::X86State::REG_RDI + i] = args[i];
  }
  // callret 预测栈（带守护页）
  const size_t page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
  const size_t cr_size = FEXCore::Core::InternalThreadState::CALLRET_STACK_SIZE;
  auto* cr_alloc = static_cast<uint8_t*>(mmap(nullptr, cr_size + 2 * page, PROT_NONE,
                                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  if (cr_alloc != MAP_FAILED) {
    mprotect(cr_alloc + page, cr_size, PROT_READ | PROT_WRITE);
    thread->CallRetStackBase = cr_alloc + page;
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
