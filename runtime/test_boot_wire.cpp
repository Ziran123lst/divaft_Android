// 宿主侧加载+接线验证：用真实 eboot 调 DivaRuntime.Init（不执行 guest，无需 FEX）。
// 验证加载器 + 643 导入接线 + 真 HLE 覆盖数。
#include "divaft/diva_runtime.h"
#include <cstdio>
int main(int argc, char** argv){
  if(argc<3){ std::printf("用法: %s <eboot.elf> <contract.tsv> [libc.elf]\n", argv[0]); return 2; }
  divaft::DivaRuntime rt;
  const char* libc = argc>=4 ? argv[3] : "";
  if(!rt.Init(argv[1], argv[2], libc)){ std::printf("BOOT_WIRE_FAIL (init)\n"); return 1; }
  std::printf("import_slots=%zu real_hle_slots=%zu stub_slots=%zu\n",
      rt.import_count(), rt.resolved_count(), rt.import_count()-rt.resolved_count());
  // 1472 个导入重定位槽（643 唯一函数）；真 HLE 覆盖的槽数应显著 >0。
  if(rt.import_count()>0 && rt.resolved_count()>=100){
    std::printf("BOOT_WIRE_PASS\n"); return 0;
  }
  std::printf("BOOT_WIRE_FAIL (count)\n"); return 1;
}