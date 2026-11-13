// divaft_Android — DIVA 运行时实现（原创）
// SPDX-License-Identifier: GPL-2.0-or-later
#include "divaft/diva_runtime.h"
#include "divaft/hle_kernel.h"
#include "divaft/hle_libc.h"

#include <cstdio>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>
#include <atomic>

namespace divaft {

namespace {
constexpr uint64_t kMB = 1024 * 1024;
constexpr uint64_t kDirectMem = 24 * kMB;
constexpr uint64_t kLibcHeap = 64 * kMB;
constexpr uint64_t kExtra = 96 * kMB;
constexpr uint64_t kLibcBase = 0x40000000;  // 自带 libc 模块基址（1GB，低4G，避开 eboot 区域）
std::atomic<int> g_trace_n{0};

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

// 解析 NID→HLE/stub，创建带追踪的 veneer，返回其 guest 地址。
uint64_t MakeVeneer(HleWire& hle, GuestBridge& bridge, const std::string& nid, bool* is_stub_out) {
  const HleBinding* b = hle.Resolve(nid);
  HostFn fn;
  std::string name;
  bool is_stub = false;
  if (b) { fn = b->fn; name = b->name; }
  else {
    name = hle.ContractName(nid);
    is_stub = true;
    fn = [](const uint64_t*, unsigned, const double*, HleReturn*) -> uint64_t { return 0; };
  }
  if (is_stub_out) *is_stub_out = is_stub;
  HostFn traced = [name, fn, is_stub](const uint64_t* a, unsigned c, const double* fp,
                                      HleReturn* ret) -> uint64_t {
    int n = g_trace_n.fetch_add(1);
    if (n < 600) std::fprintf(stderr, "[hle %d] %s%s\n", n, name.c_str(), is_stub ? " (stub)" : "");
    return fn(a, c, fp, ret);
  };
  return bridge.ExposeHost(name, std::move(traced));
}
}  // namespace

DivaRuntime::DivaRuntime() = default;
DivaRuntime::~DivaRuntime() {
  if (guest_base_ && region_size_) munmap(reinterpret_cast<void*>(guest_base_), region_size_);
  if (libc_loaded_ && libc_base_ && libc_map_size_)
    munmap(reinterpret_cast<void*>(libc_base_), libc_map_size_);
}

// 把模块映像映射到 base，应用 RELATIVE 重定位（base+addend 写入 base+target）。
bool DivaRuntime::MapModuleAt(const LoadResult& lr, uint64_t base) {
  const uint64_t page = static_cast<uint64_t>(sysconf(_SC_PAGESIZE));
  uint64_t sz = (lr.image_size + page - 1) & ~(page - 1);
  void* p = mmap(reinterpret_cast<void*>(base), sz, PROT_READ | PROT_WRITE | PROT_EXEC,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
  if (p == MAP_FAILED) { std::perror("mmap module"); return false; }
  std::memcpy(p, lr.image.data(), lr.image.size());
  for (const auto& r : lr.relative_relocs) {
    if (r.target + 8 > lr.image_size) continue;
    uint64_t val = base + r.addend;
    std::memcpy(reinterpret_cast<void*>(base + r.target), &val, 8);
  }
  return true;
}

bool DivaRuntime::MapImage(const LoadResult& lr) {
  const uint64_t page = static_cast<uint64_t>(sysconf(_SC_PAGESIZE));
  image_aligned_ = (lr.image_size + page - 1) & ~(page - 1);
  region_size_ = image_aligned_ + kExtra;
  // 优先低 4G 映射（规避 FEX-qemu 对 r8-r15 高地址指针截断）；避开静态二进制装载段。
  void* p = MAP_FAILED;
  const uint64_t low_base = 0x10000000;
  if (low_base + region_size_ < 0x100000000ULL) {
    p = mmap(reinterpret_cast<void*>(low_base), region_size_, PROT_READ | PROT_WRITE | PROT_EXEC,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
  }
  if (p == MAP_FAILED) {
    p = mmap(nullptr, region_size_, PROT_READ | PROT_WRITE | PROT_EXEC,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  }
  if (p == MAP_FAILED) { std::perror("mmap image"); return false; }
  guest_base_ = reinterpret_cast<uint64_t>(p);
  std::fprintf(stderr, "[diva] guest 区域基址=0x%llx（%s）\n",
               (unsigned long long)guest_base_,
               guest_base_ < 0x100000000ULL ? "低4G" : "高地址");
  std::memcpy(p, lr.image.data(), lr.image.size());
  // eboot RELATIVE 重定位
  for (const auto& r : lr.relative_relocs) {
    if (r.target + 8 > lr.image_size) continue;
    uint64_t val = guest_base_ + r.addend;
    std::memcpy(reinterpret_cast<void*>(guest_base_ + r.target), &val, 8);
  }
  return true;
}

// libc 自身导入全部接 HLE veneer/stub。
void DivaRuntime::WireModuleToHle(const LoadResult& lr, uint64_t base) {
  size_t real = 0;
  for (const auto& im : lr.imports) {
    bool is_stub = false;
    uint64_t veneer = MakeVeneer(hle_, bridge_, im.nid, &is_stub);
    if (!is_stub) ++real;
    uint64_t target = base + im.reloc_target;
    std::memcpy(reinterpret_cast<void*>(target), &veneer, 8);
  }
  std::fprintf(stderr, "[diva] libc 导入接 HLE %zu/%zu\n", real, lr.imports.size());
}

bool DivaRuntime::LoadLibc(const std::string& libc_path) {
  DivaLoader loader;
  LoadResult lr = loader.Load(libc_path);
  if (!lr.ok) {
    std::fprintf(stderr, "[diva] libc 加载失败: %s\n", lr.error.c_str());
    return false;
  }
  if (!MapModuleAt(lr, kLibcBase)) return false;
  libc_base_ = kLibcBase;
  libc_loaded_ = true;
  const uint64_t page = static_cast<uint64_t>(sysconf(_SC_PAGESIZE));
  libc_map_size_ = (lr.image_size + page - 1) & ~(page - 1);
  for (const auto& e : lr.exports) libc_export_[e.nid] = libc_base_ + e.addr;
  std::fprintf(stderr, "[diva] libc 模块基址=0x%llx 导出=%zu 映像=%.1fMB\n",
               (unsigned long long)libc_base_, lr.exports.size(), lr.image_size / 1048576.0);
  WireModuleToHle(lr, libc_base_);
  // 调用 libc 的 PREINIT_ARRAY / INIT_ARRAY 初始化函数（rtld 职责），建立全局状态。
  // 缺此步 libc 全局未初始化，snprintf 等会拿 null 崩溃。
  for (uint64_t fn : lr.preinit_funcs) {
    std::fprintf(stderr, "[diva] libc preinit @0x%llx\n", (unsigned long long)(libc_base_ + fn));
    bridge_.CallGuest(libc_base_ + fn, nullptr, 0);
  }
  for (uint64_t fn : lr.init_funcs) {
    std::fprintf(stderr, "[diva] libc init @0x%llx\n", (unsigned long long)(libc_base_ + fn));
    bridge_.CallGuest(libc_base_ + fn, nullptr, 0);
  }
  return true;
}

// eboot 导入接线：libc 导出命中→真实 libc guest 地址；否则 HLE veneer/stub。
void DivaRuntime::WireImports(const LoadResult& lr) {
  import_count_ = lr.imports.size();
  resolved_count_ = 0;
  size_t to_libc = 0;
  for (const auto& im : lr.imports) {
    uint64_t target = guest_base_ + im.reloc_target;
    auto it = libc_export_.find(im.nid);
    if (it != libc_export_.end()) {
      uint64_t addr = it->second;
      std::memcpy(reinterpret_cast<void*>(target), &addr, 8);
      ++to_libc; ++resolved_count_;
      continue;
    }
    bool is_stub = false;
    uint64_t veneer = MakeVeneer(hle_, bridge_, im.nid, &is_stub);
    if (!is_stub) ++resolved_count_;
    std::memcpy(reinterpret_cast<void*>(target), &veneer, 8);
  }
  std::fprintf(stderr, "[diva] eboot 接线：→libc %zu，→HLE %zu，共 %zu\n",
               to_libc, resolved_count_ - to_libc, import_count_);
}

bool DivaRuntime::Init(const std::string& eboot_path, const std::string& contract_path,
                       const std::string& libc_path) {
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
  mem_.Init(guest_base_ + image_aligned_, kDirectMem);
  heap_.Init(guest_base_ + image_aligned_ + kDirectMem, kLibcHeap);

  if (!contract_path.empty()) {
    if (!hle_.LoadContract(contract_path))
      std::fprintf(stderr, "[diva] 警告：契约加载失败 %s\n", contract_path.c_str());
  }
  size_t nk = RegisterKernelHle(hle_, &mem_, &bridge_,
                                lr.proc_param_vaddr ? guest_base_ + lr.proc_param_vaddr : 0);
  size_t nl = RegisterLibcHle(hle_, &heap_);
  std::fprintf(stderr, "[diva] HLE 注册：kernel=%zu libc=%zu（契约 %zu 条）\n",
               nk, nl, hle_.contract_size());

  // 可选加载自带 libc（多模块交叉接线）。
  if (!libc_path.empty()) {
    if (!LoadLibc(libc_path))
      std::fprintf(stderr, "[diva] 警告：libc 加载失败，退回 HLE stub\n");
  }

  entry_ = guest_base_ + lr.entry;
  WireImports(lr);
  initialized_ = true;
  return true;
}

bool DivaRuntime::Boot() {
  if (!initialized_) return false;
  // PS4 eboot 入口约定（参照 shadPS4 RunMainEntry / EntryParams）：RDI=EntryParams，RSI=exit 函数。
  uint64_t name = heap_.Malloc(32);
  if (name) std::memcpy(reinterpret_cast<void*>(name), "app0", 5);
  uint64_t params = heap_.Malloc(64);
  if (params) {
    std::memset(reinterpret_cast<void*>(params), 0, 64);
    *reinterpret_cast<uint32_t*>(params) = 1;
    *reinterpret_cast<uint64_t*>(params + 8) = name;
    *reinterpret_cast<uint64_t*>(params + 16) = 0;
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