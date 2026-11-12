// divaft_Android — HLE 接线（原创实现）
// 把 DIVA 导入的 NID 解析到 shadPS4 HLE 函数。契约来自对 eboot 的导入分析
// （643 函数，见 docs/diva-analysis.md 与 tools/ 生成的 hle_contract.json）。
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace divaft {

// 一个 HLE 函数的宿主实现（原生 ARM64）。参数经 guest_bridge 转为 AAPCS64。
using HleFn = std::function<uint64_t(const uint64_t* args, unsigned count)>;

struct HleBinding {
  std::string nid;
  std::string lib;
  std::string name;
  HleFn fn;
};

class HleWire {
public:
  // 注册一个 NID→实现 绑定（由各 shadPS4 HLE 库适配层调用）。
  void Register(const std::string& nid, const std::string& lib,
                const std::string& name, HleFn fn);

  // 按 NID 查找实现；未命中返回 nullptr（调用方走 aerolib 兜底）。
  const HleBinding* Resolve(const std::string& nid) const;

  // 从契约文件（hle_contract.json）加载 NID→name/lib 元数据（用于诊断与兜底命名）。
  bool LoadContract(const std::string& path);

  size_t registered() const { return bindings_.size(); }
  size_t contract_size() const { return contract_.size(); }

  // 兜底：未注册 NID 的默认返回（ORBIS_OK=0）。PC shadPS4 已证明 stub 可玩。
  static uint64_t StubOk(const uint64_t* args, unsigned count);

private:
  std::unordered_map<std::string, HleBinding> bindings_;
  struct ContractEntry { std::string lib, name, source; };
  std::unordered_map<std::string, ContractEntry> contract_;
};

}  // namespace divaft
