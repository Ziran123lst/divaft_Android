// divaft_Android — guest 内存管理实现（原创）
// SPDX-License-Identifier: GPL-2.0-or-later
#include "divaft/guest_memory.h"

#include <cstring>

namespace divaft {

namespace {
uint64_t AlignUp(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }
}

// ---------------- GuestMemoryManager（直接内存 bump） ----------------

void GuestMemoryManager::Init(uint64_t heap_base, uint64_t heap_size) {
  heap_base_ = heap_base;
  heap_size_ = heap_size;
  cursor_ = heap_base;
}

int GuestMemoryManager::AllocateDirect(uint64_t len, uint64_t alignment, uint64_t* phys_out) {
  if (len == 0) return -1;
  if (alignment == 0) alignment = 16384;
  uint64_t start = AlignUp(cursor_, alignment);
  if (start + len > heap_base_ + heap_size_) return -1;  // 内存不足
  cursor_ = start + len;
  if (phys_out) *phys_out = start;
  return 0;
}

int GuestMemoryManager::MapDirect(uint64_t phys_addr, uint64_t len, uint64_t* addr_out) {
  // 恒等映射：guest 虚拟地址 = physAddr（guest 地址即真实宿主地址）。
  (void)len;
  if (addr_out) *addr_out = phys_addr;
  return 0;
}

int GuestMemoryManager::Release(uint64_t, uint64_t) { return 0; }

// ---------------- GuestHeap（边界标记首次适应） ----------------

namespace {
constexpr uint64_t kAlign = 16;
constexpr uint64_t kHeader = 16;   // size|alloc + 保留
constexpr uint64_t kFooter = 8;
constexpr uint64_t kOverhead = kHeader + kFooter;      // 24
constexpr uint64_t kMinBlock = 48;                     // 16+16+8 取整到 16
constexpr uint64_t kAllocBit = 1;

inline uint64_t Rd(uint64_t addr) { return *reinterpret_cast<const uint64_t*>(addr); }
inline void Wr(uint64_t addr, uint64_t v) { *reinterpret_cast<uint64_t*>(addr) = v; }
inline uint64_t BlkSize(uint64_t b) { return Rd(b) & ~kAllocBit; }
inline bool BlkAlloc(uint64_t b) { return (Rd(b) & kAllocBit) != 0; }
inline void SetHdr(uint64_t b, uint64_t sz, bool alloc) { Wr(b, sz | (alloc ? kAllocBit : 0)); }
inline void SetFtr(uint64_t b, uint64_t sz, bool alloc) { Wr(b + sz - kFooter, sz | (alloc ? kAllocBit : 0)); }
}  // namespace

void GuestHeap::Init(uint64_t base, uint64_t size) {
  base_ = base;
  size_ = size & ~(kAlign - 1);
  used_ = 0;
  align_map_.clear();
  if (size_ < kMinBlock) { base_ = 0; size_ = 0; return; }
  SetHdr(base_, size_, false);
  SetFtr(base_, size_, false);
}

uint64_t GuestHeap::FindFirstFit(uint64_t need) {
  uint64_t cur = base_;
  const uint64_t end = base_ + size_;
  while (cur < end) {
    uint64_t sz = BlkSize(cur);
    if (sz < kMinBlock) return 0;  // 损坏保护
    if (!BlkAlloc(cur) && sz >= need) return cur;
    cur += sz;
  }
  return 0;
}

void GuestHeap::Split(uint64_t block, uint64_t block_size, uint64_t need) {
  uint64_t rem = block_size - need;
  if (rem >= kMinBlock) {
    SetHdr(block, need, false);
    SetFtr(block, need, false);
    uint64_t r = block + need;
    SetHdr(r, rem, false);
    SetFtr(r, rem, false);
  }
}

void GuestHeap::Coalesce(uint64_t block) {
  uint64_t sz = BlkSize(block);
  const uint64_t end = base_ + size_;
  // 向后合并
  uint64_t next = block + sz;
  if (next < end && !BlkAlloc(next)) {
    sz += BlkSize(next);
    SetHdr(block, sz, false);
    SetFtr(block, sz, false);
  }
  // 向前合并（读前一块 footer）
  if (block > base_) {
    uint64_t pf = Rd(block - kFooter);
    if (!(pf & kAllocBit)) {
      uint64_t psz = pf & ~kAllocBit;
      uint64_t prev = block - psz;
      sz += psz;
      SetHdr(prev, sz, false);
      SetFtr(prev, sz, false);
    }
  }
}

uint64_t GuestHeap::Malloc(uint64_t size) {
  if (!base_) return 0;
  uint64_t payload = size < kAlign ? kAlign : size;
  uint64_t need = AlignUp(kOverhead + payload, kAlign);
  if (need < kMinBlock) need = kMinBlock;
  uint64_t block = FindFirstFit(need);
  if (!block) return 0;
  uint64_t bsize = BlkSize(block);
  Split(block, bsize, need);
  bsize = BlkSize(block);
  SetHdr(block, bsize, true);
  SetFtr(block, bsize, true);
  used_ += bsize;
  return block + kHeader;
}

void GuestHeap::Free(uint64_t ptr) {
  if (!ptr || !base_) return;
  uint64_t block;
  auto it = align_map_.find(ptr);
  if (it != align_map_.end()) {
    block = it->second.block;
    align_map_.erase(it);
  } else {
    block = ptr - kHeader;
  }
  if (block < base_ || block >= base_ + size_) return;
  uint64_t sz = BlkSize(block);
  if (sz < kMinBlock) return;
  SetHdr(block, sz, false);
  SetFtr(block, sz, false);
  used_ -= sz;
  Coalesce(block);
}

uint64_t GuestHeap::Calloc(uint64_t nmemb, uint64_t size) {
  if (nmemb && size > UINT64_MAX / nmemb) return 0;  // 溢出
  uint64_t total = nmemb * size;
  uint64_t p = Malloc(total);
  if (p) std::memset(reinterpret_cast<void*>(p), 0, total);
  return p;
}

uint64_t GuestHeap::UsableSize(uint64_t ptr) {
  if (!ptr) return 0;
  auto it = align_map_.find(ptr);
  if (it != align_map_.end()) return it->second.size;
  uint64_t block = ptr - kHeader;
  if (block < base_ || block >= base_ + size_) return 0;
  uint64_t sz = BlkSize(block);
  return sz > kOverhead ? sz - kOverhead : 0;
}

uint64_t GuestHeap::Realloc(uint64_t ptr, uint64_t size) {
  if (!ptr) return Malloc(size);
  if (size == 0) { Free(ptr); return 0; }
  uint64_t old = UsableSize(ptr);
  if (size <= old) return ptr;
  uint64_t np = Malloc(size);
  if (!np) return 0;
  std::memcpy(reinterpret_cast<void*>(np), reinterpret_cast<const void*>(ptr),
              old < size ? old : size);
  Free(ptr);
  return np;
}

uint64_t GuestHeap::Memalign(uint64_t alignment, uint64_t size) {
  if (alignment == 0 || (alignment & (alignment - 1))) return 0;
  if (alignment <= kAlign) return Malloc(size);
  // 过分配：保证能在内部找到对齐地址，且前面放得下回退信息。
  uint64_t total = size + alignment + kOverhead;
  uint64_t raw = Malloc(total);
  if (!raw) return 0;
  uint64_t aligned = AlignUp(raw + kHeader, alignment);
  // 若对齐后越出 raw 块 payload，退回重新分配（极少发生）。
  if (aligned + size > raw + UsableSize(raw)) {
    // 直接以对齐需求重分：加大过分配量
    Free(raw);
    raw = Malloc(size + alignment * 2 + kOverhead);
    if (!raw) return 0;
    aligned = AlignUp(raw + kHeader, alignment);
  }
  align_map_[aligned] = AlignInfo{raw - kHeader, size};
  return aligned;
}

}  // namespace divaft