// divaft_Android — DIVA 运行时（原创）
// 集成 DivaLoader + GuestBridge + HleWire：加载 eboot、把导入解析到 veneer、启动执行。
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "divaft/diva_loader.h"
#include "divaft/guest_bridge.h"
#include "divaft/hle_wire.h"
#include "divaft/guest_memory.h"

#include <cstdint>
#include <string>

namespace divaft {

class DivaRuntime {
public:
  DivaRuntime();
  ~DivaRuntime();

  // 加载 eboot 并接线导入。contract_path 为 HLE 契约（TSV）。
  // 未注册实现的 NID 用记录型 stub（打印调用、返回 0），便于观察启动序列。
  bool Init(const std::string& eboot_path, const std::string& contract_path);

  // 从 eboot 入口开始执行 guest 代码。
  bool Boot();

  HleWire& hle() { return hle_; }
  uint64_t entry() const { return entry_; }
  size_t import_count() const { return import_count_; }
  size_t resolved_count() const { return resolved_count_; }

private:
  bool MapImage(const LoadResult& lr);
  void WireImports(const LoadResult& lr);

  GuestBridge bridge_;
  HleWire hle_;
  GuestMemoryManager mem_;
  uint64_t guest_base_ = 0;
  uint64_t region_size_ = 0;
  uint64_t entry_ = 0;
  size_t import_count_ = 0;
  size_t resolved_count_ = 0;
  bool initialized_ = false;
};

}  // namespace divaft
