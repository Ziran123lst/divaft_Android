// divaft_Android — guest 直接内存管理器（原创）
// 为 libkernel 内存 HLE 提供 guest 地址空间内的直接内存分配。
// 简化模型：physAddr 与 guest 虚拟地址恒等映射，bump 分配。
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <cstddef>

namespace divaft {

class GuestMemoryManager {
public:
  // heap_base/heap_size 为 guest 地址空间内划给直接内存的区域。
  void Init(uint64_t heap_base, uint64_t heap_size);

  // sceKernelAllocateDirectMemory：分配 len（对齐 alignment），输出 physAddr。
  // 返回 0 成功，负值失败。
  int AllocateDirect(uint64_t len, uint64_t alignment, uint64_t* phys_out);

  // sceKernelMapDirectMemory：把 phys_addr 映射到 guest 虚拟地址（恒等），输出 *addr。
  int MapDirect(uint64_t phys_addr, uint64_t len, uint64_t* addr_out);

  // sceKernelReleaseDirectMemory / munmap：bump 分配器暂不回收，返回 0。
  int Release(uint64_t start, uint64_t len);

  uint64_t total_size() const { return heap_size_; }
  uint64_t used() const { return cursor_ - heap_base_; }

private:
  uint64_t heap_base_ = 0;
  uint64_t heap_size_ = 0;
  uint64_t cursor_ = 0;
};

}  // namespace divaft
