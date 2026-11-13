// divaft_Android — HLE 接线实现（原创）
// SPDX-License-Identifier: GPL-2.0-or-later
#include "divaft/hle_wire.h"

#include <fstream>
#include <sstream>

namespace divaft {

void HleWire::Register(const std::string& nid, const std::string& lib,
                       const std::string& name, HleFn fn) {
  // 简单 HleFn 包装为 HostFn：忽略 fp_args/ret，仅返回 RAX。
  HostFn ext = [fn](const uint64_t* args, unsigned count, const double*, HleReturn*) -> uint64_t {
    return fn(args, count);
  };
  bindings_[nid] = HleBinding{nid, lib, name, std::move(ext)};
}

void HleWire::RegisterExt(const std::string& nid, const std::string& lib,
                          const std::string& name, HostFn fn) {
  bindings_[nid] = HleBinding{nid, lib, name, std::move(fn)};
}

bool HleWire::RegisterByName(const std::string& lib, const std::string& name, HleFn fn) {
  auto it = name_to_nid_.find(lib + "\t" + name);
  if (it == name_to_nid_.end()) return false;
  Register(it->second, lib, name, std::move(fn));
  return true;
}

bool HleWire::RegisterByNameExt(const std::string& lib, const std::string& name, HostFn fn) {
  auto it = name_to_nid_.find(lib + "\t" + name);
  if (it == name_to_nid_.end()) return false;
  RegisterExt(it->second, lib, name, std::move(fn));
  return true;
}

const HleBinding* HleWire::Resolve(const std::string& nid) const {
  auto it = bindings_.find(nid);
  return it == bindings_.end() ? nullptr : &it->second;
}

std::string HleWire::ContractName(const std::string& nid) const {
  auto it = contract_.find(nid);
  if (it != contract_.end() && !it->second.name.empty()) return it->second.name;
  return nid;
}

uint64_t HleWire::StubOk(const uint64_t*, unsigned) { return 0; }

// 契约文件格式（TSV，由 tools/gen_contract_tsv.py 从 hle_contract.json 生成）：
//   nid \t lib \t name \t source
bool HleWire::LoadContract(const std::string& path) {
  std::ifstream f(path);
  if (!f) return false;
  std::string line;
  while (std::getline(f, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::istringstream ss(line);
    std::string nid, lib, name, source;
    if (!std::getline(ss, nid, '\t')) continue;
    std::getline(ss, lib, '\t');
    std::getline(ss, name, '\t');
    std::getline(ss, source, '\t');
    contract_[nid] = ContractEntry{lib, name, source};
    // 反查索引：同 lib+name 若重复，保留首个（契约里偶有重复行）。
    std::string key = lib + "\t" + name;
    if (!name_to_nid_.count(key)) name_to_nid_[key] = nid;
  }
  return true;
}

}  // namespace divaft