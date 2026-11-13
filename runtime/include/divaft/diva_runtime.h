// divaft_Android — DIVA 运行时（原创）
// 集成 DivaLoader + GuestBridge + HleWire + GuestHeap：
// 加载 eboot、可选加载自带 libc.prx（多模块交叉接线）、提供 kernel/libc HLE、启动执行。
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "divaft/diva_loader.h"
#include "divaft/guest_bridge.h"
#include "divaft/hle_wire.h"
#include "divaft/guest_memory.h"

#include <cstdint>
#include <string>
#include <unordered_map>

namespace divaft {

class DivaRuntime {
public:
  DivaRuntime();
  ~DivaRuntime();

  // 加载 eboot 并接线导入。contract_path 为 HLE 契约（TSV）。
  // libc_path 非空时加载自带 libc 模块：eboot 的 libc 导入指向真实 libc 代码（guest→guest），
  // libc 自身导入接 HLE。未注册实现的 NID 用记录型 stub。
  bool Init(const std::string& eboot_path, const std::string& contract_path,
            const std::string& libc_path = "");

  // 从 eboot 入口开始执行 guest 代码。
  bool Boot();

  HleWire& hle() { return hle_; }
  uint64_t entry() const { return entry_; }
  size_t import_count() const { return import_count_; }
  size_t resolved_count() const { return resolved_count_; }

private:
  bool MapImage(const LoadResult& lr);
  // 把模块映像映射到 base 并应用 RELATIVE 重定位。
  bool MapModuleAt(const LoadResult& lr, uint64_t base);
  // 把模块导入全部接 HLE veneer/stub（用于 libc 自身导入）。
  void WireModuleToHle(const LoadResult& lr, uint64_t base);
  // eboot 导入接线：libc 导出命中→guest 地址，否则 HLE。
  void WireImports(const LoadResult& lr);
  bool LoadLibc(const std::string& libc_path);

  GuestBridge bridge_;
  HleWire hle_;
  GuestMemoryManager mem_;   // libkernel 直接内存
  GuestHeap heap_;           // libc malloc/free（guest 地址）
  uint64_t guest_base_ = 0;
  uint64_t region_size_ = 0;
  uint64_t image_aligned_ = 0;
  uint64_t entry_ = 0;
  size_t import_count_ = 0;
  size_t resolved_count_ = 0;
  bool initialized_ = false;

  // 自带 libc 模块
  bool libc_loaded_ = false;
  uint64_t libc_base_ = 0;
  uint64_t libc_map_size_ = 0;
  std::unordered_map<std::string, uint64_t> libc_export_;  // NID -> guest 地址
};

}  // namespace divaft