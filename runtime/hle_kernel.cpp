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
#include <cstring>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/resource.h>

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

size_t RegisterKernelHle(HleWire& wire, GuestMemoryManager* mem, GuestBridge* bridge,
                         uint64_t proc_param_addr) {
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

  // ---- 自带 libc 所需 kernel（scePthread* 变体 + 内存/时间），按 NID 注册 ----
  // 这些 NID 不在 eboot 契约里（libc 自身导入），故用显式 NID。多数复用现有 pthread backing。
  auto regnid = [&](const char* nid, const char* name, HleFn fn) {
    wire.Register(nid, "libkernel", name, std::move(fn)); ++n;
  };
  // pthread 条件变量 sce 变体
  regnid("kDh-NfxgMtE", "scePthreadCondSignal", [](const uint64_t* a, unsigned) -> uint64_t {
    pthread_cond_t* c; { std::lock_guard<std::mutex> lk(g_sync_mu); c = GetCond(a[0]); }
    return c && pthread_cond_signal(c) == 0 ? 0 : 22; });
  regnid("JGgj7Uvrl+A", "scePthreadCondBroadcast", [](const uint64_t* a, unsigned) -> uint64_t {
    pthread_cond_t* c; { std::lock_guard<std::mutex> lk(g_sync_mu); c = GetCond(a[0]); }
    return c && pthread_cond_broadcast(c) == 0 ? 0 : 22; });
  regnid("g+PZd2hiacg", "scePthreadCondDestroy", [](const uint64_t* a, unsigned) -> uint64_t {
    std::lock_guard<std::mutex> lk(g_sync_mu);
    auto it = g_conds.find(a[0]); if (it == g_conds.end()) return 0;
    pthread_cond_destroy(it->second); delete it->second; g_conds.erase(it); return 0; });
  regnid("BmMjYxmew1w", "scePthreadCondTimedwait", [](const uint64_t* a, unsigned) -> uint64_t {
    pthread_cond_t* c; pthread_mutex_t* m;
    { std::lock_guard<std::mutex> lk(g_sync_mu); c = GetCond(a[0]); m = GetMutex(a[1]); }
    if (!c || !m) return 22;
    struct timespec ts { (time_t)(a[2] / 1000000000ULL), (long)(a[2] % 1000000000ULL) };
    int r = pthread_cond_timedwait(c, m, &ts); return r == 0 ? 0 : r; });
  regnid("upoVrzMHFeE", "scePthreadMutexTrylock", [](const uint64_t* a, unsigned) -> uint64_t {
    pthread_mutex_t* m; { std::lock_guard<std::mutex> lk(g_sync_mu); m = GetMutex(a[0]); }
    if (!m) return 22; int r = pthread_mutex_trylock(m); return r == 0 ? 0 : r; });
  // mutexattr / attr（形式参数，安全默认）
  regnid("F8bUHwAG284", "scePthreadMutexattrInit", [](const uint64_t*, unsigned) -> uint64_t { return 0; });
  regnid("smWEktiyyG0", "scePthreadMutexattrDestroy", [](const uint64_t*, unsigned) -> uint64_t { return 0; });
  regnid("iMp8QpE+XO4", "scePthreadMutexattrSettype", [](const uint64_t*, unsigned) -> uint64_t { return 0; });
  regnid("x1X76arYMxU", "scePthreadAttrGet", [](const uint64_t*, unsigned) -> uint64_t { return 0; });
  regnid("8+s5BzZjxSg", "scePthreadAttrGetaffinity", [](const uint64_t*, unsigned) -> uint64_t { return 0; });
  regnid("JaRMy+QcpeU", "scePthreadAttrGetdetachstate", [](const uint64_t* a, unsigned) -> uint64_t {
    if (a[1]) *reinterpret_cast<int*>(a[1]) = 0; return 0; });
  regnid("3PtV6p3QNX4", "scePthreadEqual", [](const uint64_t* a, unsigned) -> uint64_t {
    return pthread_equal((pthread_t)a[0], (pthread_t)a[1]) ? 1 : 0; });
  regnid("3kg7rT0NQIs", "scePthreadExit", [](const uint64_t* a, unsigned) -> uint64_t {
    pthread_exit(reinterpret_cast<void*>(a[0])); return 0; });
  regnid("geDaqgH9lTg", "scePthreadKeyCreate", [](const uint64_t* a, unsigned) -> uint64_t {
    pthread_key_t k; int r = pthread_key_create(&k, nullptr);
    if (r == 0 && a[0]) *reinterpret_cast<uint32_t*>(a[0]) = (uint32_t)k; return r; });
  regnid("PrdHuuDekhY", "scePthreadKeyDelete", [](const uint64_t* a, unsigned) -> uint64_t {
    return pthread_key_delete((pthread_key_t)a[0]); });
  regnid("eoht7mQOCmo", "scePthreadGetspecific", [](const uint64_t* a, unsigned) -> uint64_t {
    return (uint64_t)pthread_getspecific((pthread_key_t)a[0]); });
  regnid("+BzXYkqYeLE", "scePthreadSetspecific", [](const uint64_t* a, unsigned) -> uint64_t {
    return pthread_setspecific((pthread_key_t)a[0], reinterpret_cast<void*>(a[1])); });
  regnid("14bOACANTBo", "scePthreadOnce", [bridge](const uint64_t* a, unsigned) -> uint64_t {
    static std::mutex once_mu; static std::unordered_map<uint64_t, bool> done;
    std::lock_guard<std::mutex> lk(once_mu);
    if (!done[a[0]]) { done[a[0]] = true; if (a[1]) bridge->CallGuest(a[1], nullptr, 0); }
    return 0; });
  regnid("6XG4B33N09g", "sched_yield", [](const uint64_t*, unsigned) -> uint64_t { sched_yield(); return 0; });
  // 内存 / 时间
  regnid("BPE9s9vQQXo", "mmap", [](const uint64_t* a, unsigned) -> uint64_t {
    // 简化 guest mmap：低4G bump 分配（规避 FEX-qemu 高地址截断），忽略 fd/offset。
    static std::mutex mmap_mu; static uint64_t cursor = 0x50000000;
    std::lock_guard<std::mutex> lk(mmap_mu);
    uint64_t len = (a[1] + 4095) & ~4095ULL; if (len == 0) len = 4096;
    uint64_t r = cursor;
    if (r + len >= 0xF0000000ULL) return (uint64_t)-1;
    void* p = mmap(reinterpret_cast<void*>(r), len, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (p == MAP_FAILED) return (uint64_t)-1;
    cursor = r + len; return r; });
  regnid("k+AXqu2-eBc", "getpagesize", [](const uint64_t*, unsigned) -> uint64_t {
    return (uint64_t)sysconf(_SC_PAGESIZE); });
  regnid("lLMT9vJAck0", "clock_gettime", [](const uint64_t* a, unsigned) -> uint64_t {
    struct timespec ts; int r = clock_gettime((clockid_t)a[0], &ts);
    if (r == 0 && a[1]) { auto* g = reinterpret_cast<uint64_t*>(a[1]); g[0] = ts.tv_sec; g[1] = ts.tv_nsec; }
    return r; });
  regnid("n88vx3C5nW8", "gettimeofday", [](const uint64_t* a, unsigned) -> uint64_t {
    struct timeval tv; int r = gettimeofday(&tv, nullptr);
    if (r == 0 && a[0]) { auto* g = reinterpret_cast<uint64_t*>(a[0]); g[0] = tv.tv_sec; g[1] = tv.tv_usec; }
    return r; });
  regnid("yS8U2TGCe1A", "nanosleep", [](const uint64_t* a, unsigned) -> uint64_t {
    if (!a[0]) return 22; auto* g = reinterpret_cast<const uint64_t*>(a[0]);
    struct timespec ts { (time_t)g[0], (long)g[1] }; nanosleep(&ts, nullptr); return 0; });
  regnid("NhpspxdjEKU", "_nanosleep", [](const uint64_t* a, unsigned) -> uint64_t {
    if (!a[0]) return 22; auto* g = reinterpret_cast<const uint64_t*>(a[0]);
    struct timespec ts { (time_t)g[0], (long)g[1] }; nanosleep(&ts, nullptr); return 0; });
  regnid("6Z83sYWFlA8", "_exit", [](const uint64_t* a, unsigned) -> uint64_t {
    std::fprintf(stderr, "[kernel] _exit(%d)\n", (int)a[0]); _exit((int)a[0]); return 0; });
  regnid("hHlZQUnlxSM", "getrusage", [](const uint64_t* a, unsigned) -> uint64_t {
    struct rusage ru; int r = getrusage((int)a[0], &ru);
    if (r == 0 && a[1]) std::memset(reinterpret_cast<void*>(a[1]), 0, 144);
    return r; });
  regnid("9BcDykPmo1I", "__error", [](const uint64_t*, unsigned) -> uint64_t {
    static int guest_errno = 0; return (uint64_t)&guest_errno; });
  // 进程参数：libc 预初始化靠它拿堆/libc 配置，返回 eboot 内嵌 OrbisProcParam 的 guest 地址。
  regnid("959qrazPIrg", "sceKernelGetProcParam", [proc_param_addr](const uint64_t*, unsigned) -> uint64_t {
    return proc_param_addr; });
  regnid("OMDRKKAZ8I4", "sceKernelDebugRaiseException", [](const uint64_t* a, unsigned) -> uint64_t {
    std::fprintf(stderr, "[kernel] DebugRaiseException code=0x%llx（libc 初始化报错，多为前置参数缺失）\n",
                 (unsigned long long)a[0]);
    return 0; });
  // libc 经此把自身堆 API(malloc/free/...)注册给 libkernel（供 TLS 分配等回调）。存下指针即可。
  regnid("p5EcQeEeJAE", "_sceKernelRtldSetApplicationHeapAPI", [](const uint64_t* a, unsigned) -> uint64_t {
    static uint64_t heap_api = 0;
    heap_api = a[0];
    std::fprintf(stderr, "[kernel] RtldSetApplicationHeapAPI api=0x%llx\n", (unsigned long long)a[0]);
    return 0; });
  regnid("NWtTN10cJzE", "sceLibcHeapGetTraceInfo", [](const uint64_t*, unsigned) -> uint64_t { return 0; });

  return n;
}

}  // namespace divaft