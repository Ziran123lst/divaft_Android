// divaft_Android — DIVA 运行时实现（原创）
// SPDX-License-Identifier: GPL-2.0-or-later
#include "divaft/diva_runtime.h"
#include "divaft/hle_kernel.h"
#include "divaft/hle_libc.h"

#include <cstdio>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>

namespace divaft {

namespace {
constexpr uint64_t kMB = 1024 * 1024;
// 区域布局：映像 + 直接内存 24MB + libc 堆 64MB + 顶部 2MB（栈+veneer）+ 余量。
constexpr uint64_t kDirectMem = 24 * kMB;
constexpr uint64_t kLibcHeap = 64 * kMB;
constexpr uint64_t kExtra = 96 * kMB;
}  // namespace

DivaRuntime::DivaRuntime() = default;
DivaRuntime::~DivaRuntime() {
  if (guest_base_ && region_size_) munmap(reinterpret_cast<void*>(guest_base_), region_size_);
}

bool DivaRuntime::MapImage(const LoadResult& lr) {
  // 映像大小向上取整到页：veneer/栈用 MAP_FIXED，必须页对齐，否则 EINVAL。
  const uint64_t page = static_cast<uint64_t>(sysconf(_SC_PAGESIZE));
  image_aligned_ = (lr.image_size + page - 1) & ~(page - 1);
  region_size_ = image_aligned_ + kExtra;
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
    HostFn fn;
    std::string name;
    if (b) {
      fn = b->fn;
      name = b->name;
      ++resolved_count_;
    } else {
      // 记录型 stub：打印函数名、返回 0（ORBIS_OK）。便于观察启动序列。
      name = hle_.ContractName(im.nid);
      fn = [name](const uint64_t*, unsigned, const double*, HleReturn*) -> uint64_t {
        std::fprintf(stderr, "[hle-stub] %s\n", name.c_str());
        return 0;
      };
    }
    uint64_t veneer = bridge_.ExposeHost(name, std::move(fn));
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
  // 直接内存堆：映像(页对齐)之后 24MB。libc 堆：再之后 64MB（guest 地址）。
  mem_.Init(guest_base_ + image_aligned_, kDirectMem);
  heap_.Init(guest_base_ + image_aligned_ + kDirectMem, kLibcHeap);

  // 契约须先加载：kernel/libc HLE 均按名反查 NID 注册。
  if (!contract_path.empty()) {
    if (!hle_.LoadContract(contract_path))
      std::fprintf(stderr, "[diva] 警告：契约加载失败 %s（stub 将显示 NID）\n", contract_path.c_str());
  }
  size_t nk = RegisterKernelHle(hle_, &mem_, &bridge_);
  size_t nl = RegisterLibcHle(hle_, &heap_);
  std::fprintf(stderr, "[diva] HLE 注册：kernel=%zu libc=%zu（契约 %zu 条）\n",
               nk, nl, hle_.contract_size());

  entry_ = guest_base_ + lr.entry;
  WireImports(lr);
  std::fprintf(stderr, "[diva] wired imports %zu/%zu (real HLE / total)\n",
               resolved_count_, import_count_);
  initialized_ = true;
  return true;
}

bool DivaRuntime::Boot() {
  if (!initialized_) return false;
  // PS4 eboot 入口约定（参照 shadPS4 linker.cpp RunMainEntry / EntryParams）：
  //   RDI = EntryParams{ int argc; u32 pad; char* argv[]; }，RSI = exit 函数指针。
  //   入口汇编 mov r14d,[rdi]（argc）、lea r15,[rdi+8]（argv）。
  uint64_t name = heap_.Malloc(32);
  if (name) std::memcpy(reinterpret_cast<void*>(name), "app0", 5);
  uint64_t params = heap_.Malloc(64);
  if (params) {
    std::memset(reinterpret_cast<void*>(params), 0, 64);
    *reinterpret_cast<uint32_t*>(params) = 1;          // argc
    *reinterpret_cast<uint64_t*>(params + 8) = name;   // argv[0]
    *reinterpret_cast<uint64_t*>(params + 16) = 0;     // argv[1] = NULL
  }
  uint64_t exit_fn = bridge_.ExposeHost("ProgramExit",
      [](const uint64_t* a, unsigned, const double*, HleReturn*) -> uint64_t {
        std::fprintf(stderr, "[diva] ProgramExit(%lld)\n", (long long)a[0]);
        return 0;
      });
  std::fprintf(stderr, "[diva] booting entry=0x%llx params=0x%llx exit=0x%llx\n",
               (unsigned long long)entry_, (unsigned long long)params,
               (unsigned long long)exit_fn);
  uint64_t args[2] = {params, exit_fn};
  bridge_.CallGuest(entry_, args, 2);
  return true;
}

}  // namespace divaft