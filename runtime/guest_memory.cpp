// divaft_Android — guest 直接内存管理器实现（原创）
// SPDX-License-Identifier: GPL-2.0-or-later
#include "divaft/guest_memory.h"

namespace divaft {

namespace {
uint64_t AlignUp(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }
}

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
  if (addr_out) *addr_out = phys_addr;
  return 0;
}

int GuestMemoryManager::Release(uint64_t, uint64_t) { return 0; }

}  // namespace divaft
