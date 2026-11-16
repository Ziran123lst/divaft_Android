// divaft_Android — libSceLibcInternal mspace 堆子系统（原创实现，算法参照 shadPS4 GPL-2.0）
// 真实 libc.prx 模块在 FEX 下跑，其 malloc 经此 mspace API。堆必须开在 guest 低 4G
// （guest 指针恒等映射可直接解引用）；HeapGetTraceInfo 的全局也开 guest 内存。
// SPDX-License-Identifier: GPL-2.0-or-later
#include "divaft/hle_wire.h"

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <bit>
#include <sys/mman.h>
#include <unistd.h>

namespace divaft {

namespace {
constexpr uint64_t AlignUp(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }
constexpr uint64_t AlignDown(uint64_t v, uint64_t a) { return v & ~(a - 1); }

constexpr uint64_t MspaceAlignment = 16;
constexpr uint64_t MspaceMinCapacity = 0x580;
constexpr uint64_t MspaceMapGranularity = 0x10000;
constexpr uint32_t MspaceMaxFlags = 0xf;

// guest 低 4G 区域分配器（mspace 堆 backing）。0x20000000 起，避开映像/堆/栈/callret/libc。
uint64_t GuestAlloc(uint64_t size) {
  static uint64_t cursor = 0x20000000;
  size = AlignUp(size, 0x1000);
  for (int tries = 0; tries < 4096; ++tries) {
    uint64_t addr = cursor; cursor += size;
    if (addr + size >= 0x100000000ULL) return 0;
    void* p = mmap(reinterpret_cast<void*>(addr), size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (p != MAP_FAILED) return addr;
  }
  return 0;
}

struct Mspace {
  uint8_t* base; uint64_t size; bool owns_memory;
  uint64_t current_inuse = 0, max_inuse = 0;
  std::map<uint64_t, uint64_t> free_blocks;
  std::unordered_map<uint64_t, uint64_t> used_blocks;
};
std::mutex g_mu;
std::map<uint64_t, std::unique_ptr<Mspace>> g_mspaces;

Mspace* FindMspace(uint64_t h) {
  auto it = g_mspaces.find(h);
  return it != g_mspaces.end() ? it->second.get() : nullptr;
}
Mspace* FindMspaceByPointer(uint64_t addr) {
  auto it = g_mspaces.upper_bound(addr);
  while (it != g_mspaces.begin()) {
    --it;
    if (addr < it->first + it->second->size) return it->second.get();
  }
  return nullptr;
}
void* AllocateBlock(Mspace& m, uint64_t size, uint64_t alignment) {
  if (size > m.size) return nullptr;
  alignment = std::max(alignment, MspaceAlignment);
  const uint64_t block_size = std::max(AlignUp(size, MspaceAlignment), MspaceAlignment);
  const uint64_t base_addr = reinterpret_cast<uintptr_t>(m.base);
  for (auto it = m.free_blocks.begin(); it != m.free_blocks.end(); ++it) {
    const uint64_t off = it->first, len = it->second;
    const uint64_t aligned_off = AlignUp(base_addr + off, alignment) - base_addr;
    const uint64_t padding = aligned_off - off;
    if (len < padding + block_size) continue;
    const uint64_t tail = len - padding - block_size;
    m.free_blocks.erase(it);
    if (padding) m.free_blocks.emplace(off, padding);
    if (tail) m.free_blocks.emplace(aligned_off + block_size, tail);
    m.used_blocks.emplace(aligned_off, block_size);
    m.current_inuse += block_size;
    m.max_inuse = std::max(m.max_inuse, m.current_inuse);
    return m.base + aligned_off;
  }
  return nullptr;
}
bool ReleaseBlock(Mspace& m, void* ptr) {
  const uint64_t offset = static_cast<uint8_t*>(ptr) - m.base;
  auto used = m.used_blocks.find(offset);
  if (used == m.used_blocks.end()) return false;
  uint64_t free_off = offset, free_len = used->second;
  m.current_inuse -= free_len;
  m.used_blocks.erase(used);
  auto next = m.free_blocks.lower_bound(free_off);
  if (next != m.free_blocks.end() && free_off + free_len == next->first) {
    free_len += next->second; next = m.free_blocks.erase(next);
  }
  if (next != m.free_blocks.begin()) {
    auto prev = std::prev(next);
    if (prev->first + prev->second == free_off) {
      free_off = prev->first; free_len += prev->second; m.free_blocks.erase(prev);
    }
  }
  m.free_blocks.emplace(free_off, free_len);
  return true;
}

struct MallocManagedSize {
  uint16_t size; uint16_t version; uint32_t reserved;
  uint64_t max_system_size, current_system_size, max_inuse_size, current_inuse_size;
};

// HeapGetTraceInfo 全局（开 guest 内存，返回 guest 地址）
uint64_t g_trace_mask_addr = 0;   // u64
uint64_t g_mstate_table_addr = 0; // u64[64]
void EnsureTraceGlobals() {
  if (!g_trace_mask_addr) {
    uint64_t a = GuestAlloc(0x1000);
    g_trace_mask_addr = a;
    g_mstate_table_addr = a + 0x100;
    std::memset(reinterpret_cast<void*>(a), 0, 0x1000);
  }
}
}  // namespace

size_t RegisterMspaceHle(HleWire& wire) {
  size_t n = 0;
  auto reg = [&](const char* name, HleFn fn) {
    if (wire.RegisterByName("libc", name, std::move(fn))) ++n;
  };
  reg("sceLibcMspaceCreate", [](const uint64_t* a, unsigned) -> uint64_t {
    const char* name = reinterpret_cast<const char*>(a[0]);
    uint64_t base = a[1], size = a[2]; uint32_t flags = (uint32_t)a[3];
    if (flags > MspaceMaxFlags) return 0;
    auto m = std::make_unique<Mspace>();
    if (base) {
      if ((base & 7) || (size & 7) || size <= MspaceMinCapacity) return 0;
      m->base = reinterpret_cast<uint8_t*>(base); m->size = size; m->owns_memory = false;
    } else {
      uint64_t map_size = AlignUp(size, MspaceMapGranularity);
      uint64_t addr = GuestAlloc(map_size ? map_size : MspaceMapGranularity);
      if (!addr) { std::fprintf(stderr, "[mspace] 分配 %llu 失败\n", (unsigned long long)size); return 0; }
      m->base = reinterpret_cast<uint8_t*>(addr); m->size = map_size; m->owns_memory = true;
    }
    const uint64_t base_addr = reinterpret_cast<uintptr_t>(m->base);
    const uint64_t first = AlignUp(base_addr + MspaceAlignment, MspaceAlignment) - base_addr;
    m->free_blocks.emplace(first, AlignDown(m->size - first, MspaceAlignment));
    uint64_t handle = base_addr;
    std::fprintf(stderr, "[mspace] create '%s' base=%llx size=%llx\n", name?name:"?", (unsigned long long)base_addr, (unsigned long long)m->size);
    std::lock_guard<std::mutex> lk(g_mu);
    g_mspaces.insert_or_assign(handle, std::move(m));
    return handle;
  });
  reg("sceLibcMspaceDestroy", [](const uint64_t* a, unsigned) -> uint64_t {
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_mspaces.find(a[0]);
    if (it == g_mspaces.end()) return 1;
    if (it->second->owns_memory) munmap(it->second->base, it->second->size);
    g_mspaces.erase(it); return 0;
  });
  reg("sceLibcMspaceMalloc", [](const uint64_t* a, unsigned) -> uint64_t {
    std::lock_guard<std::mutex> lk(g_mu);
    auto* m = FindMspace(a[0]);
    return m ? reinterpret_cast<uint64_t>(AllocateBlock(*m, a[1], MspaceAlignment)) : 0;
  });
  reg("sceLibcMspaceFree", [](const uint64_t* a, unsigned) -> uint64_t {
    if (!a[1]) return 0;
    std::lock_guard<std::mutex> lk(g_mu);
    auto* m = FindMspace(a[0]);
    if (m) ReleaseBlock(*m, reinterpret_cast<void*>(a[1]));
    return 0;
  });
  reg("sceLibcMspaceCalloc", [](const uint64_t* a, unsigned) -> uint64_t {
    uint64_t total = a[1] * a[2];
    std::lock_guard<std::mutex> lk(g_mu);
    auto* m = FindMspace(a[0]);
    void* p = m ? AllocateBlock(*m, total, MspaceAlignment) : nullptr;
    if (p) std::memset(p, 0, total);
    return reinterpret_cast<uint64_t>(p);
  });
  reg("sceLibcMspaceRealloc", [](const uint64_t* a, unsigned) -> uint64_t {
    std::lock_guard<std::mutex> lk(g_mu);
    auto* m = FindMspace(a[0]);
    if (!m) return 0;
    if (!a[1]) return reinterpret_cast<uint64_t>(AllocateBlock(*m, a[2], MspaceAlignment));
    const uint64_t off = a[1] - reinterpret_cast<uint64_t>(m->base);
    auto used = m->used_blocks.find(off);
    if (used == m->used_blocks.end()) return 0;
    const uint64_t old = used->second;
    if (a[2] <= old) return a[1];
    void* np = AllocateBlock(*m, a[2], MspaceAlignment);
    if (!np) return 0;
    std::memcpy(np, reinterpret_cast<void*>(a[1]), old);
    ReleaseBlock(*m, reinterpret_cast<void*>(a[1]));
    return reinterpret_cast<uint64_t>(np);
  });
  reg("sceLibcMspaceMemalign", [](const uint64_t* a, unsigned) -> uint64_t {
    uint64_t align = a[1] > MspaceAlignment ? std::bit_ceil(a[1]) : MspaceAlignment;
    std::lock_guard<std::mutex> lk(g_mu);
    auto* m = FindMspace(a[0]);
    return m ? reinterpret_cast<uint64_t>(AllocateBlock(*m, a[2], align)) : 0;
  });
  reg("sceLibcMspacePosixMemalign", [](const uint64_t* a, unsigned) -> uint64_t {
    if (a[2] < sizeof(void*) || !std::has_single_bit(a[2])) return 22;  // EINVAL
    std::lock_guard<std::mutex> lk(g_mu);
    auto* m = FindMspace(a[0]);
    void* p = m ? AllocateBlock(*m, a[3], a[2]) : nullptr;
    if (!p) return 12;  // ENOMEM
    *reinterpret_cast<uint64_t*>(a[1]) = reinterpret_cast<uint64_t>(p);
    return 0;
  });
  reg("sceLibcMspaceReallocalign", [](const uint64_t* a, unsigned) -> uint64_t {
    // (handle, ptr, alignment, size) — 简化按 realloc
    std::lock_guard<std::mutex> lk(g_mu);
    auto* m = FindMspace(a[0]);
    if (!m) return 0;
    if (!a[1]) return reinterpret_cast<uint64_t>(AllocateBlock(*m, a[3], a[2]));
    const uint64_t off = a[1] - reinterpret_cast<uint64_t>(m->base);
    auto used = m->used_blocks.find(off);
    if (used == m->used_blocks.end()) return 0;
    const uint64_t old = used->second;
    void* np = AllocateBlock(*m, a[3], a[2]);
    if (!np) return 0;
    std::memcpy(np, reinterpret_cast<void*>(a[1]), std::min(old, a[3]));
    ReleaseBlock(*m, reinterpret_cast<void*>(a[1]));
    return reinterpret_cast<uint64_t>(np);
  });
  reg("sceLibcMspaceMallocUsableSize", [](const uint64_t* a, unsigned) -> uint64_t {
    if (!a[0]) return 0;
    std::lock_guard<std::mutex> lk(g_mu);
    auto* m = FindMspaceByPointer(a[0]);
    if (!m) return 0;
    auto it = m->used_blocks.find(a[0] - reinterpret_cast<uint64_t>(m->base));
    return it != m->used_blocks.end() ? it->second : 0;
  });
  reg("sceLibcMspaceMallocStats", [](const uint64_t* a, unsigned) -> uint64_t {
    std::lock_guard<std::mutex> lk(g_mu);
    auto* m = FindMspace(a[0]);
    if (!m) return 1;
    if (a[1]) {
      auto* s = reinterpret_cast<MallocManagedSize*>(a[1]);
      if (s->size != sizeof(MallocManagedSize) || s->version != 1) return 1;
      s->max_system_size = m->size; s->current_system_size = m->size;
      s->max_inuse_size = m->max_inuse; s->current_inuse_size = m->current_inuse;
    }
    return 0;
  });
  reg("sceLibcMspaceIsHeapEmpty", [](const uint64_t* a, unsigned) -> uint64_t {
    std::lock_guard<std::mutex> lk(g_mu);
    auto* m = FindMspace(a[0]);
    return m && m->used_blocks.empty() ? 1 : 0;
  });
  wire.Register("NWtTN10cJzE", "libSceLibcInternalExt", "sceLibcHeapGetTraceInfo", [](const uint64_t* a, unsigned) -> uint64_t {
    EnsureTraceGlobals();
    if (a[0]) {
      // HeapInfoInfo: u64 size; u32 flag; u32 getSegmentInfo; u64* mask; u64* table
      uint8_t* info = reinterpret_cast<uint8_t*>(a[0]);
      std::memset(info, 0, 0x20);
      *reinterpret_cast<uint64_t*>(info + 0x00) = 0x20;
      *reinterpret_cast<uint64_t*>(info + 0x10) = g_trace_mask_addr;
      *reinterpret_cast<uint64_t*>(info + 0x18) = g_mstate_table_addr;
    }
    return 0;
  });
  std::fprintf(stderr, "[mspace] libSceLibcInternal 堆注册 %zu\n", n);
  return n;
}

}  // namespace divaft
