// divaft_Android — libc HLE 实现（原创）
// 分配类走 GuestHeap（返回 guest 地址，FEX 内可解引用）；
// 内存/字符串/非变参 stdio 直接映射宿主（guest 指针恒等映射）。
// 暂缓（需桥接 FP/变参编组或 guest 回调，见 README）：FP 数学、printf 变参族、
// qsort 比较器、C++ ABI / MSVC _Mtx/_Cnd/_Thrd / locale。这些保留记录型 stub。
// SPDX-License-Identifier: GPL-2.0-or-later
#include "divaft/hle_libc.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <ctime>
#include <cerrno>
#include <mutex>
#include <atomic>

namespace divaft {

namespace {
std::mutex g_heap_mu;      // 保护 GuestHeap（非线程安全）
std::mutex g_libc_lock;    // _Locksyslock/_Unlocksyslock

inline char* P(uint64_t a) { return reinterpret_cast<char*>(a); }
inline void* V(uint64_t a) { return reinterpret_cast<void*>(a); }
inline uint64_t S(int r) { return static_cast<uint64_t>(static_cast<int64_t>(r)); }

// FP 编组辅助：fp_args[i] 是 XMMi 低 64 位按位转 double。
// double 参直接用；float 参取低 32 位。返回经 HleReturn.xmm0 写回。
inline double F64(const double* fp, int i) { return fp[i]; }
inline float F32(const double* fp, int i) {
  uint64_t u; std::memcpy(&u, &fp[i], 8);
  uint32_t lo = static_cast<uint32_t>(u);
  float f; std::memcpy(&f, &lo, 4); return f;
}
inline void SetF64(HleReturn* ret, double d) {
  uint64_t u; std::memcpy(&u, &d, 8); ret->xmm0 = u; ret->fp = true;
}
inline void SetF32(HleReturn* ret, float f) {
  uint32_t u; std::memcpy(&u, &f, 4); ret->xmm0 = static_cast<uint64_t>(u); ret->fp = true;
}

// PS4 struct tm 为 9 个 int（无 glibc 的 tm_gmtoff/tm_zone）。guest 侧按此布局。
struct GuestTm { int sec, min, hour, mday, mon, year, wday, yday, isdst; };
}  // namespace

size_t RegisterLibcHle(HleWire& wire, GuestHeap* heap) {
  size_t n = 0;
  auto reg = [&](const char* name, HleFn fn) {
    if (wire.RegisterByName("libc", name, std::move(fn))) ++n;
  };

  // ---------------- 分配（GuestHeap，持锁） ----------------
  reg("malloc", [heap](const uint64_t* a, unsigned) -> uint64_t {
    std::lock_guard<std::mutex> lk(g_heap_mu); return heap->Malloc(a[0]); });
  reg("free", [heap](const uint64_t* a, unsigned) -> uint64_t {
    std::lock_guard<std::mutex> lk(g_heap_mu); heap->Free(a[0]); return 0; });
  reg("calloc", [heap](const uint64_t* a, unsigned) -> uint64_t {
    std::lock_guard<std::mutex> lk(g_heap_mu); return heap->Calloc(a[0], a[1]); });
  reg("realloc", [heap](const uint64_t* a, unsigned) -> uint64_t {
    std::lock_guard<std::mutex> lk(g_heap_mu); return heap->Realloc(a[0], a[1]); });
  reg("memalign", [heap](const uint64_t* a, unsigned) -> uint64_t {
    std::lock_guard<std::mutex> lk(g_heap_mu); return heap->Memalign(a[0], a[1]); });
  reg("posix_memalign", [heap](const uint64_t* a, unsigned) -> uint64_t {
    std::lock_guard<std::mutex> lk(g_heap_mu);
    uint64_t p = heap->Memalign(a[1], a[2]);
    if (a[0]) *reinterpret_cast<uint64_t*>(a[0]) = p;
    return p ? 0 : ENOMEM; });
  reg("_Znwm", [heap](const uint64_t* a, unsigned) -> uint64_t {
    std::lock_guard<std::mutex> lk(g_heap_mu); return heap->Malloc(a[0]); });
  reg("_Znam", [heap](const uint64_t* a, unsigned) -> uint64_t {
    std::lock_guard<std::mutex> lk(g_heap_mu); return heap->Malloc(a[0]); });
  reg("_ZnwmRKSt9nothrow_t", [heap](const uint64_t* a, unsigned) -> uint64_t {
    std::lock_guard<std::mutex> lk(g_heap_mu); return heap->Malloc(a[0]); });
  reg("_ZdlPv", [heap](const uint64_t* a, unsigned) -> uint64_t {
    std::lock_guard<std::mutex> lk(g_heap_mu); heap->Free(a[0]); return 0; });
  reg("_ZdaPv", [heap](const uint64_t* a, unsigned) -> uint64_t {
    std::lock_guard<std::mutex> lk(g_heap_mu); heap->Free(a[0]); return 0; });
  // mspace：简化为单一 guest 堆（忽略 capacity/flags，启动足够）。
  reg("sceLibcMspaceCreate", [](const uint64_t*, unsigned) -> uint64_t { return 1; });
  reg("sceLibcMspaceDestroy", [](const uint64_t*, unsigned) -> uint64_t { return 0; });
  reg("sceLibcMspaceMalloc", [heap](const uint64_t* a, unsigned) -> uint64_t {
    std::lock_guard<std::mutex> lk(g_heap_mu); return heap->Malloc(a[1]); });
  reg("sceLibcMspaceCalloc", [heap](const uint64_t* a, unsigned) -> uint64_t {
    std::lock_guard<std::mutex> lk(g_heap_mu); return heap->Calloc(a[1], a[2]); });
  reg("sceLibcMspaceRealloc", [heap](const uint64_t* a, unsigned) -> uint64_t {
    std::lock_guard<std::mutex> lk(g_heap_mu); return heap->Realloc(a[1], a[2]); });
  reg("sceLibcMspaceFree", [heap](const uint64_t* a, unsigned) -> uint64_t {
    std::lock_guard<std::mutex> lk(g_heap_mu); heap->Free(a[1]); return 0; });
  reg("sceLibcMspaceMemalign", [heap](const uint64_t* a, unsigned) -> uint64_t {
    std::lock_guard<std::mutex> lk(g_heap_mu); return heap->Memalign(a[1], a[2]); });
  reg("sceLibcMspacePosixMemalign", [heap](const uint64_t* a, unsigned) -> uint64_t {
    std::lock_guard<std::mutex> lk(g_heap_mu);
    uint64_t p = heap->Memalign(a[2], a[3]);
    if (a[1]) *reinterpret_cast<uint64_t*>(a[1]) = p;
    return p ? 0 : ENOMEM; });
  reg("sceLibcMspaceReallocalign", [heap](const uint64_t* a, unsigned) -> uint64_t {
    std::lock_guard<std::mutex> lk(g_heap_mu); return heap->Realloc(a[1], a[3]); });
  reg("sceLibcMspaceMallocUsableSize", [heap](const uint64_t* a, unsigned) -> uint64_t {
    std::lock_guard<std::mutex> lk(g_heap_mu); return heap->UsableSize(a[1]); });
  reg("sceLibcMspaceMallocStats", [](const uint64_t*, unsigned) -> uint64_t { return 0; });
  reg("malloc_stats_fast", [](const uint64_t*, unsigned) -> uint64_t { return 0; });

  // ---------------- 内存 / 字符串（宿主，guest 指针可直接解引用） ----------------
  reg("internal_memcpy", [](const uint64_t* a, unsigned) -> uint64_t {
    return a[0] ? (uint64_t)std::memcpy(V(a[0]), V(a[1]), a[2]) : 0; });
  reg("internal_memmove", [](const uint64_t* a, unsigned) -> uint64_t {
    return a[0] ? (uint64_t)std::memmove(V(a[0]), V(a[1]), a[2]) : 0; });
  reg("internal_memset", [](const uint64_t* a, unsigned) -> uint64_t {
    return a[0] ? (uint64_t)std::memset(V(a[0]), (int)a[1], a[2]) : 0; });
  reg("internal_memcmp", [](const uint64_t* a, unsigned) -> uint64_t {
    return S(std::memcmp(V(a[0]), V(a[1]), a[2])); });
  reg("memchr", [](const uint64_t* a, unsigned) -> uint64_t {
    return (uint64_t)std::memchr(V(a[0]), (int)a[1], a[2]); });
  reg("internal_strlen", [](const uint64_t* a, unsigned) -> uint64_t {
    return a[0] ? std::strlen(P(a[0])) : 0; });
  reg("internal_strcmp", [](const uint64_t* a, unsigned) -> uint64_t {
    return S(std::strcmp(P(a[0]), P(a[1]))); });
  reg("internal_strncmp", [](const uint64_t* a, unsigned) -> uint64_t {
    return S(std::strncmp(P(a[0]), P(a[1]), a[2])); });
  reg("internal_strncpy", [](const uint64_t* a, unsigned) -> uint64_t {
    return (uint64_t)std::strncpy(P(a[0]), P(a[1]), a[2]); });
  reg("internal_strcat", [](const uint64_t* a, unsigned) -> uint64_t {
    return (uint64_t)std::strcat(P(a[0]), P(a[1])); });
  reg("internal_strcpy_s", [](const uint64_t* a, unsigned) -> uint64_t {
    if (!a[0] || !a[1]) return EINVAL;
    std::strncpy(P(a[0]), P(a[1]), a[2]); P(a[0])[a[2] ? a[2]-1 : 0] = 0; return 0; });
  reg("strcpy", [](const uint64_t* a, unsigned) -> uint64_t {
    return (uint64_t)std::strcpy(P(a[0]), P(a[1])); });
  reg("strstr", [](const uint64_t* a, unsigned) -> uint64_t {
    return (uint64_t)std::strstr(P(a[0]), P(a[1])); });
  reg("strdup", [heap](const uint64_t* a, unsigned) -> uint64_t {
    if (!a[0]) return 0;
    size_t len = std::strlen(P(a[0])) + 1;
    std::lock_guard<std::mutex> lk(g_heap_mu);
    uint64_t p = heap->Malloc(len);
    if (p) std::memcpy(V(p), V(a[0]), len);
    return p; });
  reg("strtol", [](const uint64_t* a, unsigned) -> uint64_t {
    char* end = nullptr;
    long r = std::strtol(P(a[0]), a[1] ? &end : nullptr, (int)a[2]);
    if (a[1]) *reinterpret_cast<uint64_t*>(a[1]) = (uint64_t)end;
    return S((int)r); });
  reg("_Stoul", [](const uint64_t* a, unsigned) -> uint64_t {
    char* end = nullptr;
    unsigned long r = std::strtoul(P(a[0]), a[1] ? &end : nullptr, (int)a[2]);
    if (a[1]) *reinterpret_cast<uint64_t*>(a[1]) = (uint64_t)end;
    return r; });
  reg("wcslen", [](const uint64_t* a, unsigned) -> uint64_t {
    return a[0] ? std::wcslen(reinterpret_cast<const wchar_t*>(a[0])) : 0; });
  reg("wmemcpy", [](const uint64_t* a, unsigned) -> uint64_t {
    return (uint64_t)std::wmemcpy(reinterpret_cast<wchar_t*>(a[0]),
                                  reinterpret_cast<const wchar_t*>(a[1]), a[2]); });
  reg("wmemmove", [](const uint64_t* a, unsigned) -> uint64_t {
    return (uint64_t)std::wmemmove(reinterpret_cast<wchar_t*>(a[0]),
                                   reinterpret_cast<const wchar_t*>(a[1]), a[2]); });
  reg("wmemset", [](const uint64_t* a, unsigned) -> uint64_t {
    return (uint64_t)std::wmemset(reinterpret_cast<wchar_t*>(a[0]), (wchar_t)a[1], a[2]); });
  reg("wmemcmp", [](const uint64_t* a, unsigned) -> uint64_t {
    return S(std::wmemcmp(reinterpret_cast<const wchar_t*>(a[0]),
                          reinterpret_cast<const wchar_t*>(a[1]), a[2])); });
  reg("wmemchr", [](const uint64_t* a, unsigned) -> uint64_t {
    return (uint64_t)std::wmemchr(reinterpret_cast<const wchar_t*>(a[0]), (wchar_t)a[1], a[2]); });

  // ---------------- 非变参 stdio（宿主 FILE*；game 多回传给 libc，暂可接受） ----------------
  reg("puts", [](const uint64_t* a, unsigned) -> uint64_t { return S(std::puts(P(a[0]))); });
  reg("fputs", [](const uint64_t* a, unsigned) -> uint64_t {
    return S(std::fputs(P(a[0]), reinterpret_cast<FILE*>(a[1]))); });
  reg("fputc", [](const uint64_t* a, unsigned) -> uint64_t {
    return S(std::fputc((int)a[0], reinterpret_cast<FILE*>(a[1]))); });
  reg("fwrite", [](const uint64_t* a, unsigned) -> uint64_t {
    return std::fwrite(V(a[0]), a[1], a[2], reinterpret_cast<FILE*>(a[3])); });
  reg("fread", [](const uint64_t* a, unsigned) -> uint64_t {
    return std::fread(V(a[0]), a[1], a[2], reinterpret_cast<FILE*>(a[3])); });
  reg("internal_fread", [](const uint64_t* a, unsigned) -> uint64_t {
    return std::fread(V(a[0]), a[1], a[2], reinterpret_cast<FILE*>(a[3])); });
  reg("fopen", [](const uint64_t* a, unsigned) -> uint64_t {
    return (uint64_t)std::fopen(P(a[0]), P(a[1])); });
  reg("internal_fopen", [](const uint64_t* a, unsigned) -> uint64_t {
    return (uint64_t)std::fopen(P(a[0]), P(a[1])); });
  reg("fopen_s", [](const uint64_t* a, unsigned) -> uint64_t {
    FILE* f = std::fopen(P(a[1]), P(a[2]));
    if (a[0]) *reinterpret_cast<uint64_t*>(a[0]) = (uint64_t)f;
    return f ? 0 : EINVAL; });
  reg("fclose", [](const uint64_t* a, unsigned) -> uint64_t {
    return S(std::fclose(reinterpret_cast<FILE*>(a[0]))); });
  reg("internal_fclose", [](const uint64_t* a, unsigned) -> uint64_t {
    return S(std::fclose(reinterpret_cast<FILE*>(a[0]))); });
  reg("fseek", [](const uint64_t* a, unsigned) -> uint64_t {
    return S(std::fseek(reinterpret_cast<FILE*>(a[0]), (long)a[1], (int)a[2])); });
  reg("internal_fseek", [](const uint64_t* a, unsigned) -> uint64_t {
    return S(std::fseek(reinterpret_cast<FILE*>(a[0]), (long)a[1], (int)a[2])); });
  reg("ftell", [](const uint64_t* a, unsigned) -> uint64_t {
    return S((int)std::ftell(reinterpret_cast<FILE*>(a[0]))); });
  reg("fflush", [](const uint64_t* a, unsigned) -> uint64_t {
    return S(std::fflush(reinterpret_cast<FILE*>(a[0]))); });
  reg("internal_fflush", [](const uint64_t* a, unsigned) -> uint64_t {
    return S(std::fflush(reinterpret_cast<FILE*>(a[0]))); });
  reg("feof", [](const uint64_t* a, unsigned) -> uint64_t {
    return S(std::feof(reinterpret_cast<FILE*>(a[0]))); });
  reg("fgetc", [](const uint64_t* a, unsigned) -> uint64_t {
    return S(std::fgetc(reinterpret_cast<FILE*>(a[0]))); });
  reg("ungetc", [](const uint64_t* a, unsigned) -> uint64_t {
    return S(std::ungetc((int)a[0], reinterpret_cast<FILE*>(a[1]))); });
  reg("setvbuf", [](const uint64_t* a, unsigned) -> uint64_t {
    return S(std::setvbuf(reinterpret_cast<FILE*>(a[0]), P(a[1]), (int)a[2], a[3])); });

  // ---------------- 杂项 ----------------
  reg("abs", [](const uint64_t* a, unsigned) -> uint64_t { return S(std::abs((int)a[0])); });
  reg("labs", [](const uint64_t* a, unsigned) -> uint64_t { return S((int)std::labs((long)a[0])); });
  reg("ldiv", [](const uint64_t* a, unsigned) -> uint64_t {
    // ldiv_t{quot,rem} 两个 int，按值返回装进 RAX（低 quot 高 rem，小端）。
    std::ldiv_t r = std::ldiv((long)a[0], (long)a[1]);
    return ((uint64_t)(uint32_t)r.rem << 32) | (uint32_t)r.quot; });
  reg("rand", [](const uint64_t*, unsigned) -> uint64_t { return S(std::rand()); });
  reg("srand", [](const uint64_t* a, unsigned) -> uint64_t { std::srand((unsigned)a[0]); return 0; });
  reg("time", [](const uint64_t* a, unsigned) -> uint64_t {
    time_t t = std::time(nullptr); if (a[0]) *reinterpret_cast<uint64_t*>(a[0]) = (uint64_t)t;
    return (uint64_t)t; });
  reg("strerror", [](const uint64_t* a, unsigned) -> uint64_t {
    return (uint64_t)std::strerror((int)a[0]); });
  reg("exit", [](const uint64_t* a, unsigned) -> uint64_t {
    std::fprintf(stderr, "[libc] exit(%d)\n", (int)a[0]); std::_Exit((int)a[0]); return 0; });
  reg("atexit", [](const uint64_t*, unsigned) -> uint64_t { return 0; }); // guest 回调暂缓
  reg("_Locksyslock", [](const uint64_t*, unsigned) -> uint64_t { g_libc_lock.lock(); return 0; });
  reg("_Unlocksyslock", [](const uint64_t*, unsigned) -> uint64_t { g_libc_lock.unlock(); return 0; });
  reg("_Atomic_fetch_add_4", [](const uint64_t* a, unsigned) -> uint64_t {
    return __atomic_fetch_add(reinterpret_cast<uint32_t*>(a[0]), (uint32_t)a[1], __ATOMIC_SEQ_CST); });
  reg("_Atomic_fetch_sub_4", [](const uint64_t* a, unsigned) -> uint64_t {
    return __atomic_fetch_sub(reinterpret_cast<uint32_t*>(a[0]), (uint32_t)a[1], __ATOMIC_SEQ_CST); });
  reg("_Atomic_copy", [](const uint64_t* a, unsigned) -> uint64_t {
    __atomic_store(reinterpret_cast<uint32_t*>(a[0]),
                   reinterpret_cast<uint32_t*>(a[1]), __ATOMIC_SEQ_CST); return 0; });
  reg("_Xtime_get_ticks", [](const uint64_t*, unsigned) -> uint64_t {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 10000000ULL + ts.tv_nsec / 100; });

  // ---------------- FP 数学（扩展注册：读 fp_args，经 HleReturn 写回 XMM0） ----------------
  auto regExt = [&](const char* name, HostFn fn) {
    if (wire.RegisterByNameExt("libc", name, std::move(fn))) ++n;
  };
  // double 一元
  regExt("internal_acos", [](const uint64_t*, unsigned, const double* fp, HleReturn* r) -> uint64_t {
    SetF64(r, std::acos(F64(fp,0))); return 0; });
  regExt("internal_atan", [](const uint64_t*, unsigned, const double* fp, HleReturn* r) -> uint64_t {
    SetF64(r, std::atan(F64(fp,0))); return 0; });
  regExt("internal_exp", [](const uint64_t*, unsigned, const double* fp, HleReturn* r) -> uint64_t {
    SetF64(r, std::exp(F64(fp,0))); return 0; });
  regExt("_Sin", [](const uint64_t*, unsigned, const double* fp, HleReturn* r) -> uint64_t {
    SetF64(r, std::sin(F64(fp,0))); return 0; });
  regExt("_Log", [](const uint64_t*, unsigned, const double* fp, HleReturn* r) -> uint64_t {
    SetF64(r, std::log(F64(fp,0))); return 0; });
  // double 二元
  regExt("internal_atan2", [](const uint64_t*, unsigned, const double* fp, HleReturn* r) -> uint64_t {
    SetF64(r, std::atan2(F64(fp,0), F64(fp,1))); return 0; });
  regExt("internal_pow", [](const uint64_t*, unsigned, const double* fp, HleReturn* r) -> uint64_t {
    SetF64(r, std::pow(F64(fp,0), F64(fp,1))); return 0; });
  regExt("fmod", [](const uint64_t*, unsigned, const double* fp, HleReturn* r) -> uint64_t {
    SetF64(r, std::fmod(F64(fp,0), F64(fp,1))); return 0; });
  // double + int / 指针
  regExt("ldexp", [](const uint64_t* a, unsigned, const double* fp, HleReturn* r) -> uint64_t {
    SetF64(r, std::ldexp(F64(fp,0), (int)a[0])); return 0; });
  regExt("modf", [](const uint64_t* a, unsigned, const double* fp, HleReturn* r) -> uint64_t {
    double ip = 0; double frac = std::modf(F64(fp,0), &ip);
    if (a[0]) *reinterpret_cast<double*>(a[0]) = ip;
    SetF64(r, frac); return 0; });
  // float 一元
  regExt("internal_acosf", [](const uint64_t*, unsigned, const double* fp, HleReturn* r) -> uint64_t {
    SetF32(r, std::acos(F32(fp,0))); return 0; });
  regExt("internal_asinf", [](const uint64_t*, unsigned, const double* fp, HleReturn* r) -> uint64_t {
    SetF32(r, std::asin(F32(fp,0))); return 0; });
  regExt("internal_atanf", [](const uint64_t*, unsigned, const double* fp, HleReturn* r) -> uint64_t {
    SetF32(r, std::atan(F32(fp,0))); return 0; });
  regExt("internal_expf", [](const uint64_t*, unsigned, const double* fp, HleReturn* r) -> uint64_t {
    SetF32(r, std::exp(F32(fp,0))); return 0; });
  regExt("internal_tanf", [](const uint64_t*, unsigned, const double* fp, HleReturn* r) -> uint64_t {
    SetF32(r, std::tan(F32(fp,0))); return 0; });
  regExt("fabsf", [](const uint64_t*, unsigned, const double* fp, HleReturn* r) -> uint64_t {
    SetF32(r, std::fabs(F32(fp,0))); return 0; });
  regExt("ceilf", [](const uint64_t*, unsigned, const double* fp, HleReturn* r) -> uint64_t {
    SetF32(r, std::ceil(F32(fp,0))); return 0; });
  regExt("floorf", [](const uint64_t*, unsigned, const double* fp, HleReturn* r) -> uint64_t {
    SetF32(r, std::floor(F32(fp,0))); return 0; });
  regExt("sqrtf", [](const uint64_t*, unsigned, const double* fp, HleReturn* r) -> uint64_t {
    SetF32(r, std::sqrt(F32(fp,0))); return 0; });
  regExt("roundf", [](const uint64_t*, unsigned, const double* fp, HleReturn* r) -> uint64_t {
    SetF32(r, std::round(F32(fp,0))); return 0; });
  regExt("_FSin", [](const uint64_t*, unsigned, const double* fp, HleReturn* r) -> uint64_t {
    SetF32(r, std::sin(F32(fp,0))); return 0; });
  regExt("_FLog", [](const uint64_t*, unsigned, const double* fp, HleReturn* r) -> uint64_t {
    SetF32(r, std::log(F32(fp,0))); return 0; });
  // float 二元 / +int / +指针
  regExt("internal_powf", [](const uint64_t*, unsigned, const double* fp, HleReturn* r) -> uint64_t {
    SetF32(r, std::pow(F32(fp,0), F32(fp,1))); return 0; });
  regExt("fmodf", [](const uint64_t*, unsigned, const double* fp, HleReturn* r) -> uint64_t {
    SetF32(r, std::fmod(F32(fp,0), F32(fp,1))); return 0; });
  regExt("ldexpf", [](const uint64_t* a, unsigned, const double* fp, HleReturn* r) -> uint64_t {
    SetF32(r, std::ldexp(F32(fp,0), (int)a[0])); return 0; });
  regExt("frexpf", [](const uint64_t* a, unsigned, const double* fp, HleReturn* r) -> uint64_t {
    int e = 0; float v = std::frexp(F32(fp,0), &e);
    if (a[0]) *reinterpret_cast<int*>(a[0]) = e;
    SetF32(r, v); return 0; });
  regExt("modff", [](const uint64_t* a, unsigned, const double* fp, HleReturn* r) -> uint64_t {
    float ip = 0; float frac = std::modf(F32(fp,0), &ip);
    if (a[0]) *reinterpret_cast<float*>(a[0]) = ip;
    SetF32(r, frac); return 0; });
  // 分类（返回 int 于 RAX，不写 XMM）
  regExt("__isfinite", [](const uint64_t*, unsigned, const double* fp, HleReturn*) -> uint64_t {
    return std::isfinite(F64(fp,0)) ? 1 : 0; });
  regExt("__isinf", [](const uint64_t*, unsigned, const double* fp, HleReturn*) -> uint64_t {
    return std::isinf(F64(fp,0)) ? 1 : 0; });
  regExt("__isnan", [](const uint64_t*, unsigned, const double* fp, HleReturn*) -> uint64_t {
    return std::isnan(F64(fp,0)) ? 1 : 0; });
  regExt("__signbit", [](const uint64_t*, unsigned, const double* fp, HleReturn*) -> uint64_t {
    return std::signbit(F64(fp,0)) ? 1 : 0; });
  regExt("__isinff", [](const uint64_t*, unsigned, const double* fp, HleReturn*) -> uint64_t {
    return std::isinf(F32(fp,0)) ? 1 : 0; });
  regExt("__isnanf", [](const uint64_t*, unsigned, const double* fp, HleReturn*) -> uint64_t {
    return std::isnan(F32(fp,0)) ? 1 : 0; });
  regExt("__signbitf", [](const uint64_t*, unsigned, const double* fp, HleReturn*) -> uint64_t {
    return std::signbit(F32(fp,0)) ? 1 : 0; });

  return n;
}

}  // namespace divaft