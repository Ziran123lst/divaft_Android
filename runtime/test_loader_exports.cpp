#include "divaft/diva_loader.h"
#include <cstdio>
#include <algorithm>
int main(int argc, char** argv){
  if(argc<2){std::printf("用法: %s <elf>\n",argv[0]);return 2;}
  divaft::DivaLoader L;
  auto r=L.Load(argv[1]);
  if(!r.ok){std::printf("LOAD_FAIL %s\n",r.error.c_str());return 1;}
  std::printf("image=%.1fMB imports=%zu exports=%zu rel_relocs=%zu entry=0x%llx\n",
    r.image_size/1048576.0, r.imports.size(), r.exports.size(), r.relative_relocs.size(),
    (unsigned long long)r.entry);
  // 找 _init_env (bzQExy189ZI)
  bool found=false;
  for(auto&e:r.exports) if(e.nid=="bzQExy189ZI"){std::printf("_init_env 导出 addr=0x%llx\n",(unsigned long long)e.addr);found=true;}
  std::printf("_init_env in exports: %s\n", found?"YES":"NO");
  // 打印前几个导出
  for(size_t i=0;i<5 && i<r.exports.size();++i)
    std::printf("  export %s @0x%llx\n", r.exports[i].nid.c_str(),(unsigned long long)r.exports[i].addr);
  return 0;
}