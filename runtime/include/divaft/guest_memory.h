// divaft_Android — guest 内存管理（原创）
// 1) GuestMemoryManager：libkernel 直接内存（sceKernelAllocateDirectMemory）bump 分配。
// 2) GuestHeap：libc malloc/free 的 guest 地址堆。
// 关键约束：guest 地址 == 真实宿主地址（恒等映射）。malloc 必须返回 guest 地址，
// 否则 FEX 内的游戏代码解引用宿主堆指针会 fault。故不能用宿主 malloc。
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <cstddef>
#include <unordered_map>

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

// GuestHeap：边界标记（boundary-tag）首次适应分配器，支持分裂与双向合并。
// 块布局（块大小恒为 16 的倍数，块首 16 对齐）：
//   [header 16B: size|alloc 位 + 保留][payload ...][footer 8B: size|alloc 位]
// payload 从 block+16 开始，16 对齐。块大小 = 16 + payload + 8，向上取整到 16。
// 线程安全：由调用方（HLE 层）持锁；本类自身不加锁，便于宿主单测。
class GuestHeap {
public:
  void Init(uint64_t base, uint64_t size);

  // 返回 guest 地址（16 对齐），失败返回 0。size=0 返回一个最小块（非 0）。
  uint64_t Malloc(uint64_t size);
  void Free(uint64_t ptr);
  uint64_t Calloc(uint64_t nmemb, uint64_t size);
  uint64_t Realloc(uint64_t ptr, uint64_t size);
  // alignment 须为 2 的幂。<=16 走普通分配；>16 过分配并记录对齐映射。
  uint64_t Memalign(uint64_t alignment, uint64_t size);
  uint64_t UsableSize(uint64_t ptr);

  uint64_t base() const { return base_; }
  uint64_t size() const { return size_; }
  uint64_t used() const { return used_; }
  bool initialized() const { return base_ != 0; }

private:
  struct AlignInfo { uint64_t block; uint64_t size; };

  uint64_t base_ = 0;
  uint64_t size_ = 0;
  uint64_t used_ = 0;
  // 过对齐分配：对齐后指针 -> {真实块首, 请求大小}。Free/UsableSize 先查这里。
  std::unordered_map<uint64_t, AlignInfo> align_map_;

  uint64_t FindFirstFit(uint64_t need);
  void Split(uint64_t block, uint64_t block_size, uint64_t need);
  void Coalesce(uint64_t block);
};

}  // namespace divaft