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
#include <random>
#include <pthread.h>
#include <condition_variable>
#include <atomic>
#include <string>

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

// ---- guest x86-64 变参提取（printf 族） ----
// SysV AMD64：整数参依次 RDI,RSI,RDX,RCX,R8,R9 后转栈；SSE 参依次 XMM0..7 后转栈。
// 桥接已编组：ia[0]=format，ia[1..5]=整数寄存器参，ia[6..13]=栈溢出参（按声明序），fa[0..7]=XMM。
// 跨 ABI：不构造 va_list（guest x86-64 与宿主 ARM64 不同），改为逐 specifier 取值后用宿主 snprintf。
struct GuestVarArgs {
  const uint64_t* ia;
  const double* fa;
  int int_reg = 1;   // 下一个整数寄存器参下标（1..5；0 是 format）
  int fp_reg = 0;    // 下一个 SSE 寄存器参下标（0..7）
  int stack = 6;     // 下一个栈溢出参下标（6..13）
  uint64_t next_int() {
    if (int_reg <= 5) return ia[int_reg++];
    if (stack <= 13) return ia[stack++];
    return 0;
  }
  double next_fp() {
    if (fp_reg <= 7) return fa[fp_reg++];
    if (stack <= 13) { uint64_t u = ia[stack++]; double d; std::memcpy(&d, &u, 8); return d; }
    return 0.0;
  }
};

