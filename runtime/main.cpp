// divaft_Android — 加载器验证入口
// 用法: divaft_load <eboot.bin|eboot.elf> [contract.tsv]
// 加载 DIVA eboot，报告段/依赖/导入，验证原创加载器正确性。
// SPDX-License-Identifier: GPL-2.0-or-later
#include "divaft/diva_loader.h"
#include "divaft/hle_wire.h"

#include <cstdio>
#include <set>

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <eboot> [contract.tsv]\n", argv[0]);
    return 1;
  }
  divaft::DivaLoader loader;
  auto res = loader.Load(argv[1]);
  if (!res.ok) {
    std::fprintf(stderr, "LOAD FAIL: %s\n", res.error.c_str());
    return 2;
  }
  std::printf("LOAD OK\n");
  std::printf("  entry        = 0x%llx\n", (unsigned long long)res.entry);
  std::printf("  image_size   = %llu bytes (%.1f MB)\n",
              (unsigned long long)res.image_size, res.image_size / 1048576.0);
  std::printf("  segments     = %zu\n", res.segments.size());
  std::printf("  relocations  = %zu\n", res.reloc_count);
  std::printf("  needed_mods  = %zu\n", res.needed_modules.size());
  std::printf("  imports      = %zu\n", res.imports.size());

  // 按库统计导入
  std::set<std::string> libs;
  for (const auto& im : res.imports) libs.insert(im.lib_code);
  std::printf("  import_libs  = %zu\n", libs.size());

  if (argc >= 3) {
    divaft::HleWire wire;
    if (wire.LoadContract(argv[2])) {
      std::printf("  contract     = %zu entries\n", wire.contract_size());
      size_t named = 0;
      for (const auto& im : res.imports) {
        // 契约以 NID 为键
        if (wire.Resolve(im.nid)) ++named;
      }
      std::printf("  (contract loaded; register HLE impls to resolve)\n");
    }
  }
  return 0;
}
