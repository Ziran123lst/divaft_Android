// divaft_Android — guest↔host 桥接实现（原创）
// SPDX-License-Identifier: GPL-2.0-or-later
#include "divaft/guest_bridge.h"

#include <cstring>
#include <sys/mman.h>

namespace divaft {

// veneer 空间：guest 地址空间内的一段 RWX 区域，存放 x86 桩。
constexpr uint64_t kVeneerSpace = 16 * 1024 * 1024;  // 16MiB，足够数万桩
constexpr size_t kVeneerSize = 16;                   // 每桩 16 字节

// 单个 veneer 的 x86-64 字节码：
//   mov r10, rcx      ; 49 89 CA   （MS ABI 第1参数搬移，SysV 下无害）
//   mov eax, <id>     ; B8 <id32>
//   syscall           ; 0F 05      （FEX 陷入）
//   ret               ; C3
static void EmitVeneer(uint8_t* p, uint32_t id) {
  p[0] = 0x49; p[1] = 0x89; p[2] = 0xCA;
  p[3] = 0xB8;
  std::memcpy(p + 4, &id, 4);
  p[8] = 0x0F; p[9] = 0x05;
  p[10] = 0xC3;
}

struct GuestBridge::Impl {
  uint64_t guest_base = 0;
  uint8_t* veneer_space = nullptr;
  uint32_t next_id = 0;
  std::unordered_map<uint32_t, Veneer> by_id;
  bool initialized = false;
};

GuestBridge::GuestBridge() : impl_(new Impl) {}
GuestBridge::~GuestBridge() { Shutdown(); delete impl_; }

bool GuestBridge::Init(uint64_t guest_base, uint64_t guest_size) {
  impl_->guest_base = guest_base;
  // veneer 空间映射在 guest 地址空间高端（低 4G 内）
  uint64_t vbase = guest_base + guest_size - kVeneerSpace;
  void* p = mmap(reinterpret_cast<void*>(vbase), kVeneerSpace,
                 PROT_READ | PROT_WRITE | PROT_EXEC,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
  if (p == MAP_FAILED) return false;
  impl_->veneer_space = static_cast<uint8_t*>(p);
  impl_->initialized = true;
  // FEX 上下文创建在此接入（依赖 FEX::Context::CreateNewContext）。
  // 见 docs/architecture.md §2；FEX 作为 submodule 提供 JIT 与陷入。
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
  // SysV 整数参数序：rdi rsi rdx rcx r8 r9（FEX 陷入时已按此收集到 sysv_args）。
  // AAPCS64 用 x0-x7，此处直接转交（宿主函数按 AAPCS64 接收）。
  return it->second.fn(sysv_args, 6, fp_args);
}

uint64_t GuestBridge::CallGuest(uint64_t entry, const uint64_t* args, unsigned count) {
  // host→guest：经 FEX ExecuteThread/HandleCallback 执行 guest 代码。
  // 具体 FEX 调用在此接入（依赖 submodule）。
  (void)entry; (void)args; (void)count;
  return 0;  // 占位：FEX 接入后实现
}

void GuestBridge::Shutdown() {
  if (impl_->veneer_space) {
    munmap(impl_->veneer_space, kVeneerSpace);
    impl_->veneer_space = nullptr;
  }
  impl_->initialized = false;
}

}  // namespace divaft