// 按格式串格式化到 out（容量 outsz，含结尾 0）。返回写入字符数（不含 0）。
static size_t FormatGuest(char* out, size_t outsz, const char* fmt, GuestVarArgs& va) {
  if (!out || outsz == 0) return 0;
  size_t oi = 0;
  auto emit = [&](const char* s) {
    for (const char* q = s; *q && oi + 1 < outsz; ++q) out[oi++] = *q;
  };
  for (const char* p = fmt; *p;) {
    if (*p != '%') {
      if (oi + 1 < outsz) out[oi++] = *p;
      ++p; continue;
    }
    ++p;
    if (*p == '%') { if (oi + 1 < outsz) out[oi++] = '%'; ++p; continue; }
    // flags
    std::string flags;
    while (*p == '-' || *p == '+' || *p == ' ' || *p == '#' || *p == '0') flags += *p++;
    // width
    std::string width;
    if (*p == '*') { width = std::to_string((int)va.next_int()); ++p; }
    else while (*p >= '0' && *p <= '9') width += *p++;
    // precision
    std::string prec;
    if (*p == '.') {
      ++p;
      if (*p == '*') { prec = std::to_string((int)va.next_int()); ++p; }
      else while (*p >= '0' && *p <= '9') prec += *p++;
      prec = "." + prec;
    }
    // length
    std::string len;
    while (*p == 'h' || *p == 'l' || *p == 'z' || *p == 'j' || *p == 't' || *p == 'L') len += *p++;
    char spec = *p;
    if (!spec) break;
    ++p;
    std::string sf = "%" + flags + width + prec + len + spec;
    char tmp[1024];
    tmp[0] = 0;
    switch (spec) {
      case 'd': case 'i': {
        uint64_t v = va.next_int();
        if (len == "hh") std::snprintf(tmp, sizeof tmp, sf.c_str(), (signed char)v);
        else if (len == "h") std::snprintf(tmp, sizeof tmp, sf.c_str(), (short)v);
        else if (len == "l" || len == "ll" || len == "z" || len == "j" || len == "t")
          std::snprintf(tmp, sizeof tmp, sf.c_str(), (long long)v);
        else std::snprintf(tmp, sizeof tmp, sf.c_str(), (int)v);
        break;
      }
      case 'u': case 'x': case 'X': case 'o': {
        uint64_t v = va.next_int();
        if (len == "hh") std::snprintf(tmp, sizeof tmp, sf.c_str(), (unsigned char)v);
        else if (len == "h") std::snprintf(tmp, sizeof tmp, sf.c_str(), (unsigned short)v);
        else if (len == "l" || len == "ll" || len == "z" || len == "j" || len == "t")
          std::snprintf(tmp, sizeof tmp, sf.c_str(), (unsigned long long)v);
        else std::snprintf(tmp, sizeof tmp, sf.c_str(), (unsigned)v);
        break;
      }
      case 'c': {
        int ch = (int)va.next_int();
        tmp[0] = (char)ch; tmp[1] = 0;
        break;
      }
      case 's': {
        uint64_t sp = va.next_int();
        const char* s = sp ? reinterpret_cast<const char*>(sp) : "(null)";
        std::snprintf(tmp, sizeof tmp, sf.c_str(), s);
        break;
      }
      case 'p': {
        uint64_t pv = va.next_int();
        std::snprintf(tmp, sizeof tmp, sf.c_str(), reinterpret_cast<void*>(pv));
        break;
      }
      case 'f': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
        double d = va.next_fp();
        std::snprintf(tmp, sizeof tmp, sf.c_str(), d);
        break;
      }
      case 'n': {
        uint64_t np = va.next_int();
        if (np) *reinterpret_cast<int*>(np) = (int)oi;
        break;
      }
      default:
        // 未知 specifier：原样输出，避免崩溃
        tmp[0] = spec; tmp[1] = 0;
        break;
    }
    emit(tmp);
  }
  out[oi] = 0;
  return oi;
}
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

  // ---------------- printf 变参族（跨 ABI 逐项格式化） ----------------
  regExt("printf", [](const uint64_t* a, unsigned, const double* fp, HleReturn*) -> uint64_t {
    if (!a[0]) return 0;
    GuestVarArgs va{a, fp};                 // format=a[0]，变参整数从 a[1]
    char buf[4096];
    size_t n = FormatGuest(buf, sizeof buf, reinterpret_cast<const char*>(a[0]), va);
    std::fputs(buf, stdout);
    return n; });
  regExt("fprintf", [](const uint64_t* a, unsigned, const double* fp, HleReturn*) -> uint64_t {
    if (!a[1]) return 0;
    GuestVarArgs va{a, fp}; va.int_reg = 2;  // format=a[1]（a[0]=FILE*）
    char buf[4096];
    size_t n = FormatGuest(buf, sizeof buf, reinterpret_cast<const char*>(a[1]), va);
    FILE* f = reinterpret_cast<FILE*>(a[0]);
    if (f) std::fputs(buf, f);
    return n; });
  regExt("internal_snprintf", [](const uint64_t* a, unsigned, const double* fp, HleReturn*) -> uint64_t {
    if (!a[2]) return 0;
    GuestVarArgs va{a, fp}; va.int_reg = 3;  // (buf, size, fmt, ...)
    char* buf = reinterpret_cast<char*>(a[0]);
    if (!buf || a[1] == 0) return 0;
    return FormatGuest(buf, a[1], reinterpret_cast<const char*>(a[2]), va); });
  regExt("sprintf_s", [](const uint64_t* a, unsigned, const double* fp, HleReturn*) -> uint64_t {
    if (!a[2]) return 0;
    GuestVarArgs va{a, fp}; va.int_reg = 3;  // (buf, size, fmt, ...)
    char* buf = reinterpret_cast<char*>(a[0]);
    if (!buf || a[1] == 0) return 0;
    return FormatGuest(buf, a[1], reinterpret_cast<const char*>(a[2]), va); });

  // std::random_device()() → 宿主随机数（桩返回0会致随机种子恒定）
  reg("_ZSt14_Random_devicev", [](const uint64_t*, unsigned) -> uint64_t {
    static std::mt19937 rng{std::random_device{}()};
    return rng(); });
  // ---------------- C++ ABI guard（Meyers 单例懒初始化，返回0会跳过构造致空表崩溃）----------------
  auto guard_acquire = [](const uint64_t* a, unsigned) -> uint64_t {
    auto* g = reinterpret_cast<uint8_t*>(a[0]);
    if (*g == 0) { *g = 1; return 1; }  // 首次：标记进行中，返回1让调用方跑构造
    return 0;                            // 已初始化：跳过
  };
  auto guard_release = [](const uint64_t* a, unsigned) -> uint64_t {
    *reinterpret_cast<uint8_t*>(a[0]) = 1; return 0;
  };
  auto guard_abort = [](const uint64_t* a, unsigned) -> uint64_t {
    *reinterpret_cast<uint8_t*>(a[0]) = 0; return 0;
  };
  reg("internal__cxa_guard_acquire", guard_acquire);
  reg("internal__cxa_guard_release", guard_release);
  reg("internal__cxa_guard_abort", guard_abort);
  reg("__cxa_guard_acquire", guard_acquire);
  reg("__cxa_guard_release", guard_release);
  reg("__cxa_guard_abort", guard_abort);
  reg("__cxa_pure_virtual", [](const uint64_t*, unsigned) -> uint64_t {
    std::fprintf(stderr, "[libc] __cxa_pure_virtual 调用！\n"); std::abort(); return 0; });
  reg("_ZSt9terminatev", [](const uint64_t*, unsigned) -> uint64_t {
    std::fprintf(stderr, "[libc] std::terminate 调用！\n"); std::abort(); return 0; });

  // ---------------- MSVC 线程原语 _Mtx/_Cnd/_Thrd（宿主 backing，按值传 opaque 指针）----------------
  reg("_Mtx_init", [](const uint64_t* a, unsigned) -> uint64_t {
    *reinterpret_cast<uint64_t*>(a[0]) =
        reinterpret_cast<uint64_t>(new std::recursive_mutex());
    return 0; });
  reg("_Mtx_destroy", [](const uint64_t* a, unsigned) -> uint64_t {
    delete reinterpret_cast<std::recursive_mutex*>(*reinterpret_cast<uint64_t*>(a[0])); return 0; });
  reg("_Mtx_lock", [](const uint64_t* a, unsigned) -> uint64_t {
    reinterpret_cast<std::recursive_mutex*>(*reinterpret_cast<uint64_t*>(a[0]))->lock(); return 0; });
  reg("_Mtx_unlock", [](const uint64_t* a, unsigned) -> uint64_t {
    reinterpret_cast<std::recursive_mutex*>(*reinterpret_cast<uint64_t*>(a[0]))->unlock(); return 0; });
  reg("_Mtx_trylock", [](const uint64_t* a, unsigned) -> uint64_t {
    return reinterpret_cast<std::recursive_mutex*>(*reinterpret_cast<uint64_t*>(a[0]))->try_lock() ? 0 : 1; });
  reg("_Mtx_current_owns", [](const uint64_t*, unsigned) -> uint64_t { return 1; });
  reg("_Cnd_init", [](const uint64_t* a, unsigned) -> uint64_t {
    *reinterpret_cast<uint64_t*>(a[0]) =
        reinterpret_cast<uint64_t>(new std::condition_variable_any());
    return 0; });
  reg("_Cnd_destroy", [](const uint64_t* a, unsigned) -> uint64_t {
    delete reinterpret_cast<std::condition_variable_any*>(*reinterpret_cast<uint64_t*>(a[0])); return 0; });
  reg("_Cnd_signal", [](const uint64_t* a, unsigned) -> uint64_t {
    reinterpret_cast<std::condition_variable_any*>(*reinterpret_cast<uint64_t*>(a[0]))->notify_one(); return 0; });
  reg("_Cnd_wait", [](const uint64_t* a, unsigned) -> uint64_t {
    auto* cv = reinterpret_cast<std::condition_variable_any*>(*reinterpret_cast<uint64_t*>(a[0]));
    auto* mx = reinterpret_cast<std::recursive_mutex*>(*reinterpret_cast<uint64_t*>(a[1]));
    std::unique_lock<std::recursive_mutex> lk(*mx);
    cv->wait(lk); return 0; });
  reg("_Cnd_timedwait", [](const uint64_t* a, unsigned) -> uint64_t {
    auto* cv = reinterpret_cast<std::condition_variable_any*>(*reinterpret_cast<uint64_t*>(a[0]));
    auto* mx = reinterpret_cast<std::recursive_mutex*>(*reinterpret_cast<uint64_t*>(a[1]));
    std::unique_lock<std::recursive_mutex> lk(*mx);
    cv->wait_for(lk, std::chrono::milliseconds(10)); return 0; });
  reg("_Thrd_current", [](const uint64_t*, unsigned) -> uint64_t {
    return (uint64_t)pthread_self(); });
  reg("_Thrd_equal", [](const uint64_t* a, unsigned) -> uint64_t { return a[0]==a[1]?1:0; });
  reg("_Thrd_join", [](const uint64_t*, unsigned) -> uint64_t { return 0; });

  // ---------------- 时间（tm 前9个int为标准布局）----------------
  auto read_tm = [](uint64_t g, std::tm& t) {
    auto* gi = reinterpret_cast<const int*>(g);
    t.tm_sec=gi[0]; t.tm_min=gi[1]; t.tm_hour=gi[2]; t.tm_mday=gi[3];
    t.tm_mon=gi[4]; t.tm_year=gi[5]; t.tm_wday=gi[6]; t.tm_yday=gi[7]; t.tm_isdst=gi[8];
  };
  auto write_tm = [](uint64_t g, const std::tm& t) {
    auto* gi = reinterpret_cast<int*>(g);
    gi[0]=t.tm_sec; gi[1]=t.tm_min; gi[2]=t.tm_hour; gi[3]=t.tm_mday;
    gi[4]=t.tm_mon; gi[5]=t.tm_year; gi[6]=t.tm_wday; gi[7]=t.tm_yday; gi[8]=t.tm_isdst;
  };
  reg("mktime", [read_tm](const uint64_t* a, unsigned) -> uint64_t {
    std::tm t{}; read_tm(a[0], t); return (uint64_t)(int64_t)mktime(&t); });
  reg("localtime", [write_tm](const uint64_t* a, unsigned) -> uint64_t {
    static thread_local std::tm out; static thread_local int buf[9];
    time_t tt = (time_t)(int64_t)*reinterpret_cast<const uint64_t*>(a[0]);
    localtime_r(&tt, &out); write_tm((uint64_t)buf, out);
    return (uint64_t)buf; });
  reg("localtime_s", [read_tm,write_tm](const uint64_t* a, unsigned) -> uint64_t {
    std::tm t{}; time_t tt=(time_t)(int64_t)*reinterpret_cast<const uint64_t*>(a[1]);
    localtime_r(&tt,&t); write_tm(a[0],t); return 0; });
  reg("gmtime_s", [write_tm](const uint64_t* a, unsigned) -> uint64_t {
    std::tm t{}; time_t tt=(time_t)(int64_t)*reinterpret_cast<const uint64_t*>(a[1]);
    gmtime_r(&tt,&t); write_tm(a[0],t); return 0; });
  reg("strftime", [read_tm](const uint64_t* a, unsigned) -> uint64_t {
    std::tm t{}; read_tm(a[3], t);
    return strftime(reinterpret_cast<char*>(a[0]), (size_t)a[1],
                    reinterpret_cast<const char*>(a[2]), &t); });

  // ---------------- sscanf（转发寄存器/栈变参，输出指针为整数参）----------------
  reg("sscanf", [](const uint64_t* a, unsigned) -> uint64_t {
    return (uint64_t)(int64_t)sscanf(reinterpret_cast<const char*>(a[0]),
        reinterpret_cast<const char*>(a[1]),
        a[2],a[3],a[4],a[5],a[6],a[7],a[8],a[9],a[10],a[11],a[12],a[13]); });
  reg("fgetpos", [](const uint64_t* a, unsigned) -> uint64_t {
    fpos_t p; if (fgetpos(reinterpret_cast<FILE*>(a[0]), &p)!=0) return 1;
    *reinterpret_cast<uint64_t*>(a[1]) = (uint64_t)p; return 0; });
  reg("fsetpos", [](const uint64_t* a, unsigned) -> uint64_t {
    fpos_t p = (fpos_t)*reinterpret_cast<const uint64_t*>(a[1]);
    return fsetpos(reinterpret_cast<FILE*>(a[0]), &p)==0 ? 0 : 1; });

  return n;
}

}  // namespace divaft