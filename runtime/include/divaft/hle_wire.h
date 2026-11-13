// divaft_Android — HLE 接线（原创实现）
// 把 DIVA 导入的 NID 解析到 HLE 函数。契约来自对 eboot 的导入分析
// （643 函数，见 docs/diva-analysis.md 与 tools/ 生成的 hle_contract.json）。
// 里程碑24：绑定统一存 HostFn（4 参，含 fp_args/HleReturn）；简单 HleFn（2 参）自动包装，
// FP/变参函数用 RegisterExt / RegisterByNameExt 直接注册 HostFn。
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "divaft/guest_bridge.h"

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace divaft {

// 简单 HLE：只用整数参、返回 RAX。guest 指针即真实宿主地址（恒等映射）可直接解引用。
using HleFn = std::function<uint64_t(const uint64_t* args, unsigned count)>;

struct HleBinding {
  std::string nid;
  std::string lib;
  std::string name;
  HostFn fn;  // 统一 4 参（简单 HleFn 已包装）
};

class HleWire {
public:
  // 简单 HLE 注册（自动包装为 HostFn，忽略 fp/ret）。
  void Register(const std::string& nid, const std::string& lib,
                const std::string& name, HleFn fn);
  // 扩展 HLE 注册（FP/变参，直接用 fp_args 与 HleReturn）。
  void RegisterExt(const std::string& nid, const std::string& lib,
                   const std::string& name, HostFn fn);

  // 按 lib+name 从契约反查 NID 并注册（契约须已 LoadContract）。命中返回 true。
  bool RegisterByName(const std::string& lib, const std::string& name, HleFn fn);
  bool RegisterByNameExt(const std::string& lib, const std::string& name, HostFn fn);

  // 把契约中某库的全部函数用同一处理器注册（整库 stub 骨架用）。返回注册条数。
  size_t RegisterLibrary(const std::string& lib, HleFn fn);

  // 按 NID 查找实现；未命中返回 nullptr（调用方走记录型 stub）。
  const HleBinding* Resolve(const std::string& nid) const;

  // 按 NID 查契约函数名（供 stub 日志）；未知返回 nid 本身。
  std::string ContractName(const std::string& nid) const;

  // 从契约文件（TSV: nid\tlib\tname\tsource）加载 NID→name/lib 元数据。
  bool LoadContract(const std::string& path);

  size_t registered() const { return bindings_.size(); }
  size_t contract_size() const { return contract_.size(); }

  // 兜底：未注册 NID 的默认返回（ORBIS_OK=0）。
  static uint64_t StubOk(const uint64_t* args, unsigned count);

private:
  std::unordered_map<std::string, HleBinding> bindings_;
  struct ContractEntry { std::string lib, name, source; };
  std::unordered_map<std::string, ContractEntry> contract_;
  // lib + "\t" + name -> nid（LoadContract 时建，供 RegisterByName）。
  std::unordered_map<std::string, std::string> name_to_nid_;
};

}  // namespace divaft