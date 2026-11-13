// divaft_Android — libkernel HLE（原创实现）
// 内存管理（直接内存分配/映射）+ 时间/CPU 基础函数。NID 来自 DIVA 导入契约。
// SPDX-License-Identifier: GPL-2.0-or-later
#include "divaft/hle_wire.h"
#include "divaft/guest_memory.h"

#include <ctime>
#include <unistd.h>

namespace divaft {

namespace {
uint64_t NowNsec() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL + ts.tv_nsec;
}
}  // namespace

void RegisterKernelHle(HleWire& wire, GuestMemoryManager* mem) {
  // ---- 内存管理 ----
  wire.Register("pO96TwzOm5E", "libkernel", "sceKernelGetDirectMemorySize",
      [mem](const uint64_t*, unsigned) -> uint64_t { return mem->total_size(); });

  wire.Register("rTXw65xmLIA", "libkernel", "sceKernelAllocateDirectMemory",
      [mem](const uint64_t* a, unsigned) -> uint64_t {
        // (searchStart, searchEnd, len, alignment, memoryType, physAddrOut)
        uint64_t phys = 0;
        int r = mem->AllocateDirect(a[2], a[3], &phys);
        if (r == 0 && a[5]) *reinterpret_cast<uint64_t*>(a[5]) = phys;
        return static_cast<uint64_t>(r);
      });

  wire.Register("L-Q3LEjIbgA", "libkernel", "sceKernelMapDirectMemory",
      [mem](const uint64_t* a, unsigned) -> uint64_t {
        // (addr**, len, prot, flags, phys_addr, alignment)
        uint64_t addr = 0;
        int r = mem->MapDirect(a[4], a[1], &addr);
        if (r == 0 && a[0]) *reinterpret_cast<uint64_t*>(a[0]) = addr;
        return static_cast<uint64_t>(r);
      });

  wire.Register("MBuItvba6z8", "libkernel", "sceKernelReleaseDirectMemory",
      [mem](const uint64_t* a, unsigned) -> uint64_t {
        return static_cast<uint64_t>(mem->Release(a[0], a[1]));
      });

  wire.Register("cQke9UuBQOk", "libkernel", "sceKernelMunmap",
      [](const uint64_t*, unsigned) -> uint64_t { return 0; });

  // ---- 时间 / CPU ----
  wire.Register("-2IRUCO--PM", "libkernel", "sceKernelReadTsc",
      [](const uint64_t*, unsigned) -> uint64_t { return NowNsec(); });
  wire.Register("1j3S3n-tTW4", "libkernel", "sceKernelGetTscFrequency",
      [](const uint64_t*, unsigned) -> uint64_t { return 1000000000ULL; });
  wire.Register("4J2sUJmuHZQ", "libkernel", "sceKernelGetProcessTime",
      [](const uint64_t*, unsigned) -> uint64_t { return NowNsec() / 1000; });
  wire.Register("1jfXLRVzisc", "libkernel", "sceKernelUsleep",
      [](const uint64_t* a, unsigned) -> uint64_t { usleep(static_cast<useconds_t>(a[0])); return 0; });
  wire.Register("QvsZxomvUHs", "libkernel", "sceKernelNanosleep",
      [](const uint64_t* a, unsigned) -> uint64_t {
        struct timespec ts { static_cast<time_t>(a[0] / 1000000000ULL),
                             static_cast<long>(a[0] % 1000000000ULL) };
        nanosleep(&ts, nullptr);
        return 0;
      });
  wire.Register("WslcK1FQcGI", "libkernel", "sceKernelIsNeoMode",
      [](const uint64_t*, unsigned) -> uint64_t { return 0; });
  wire.Register("g0VTBxfJyu0", "libkernel", "sceKernelGetCurrentCpu",
      [](const uint64_t*, unsigned) -> uint64_t { return 0; });
}

}  // namespace divaft
