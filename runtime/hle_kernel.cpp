// divaft_Android — libkernel HLE（原创实现）
// 内存管理 + 时间/CPU + 同步原语。NID 经契约按名反查（RegisterByName），不手敲。
// 同步对象（mutex/cond/equeue）用宿主 backing，按 guest 指针地址索引，
// 不假设 guest 侧 OrbisPthread* 结构体布局。
// SPDX-License-Identifier: GPL-2.0-or-later
#include "divaft/hle_kernel.h"

#include <ctime>
#include <unistd.h>
#include <pthread.h>
#include <mutex>
#include <unordered_map>
#include <thread>
#include <atomic>
#include <cstdio>

namespace divaft {

namespace {
uint64_t NowNsec() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL + ts.tv_nsec;
}

// ---- 同步对象宿主 backing（按 guest 指针地址索引） ----
std::mutex g_sync_mu;
std::unordered_map<uint64_t, pthread_mutex_t*> g_mutexes;
std::unordered_map<uint64_t, pthread_cond_t*> g_conds;
std::unordered_map<uint64_t, uint64_t> g_equeues;  // handle -> 占位（事件源后续接入）
uint64_t g_next_equeue = 1;

// ---- guest 线程宿主 backing ----
// 每个 guest 线程对应一个宿主线程，后者调 GuestBridge::CallGuest 执行 guest 例程。
struct GuestThread {
  std::thread th;
  uint64_t retval = 0;
  std::atomic<bool> done{false};
  std::atomic<bool> detached{false};
};
std::unordered_map<uint64_t, GuestThread*> g_threads;  // guest handle -> GuestThread

pthread_mutex_t* GetMutex(uint64_t addr) {
  auto it = g_mutexes.find(addr);
  return it == g_mutexes.end() ? nullptr : it->second;
}
pthread_cond_t* GetCond(uint64_t addr) {
  auto it = g_conds.find(addr);
  return it == g_conds.end() ? nullptr : it->second;
}
}  // namespace

