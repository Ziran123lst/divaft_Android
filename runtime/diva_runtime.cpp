// divaft_Android — DIVA 运行时实现（原创）
// SPDX-License-Identifier: GPL-2.0-or-later
#include "divaft/diva_runtime.h"
#include "divaft/hle_kernel.h"

#include <cstdio>
#include <cstring>
#include <sys/mman.h>

namespace divaft {

DivaRuntime::DivaRuntime() = default;
DivaRuntime::~DivaRuntime() {
  if (guest_base_ && region_size_) munmap(reinterpret_cast<void*>(guest_base_), region_size_);
}

bool DivaRuntime::MapImage(const LoadResult& lr) {
  // 区域 = 映像 + 32MB（直接内存堆 24MB + veneer 1MB + 栈 + 余量）。OS 选地址。
  region_size_ = lr.image_size + 32 * 1024 * 1024;
  void* p = mmap(nullptr, region_size_, PROT_READ | PROT_WRITE | PROT_EXEC,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) { std::perror("mmap image"); return false; }
  guest_base_ = reinterpret_cast<uint64_t>(p);
  std::memcpy(p, lr.image.data(), lr.image.size());
  return true;
}

void DivaRuntime::WireImports(const LoadResult& lr) {
  import_count_ = lr.imports.size();
  resolved_count_ = 0;
  for (const auto& im : lr.imports) {
    const HleBinding* b = hle_.Resolve(im.nid);
    HleFn fn;
    std::string name;
    if (b) {
      fn = b->fn;
      name = b->name;
      ++resolved_count_;
    } else {
      // 记录型 stub：打印调用、返回 0（ORBIS_OK）。便于观察启动序列。
      name = im.nid;
      fn = [name](const uint64_t*, unsigned) -> uint64_t {
        std::fprintf(stderr, "[hle-stub] %s\n", name.c_str());
        return 0;
      };
    }
    // HleFn(2参) 适配为 GuestBridge HostFn(3参)
    uint64_t veneer = bridge_.ExposeHost(name,
        [fn](const uint64_t* args, unsigned count, const double*) -> uint64_t {
          return fn(args, count);
        });
    // 把 veneer 地址写入导入的重定位目标（guest 地址 = base + reloc_target）
    uint64_t target = guest_base_ + im.reloc_target;
    std::memcpy(reinterpret_cast<void*>(target), &veneer, 8);
  }
}

bool DivaRuntime::Init(const std::string& eboot_path, const std::string& contract_path) {
  DivaLoader loader;
  LoadResult lr = loader.Load(eboot_path);
  if (!lr.ok) {
    std::fprintf(stderr, "[diva] load failed: %s\n", lr.error.c_str());
    return false;
  }
  std::fprintf(stderr, "[diva] loaded entry=0x%llx image=%.1fMB imports=%zu\n",
               (unsigned long long)lr.entry, lr.image_size / 1048576.0, lr.imports.size());
  if (!MapImage(lr)) return false;
  if (!bridge_.Init(guest_base_, region_size_)) {
    std::fprintf(stderr, "[diva] bridge init failed\n");
    return false;
  }
  // 直接内存堆：映像之后 24MB。注册 libkernel 内存/时间 HLE。
  mem_.Init(guest_base_ + lr.image_size, 24 * 1024 * 1024);
  RegisterKernelHle(hle_, &mem_);
  if (!contract_path.empty()) hle_.LoadContract(contract_path);
  entry_ = guest_base_ + lr.entry;
  WireImports(lr);
  std::fprintf(stderr, "[diva] wired imports %zu/%zu (real HLE / total)\n",
               resolved_count_, import_count_);
  initialized_ = true;
  return true;
}

bool DivaRuntime::Boot() {
  if (!initialized_) return false;
  std::fprintf(stderr, "[diva] booting entry=0x%llx\n", (unsigned long long)entry_);
  bridge_.CallGuest(entry_, nullptr, 0);
  return true;
}

}  // namespace divaft
