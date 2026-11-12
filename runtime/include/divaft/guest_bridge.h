// divaft_Android — guest↔host 桥接（原创设计）
// 原理：guest(x86-64) 调用系统函数时跳入一段 veneer（x86 桩），触发 FEX 陷入，
// 桥接层把 SysV 参数转 AAPCS64 调原生 HLE，再把返回值搬回 guest 寄存器。
// 本模块为原创实现，FEX 作为依赖提供 JIT/陷入机制。
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace divaft {

// 宿主函数签名：原生 ARM64，参数已由桥接转为 AAPCS64 整数槽。
using HostFn = std::function<uint64_t(const uint64_t* int_args, unsigned count,
                                     const double* fp_args)>;

struct Veneer {
  uint32_t id;
  uint64_t guest_addr;  // veneer 在 guest 地址空间的地址
  std::string name;
  HostFn fn;
};

class GuestBridge {
public:
  GuestBridge();
  ~GuestBridge();

  // 初始化 FEX 上下文与 veneer 空间。guest_base 为低 4G 映像基址。
  bool Init(uint64_t guest_base, uint64_t guest_size);

  // 注册宿主函数，返回其 veneer 的 guest 地址（供加载器写重定位）。
  uint64_t ExposeHost(const std::string& name, HostFn fn);

  // 从当前 guest 线程调用 guest 函数（host→guest），返回 RAX。
  uint64_t CallGuest(uint64_t entry, const uint64_t* args, unsigned count);

  // FEX 陷入分发：veneer syscall 到达时调用，按 id 找宿主函数并桥接参数。
  uint64_t DispatchSyscall(uint32_t veneer_id, const uint64_t* sysv_args,
                           const double* fp_args);

  void Shutdown();

private:
  struct Impl;
  Impl* impl_;
};

}  // namespace divaft