size_t RegisterKernelHle(HleWire& wire, GuestMemoryManager* mem, GuestBridge* bridge) {
  size_t n = 0;
  auto reg = [&](const char* name, HleFn fn) {
    if (wire.RegisterByName("libkernel", name, std::move(fn))) ++n;
  };

  // ---- 内存管理 ----
  reg("sceKernelGetDirectMemorySize",
      [mem](const uint64_t*, unsigned) -> uint64_t { return mem->total_size(); });
  reg("sceKernelAllocateDirectMemory",
      [mem](const uint64_t* a, unsigned) -> uint64_t {
        // (searchStart, searchEnd, len, alignment, memoryType, physAddrOut)
        uint64_t phys = 0;
        int r = mem->AllocateDirect(a[2], a[3], &phys);
        if (r == 0 && a[5]) *reinterpret_cast<uint64_t*>(a[5]) = phys;
        return static_cast<uint64_t>(r);
      });
  reg("sceKernelMapDirectMemory",
      [mem](const uint64_t* a, unsigned) -> uint64_t {
        // (addr**, len, prot, flags, phys_addr, alignment)
        uint64_t addr = 0;
        int r = mem->MapDirect(a[4], a[1], &addr);
        if (r == 0 && a[0]) *reinterpret_cast<uint64_t*>(a[0]) = addr;
        return static_cast<uint64_t>(r);
      });
  reg("sceKernelReleaseDirectMemory",
      [mem](const uint64_t* a, unsigned) -> uint64_t {
        return static_cast<uint64_t>(mem->Release(a[0], a[1]));
      });
  reg("sceKernelMunmap", [](const uint64_t*, unsigned) -> uint64_t { return 0; });

  // ---- 时间 / CPU ----
  reg("sceKernelReadTsc", [](const uint64_t*, unsigned) -> uint64_t { return NowNsec(); });
  reg("sceKernelGetTscFrequency", [](const uint64_t*, unsigned) -> uint64_t { return 1000000000ULL; });
  reg("sceKernelGetProcessTime", [](const uint64_t*, unsigned) -> uint64_t { return NowNsec() / 1000; });
  reg("sceKernelUsleep", [](const uint64_t* a, unsigned) -> uint64_t {
    usleep(static_cast<useconds_t>(a[0])); return 0; });
  reg("sceKernelNanosleep", [](const uint64_t* a, unsigned) -> uint64_t {
    struct timespec ts { static_cast<time_t>(a[0] / 1000000000ULL),
                         static_cast<long>(a[0] % 1000000000ULL) };
    nanosleep(&ts, nullptr); return 0; });
  reg("sceKernelIsNeoMode", [](const uint64_t*, unsigned) -> uint64_t { return 0; });
  reg("sceKernelGetCurrentCpu", [](const uint64_t*, unsigned) -> uint64_t { return 0; });

  // ---- pthread 互斥锁（宿主 backing，递归型避免启动期重锁死锁） ----
  reg("scePthreadMutexInit", [](const uint64_t* a, unsigned) -> uint64_t {
    if (!a[0]) return 22;  // EINVAL
    std::lock_guard<std::mutex> lk(g_sync_mu);
    if (!g_mutexes.count(a[0])) {
      auto* m = new pthread_mutex_t;
      pthread_mutexattr_t attr;
      pthread_mutexattr_init(&attr);
      pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
      pthread_mutex_init(m, &attr);
      pthread_mutexattr_destroy(&attr);
      g_mutexes[a[0]] = m;
    }
    return 0; });
  reg("posix_pthread_mutex_lock", [](const uint64_t* a, unsigned) -> uint64_t {
    pthread_mutex_t* m;
    { std::lock_guard<std::mutex> lk(g_sync_mu); m = GetMutex(a[0]); }
    if (!m) return 22;
    return pthread_mutex_lock(m) == 0 ? 0 : 22; });
  reg("posix_pthread_mutex_unlock", [](const uint64_t* a, unsigned) -> uint64_t {
    pthread_mutex_t* m;
    { std::lock_guard<std::mutex> lk(g_sync_mu); m = GetMutex(a[0]); }
    if (!m) return 22;
    return pthread_mutex_unlock(m) == 0 ? 0 : 22; });
  reg("posix_pthread_mutex_destroy", [](const uint64_t* a, unsigned) -> uint64_t {
    std::lock_guard<std::mutex> lk(g_sync_mu);
    auto it = g_mutexes.find(a[0]);
    if (it == g_mutexes.end()) return 0;
    pthread_mutex_destroy(it->second); delete it->second; g_mutexes.erase(it);
    return 0; });

  // ---- pthread 条件变量 ----
  reg("scePthreadCondInit", [](const uint64_t* a, unsigned) -> uint64_t {
    if (!a[0]) return 22;
    std::lock_guard<std::mutex> lk(g_sync_mu);
    if (!g_conds.count(a[0])) {
      auto* c = new pthread_cond_t;
      pthread_cond_init(c, nullptr);
      g_conds[a[0]] = c;
    }
    return 0; });
  reg("posix_pthread_cond_wait", [](const uint64_t* a, unsigned) -> uint64_t {
    pthread_cond_t* c; pthread_mutex_t* m;
    { std::lock_guard<std::mutex> lk(g_sync_mu); c = GetCond(a[0]); m = GetMutex(a[1]); }
    if (!c || !m) return 22;
    return pthread_cond_wait(c, m) == 0 ? 0 : 22; });
  reg("posix_pthread_cond_signalto_np", [](const uint64_t* a, unsigned) -> uint64_t {
    pthread_cond_t* c;
    { std::lock_guard<std::mutex> lk(g_sync_mu); c = GetCond(a[0]); }
    if (!c) return 22;
    return pthread_cond_signal(c) == 0 ? 0 : 22; });

  // ---- pthread 属性 / 杂项（启动期多为形式参数，安全默认） ----
  reg("posix_pthread_attr_init", [](const uint64_t* a, unsigned) -> uint64_t {
    if (a[0]) *reinterpret_cast<uint64_t*>(a[0]) = 0;
    return 0; });
  reg("posix_pthread_attr_destroy", [](const uint64_t*, unsigned) -> uint64_t { return 0; });
  reg("posix_pthread_attr_setstacksize", [](const uint64_t*, unsigned) -> uint64_t { return 0; });
  reg("posix_pthread_attr_getstacksize", [](const uint64_t* a, unsigned) -> uint64_t {
    if (a[1]) *reinterpret_cast<uint64_t*>(a[1]) = 1024 * 1024;
    return 0; });
  reg("posix_pthread_attr_getschedparam", [](const uint64_t*, unsigned) -> uint64_t { return 0; });
  reg("scePthreadAttrSetaffinity", [](const uint64_t*, unsigned) -> uint64_t { return 0; });
  reg("scePthreadSetaffinity", [](const uint64_t*, unsigned) -> uint64_t { return 0; });
  reg("posix_pthread_self", [](const uint64_t*, unsigned) -> uint64_t {
    return static_cast<uint64_t>(pthread_self()); });
  reg("posix_pthread_yield", [](const uint64_t*, unsigned) -> uint64_t { sched_yield(); return 0; });
  reg("posix_pthread_setprio", [](const uint64_t*, unsigned) -> uint64_t { return 0; });
  reg("posix_pthread_rename_np", [](const uint64_t*, unsigned) -> uint64_t { return 0; });
  // ---- guest 线程创建 / 汇合 / 分离（宿主线程调 CallGuest 执行 guest 例程） ----
  // create_name_np(thread_out, attr, start_routine, arg, name)
  reg("posix_pthread_create_name_np", [bridge](const uint64_t* a, unsigned) -> uint64_t {
    if (!a[0] || !a[2]) return 22;  // EINVAL
    const uint64_t start = a[2], arg = a[3];
    auto* gt = new GuestThread;
    const uint64_t handle = reinterpret_cast<uint64_t>(gt);
    gt->th = std::thread([bridge, start, arg, gt]() {
      uint64_t guest_arg = arg;
      uint64_t rv = bridge->CallGuest(start, &guest_arg, 1);  // 例程 ret→返回蹦床退出
      std::lock_guard<std::mutex> lk(g_sync_mu);
      gt->retval = rv;
      gt->done = true;
      if (gt->detached) delete gt;  // 已 detach：自清理
    });
    { std::lock_guard<std::mutex> lk(g_sync_mu); g_threads[handle] = gt; }
    *reinterpret_cast<uint64_t*>(a[0]) = handle;
    return 0; });
  // 兼容裸 create（无 name 参数，前 4 参相同）。
  reg("posix_pthread_create", [bridge](const uint64_t* a, unsigned) -> uint64_t {
    if (!a[0] || !a[2]) return 22;
    const uint64_t start = a[2], arg = a[3];
    auto* gt = new GuestThread;
    const uint64_t handle = reinterpret_cast<uint64_t>(gt);
    gt->th = std::thread([bridge, start, arg, gt]() {
      uint64_t guest_arg = arg;
      uint64_t rv = bridge->CallGuest(start, &guest_arg, 1);
      std::lock_guard<std::mutex> lk(g_sync_mu);
      gt->retval = rv;
      gt->done = true;
      if (gt->detached) delete gt;
    });
    { std::lock_guard<std::mutex> lk(g_sync_mu); g_threads[handle] = gt; }
    *reinterpret_cast<uint64_t*>(a[0]) = handle;
    return 0; });
  reg("posix_pthread_join", [](const uint64_t* a, unsigned) -> uint64_t {
    GuestThread* gt = nullptr;
    { std::lock_guard<std::mutex> lk(g_sync_mu);
      auto it = g_threads.find(a[0]);
      if (it == g_threads.end()) return 0;  // 无效或已 detach
      gt = it->second; g_threads.erase(it); }
    gt->th.join();
    if (a[1]) *reinterpret_cast<uint64_t*>(a[1]) = gt->retval;
    delete gt;
    return 0; });
  reg("posix_pthread_detach", [](const uint64_t* a, unsigned) -> uint64_t {
    std::lock_guard<std::mutex> lk(g_sync_mu);
    auto it = g_threads.find(a[0]);
    if (it == g_threads.end()) return 0;
    GuestThread* gt = it->second;
    g_threads.erase(it);
    if (gt->done) { delete gt; }
    else { gt->detached = true; gt->th.detach(); }  // 线程结束时自清理
    return 0; });
  reg("scePthreadCancel", [](const uint64_t*, unsigned) -> uint64_t { return 0; });

  // ---- equeue（创建/等待/删除；事件源后续接入，Wait 暂返回 0 事件） ----
  reg("sceKernelCreateEqueue", [](const uint64_t* a, unsigned) -> uint64_t {
    std::lock_guard<std::mutex> lk(g_sync_mu);
    uint64_t h = g_next_equeue++;
    g_equeues[h] = 0;
    if (a[0]) *reinterpret_cast<uint64_t*>(a[0]) = h;
    return 0; });
  reg("sceKernelWaitEqueue", [](const uint64_t* a, unsigned) -> uint64_t {
    // (eq, events_out, num, timeout_us?) 暂无事件源：返回 0 事件。
    // 若给了超时指针，按其微秒值小睡，避免忙等。
    if (a[3]) {
      uint64_t us = *reinterpret_cast<const uint64_t*>(a[3]);
      if (us && us < 100000) usleep(static_cast<useconds_t>(us));
    }
    return 0; });
  reg("sceKernelDeleteEqueue", [](const uint64_t* a, unsigned) -> uint64_t {
    std::lock_guard<std::mutex> lk(g_sync_mu);
    g_equeues.erase(a[0]); return 0; });

  // ---- 栈保护 / 错误（__stack_chk_guard 为数据符号，需加载器特殊处理，见 README） ----
  reg("__Error", [](const uint64_t*, unsigned) -> uint64_t { return 0; });
  reg("stack_chk_fail", [](const uint64_t*, unsigned) -> uint64_t {
    std::fprintf(stderr, "[kernel] stack_chk_fail: 栈金丝雀校验失败\n");
    return 0; });

  return n;
}

}  // namespace divaft