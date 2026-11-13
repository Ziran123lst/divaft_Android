// divaft_Android — guest 执行验证测试（原创）
// 用 GuestBridge 在 FEX 下执行 x86-64 代码，验证桥接 + 翻译正确。
// 用例：整数加法、标量浮点乘法（mulsd）、打包加法（addpd）。
// SPDX-License-Identifier: GPL-2.0-or-later
#include "divaft/guest_bridge.h"
#include "divaft/diva_runtime.h"
#include "divaft/guest_memory.h"

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <sys/mman.h>
#include <thread>

namespace {

void Emit(std::vector<uint8_t>& c, std::initializer_list<uint8_t> bytes) {
  c.insert(c.end(), bytes);
}
void MovRaxImm64(std::vector<uint8_t>& c, uint64_t v) {
  Emit(c, {0x48, 0xB8});
  for (int i = 0; i < 8; ++i) c.push_back((v >> (i * 8)) & 0xFF);
}
// mov r64, imm64（reg: 0=rax 1=rcx 2=rdx 3=rbx 4=rsp 5=rbp 6=rsi 7=rdi 8-15=r8-r15）
void MovRegImm64(std::vector<uint8_t>& c, int reg, uint64_t v) {
  if (reg < 8) Emit(c, {0x48, static_cast<uint8_t>(0xB8 + reg)});
  else Emit(c, {0x49, static_cast<uint8_t>(0xB8 + (reg - 8))});
  for (int i = 0; i < 8; ++i) c.push_back((v >> (i * 8)) & 0xFF);
}
uint64_t Bits(double d) { uint64_t u; std::memcpy(&u, &d, 8); return u; }

// 整数：rax = 100 + 23 = 123
std::vector<uint8_t> BuildInt() {
  std::vector<uint8_t> c;
  MovRaxImm64(c, 100);
  Emit(c, {0x48, 0x05, 0x17, 0x00, 0x00, 0x00});  // add rax, 23
  Emit(c, {0xF4});                                 // hlt
  return c;
}

// 浮点：xmm0 = 2.5 * 4.0 = 10.0，结果搬回 rax
std::vector<uint8_t> BuildMulsd() {
  std::vector<uint8_t> c;
  MovRaxImm64(c, Bits(2.5));
  Emit(c, {0x66, 0x48, 0x0F, 0x6E, 0xC0});  // movq xmm0, rax
  MovRaxImm64(c, Bits(4.0));
  Emit(c, {0x66, 0x48, 0x0F, 0x6E, 0xC8});  // movq xmm1, rax
  Emit(c, {0xF2, 0x0F, 0x59, 0xC1});        // mulsd xmm0, xmm1
  Emit(c, {0x66, 0x48, 0x0F, 0x7E, 0xC0});  // movq rax, xmm0
  Emit(c, {0xF4});
  return c;
}

// 追加 call rel32 到 target（code 将放在 code_addr）。返回后 rel32 已算好。
void EmitCallRel32(std::vector<uint8_t>& c, uint64_t code_addr, uint64_t target) {
  c.push_back(0xE8);
  size_t rel_off = c.size();
  c.insert(c.end(), {0, 0, 0, 0});
  int32_t rel = static_cast<int32_t>(target - (code_addr + rel_off + 4));
  std::memcpy(c.data() + rel_off, &rel, 4);
}

// 宿主调用：rdi=7, rsi=5, 直接 call veneer（host 算 7+5=12），结果在 rax
std::vector<uint8_t> BuildHostCall(uint64_t veneer_addr, uint64_t code_addr) {
  std::vector<uint8_t> c;
  MovRaxImm64(c, 7);
  Emit(c, {0x48, 0x89, 0xC7});              // mov rdi, rax
  MovRaxImm64(c, 5);
  Emit(c, {0x48, 0x89, 0xC6});              // mov rsi, rax
  EmitCallRel32(c, code_addr, veneer_addr); // call veneer（直接调用）
  Emit(c, {0xF4});                          // hlt
  return c;
}

// 自包含直接调用：call sub; hlt; sub: mov rax,42; ret（验证 call rel32）
std::vector<uint8_t> BuildDirectCallRet(uint64_t code_addr) {
  std::vector<uint8_t> c;
  size_t call_off = c.size();
  c.push_back(0xE8);
  size_t rel_off = c.size();
  c.insert(c.end(), {0, 0, 0, 0});
  c.push_back(0xF4);                        // hlt
  size_t sub = c.size();
  MovRaxImm64(c, 42);
  c.push_back(0xC3);                        // ret
  int32_t rel = static_cast<int32_t>((code_addr + sub) - (code_addr + rel_off + 4));
  std::memcpy(c.data() + rel_off, &rel, 4);
  (void)call_off;
  return c;
}

// 会返回的 guest 函数：mov rax, value; ret（不含 HLT）。
// 验证返回蹦床：ret 落到 [rsp] 里的 exit_hlt，CallGuest 取回 RAX。线程例程即此形态。
std::vector<uint8_t> BuildRetFunc(uint64_t value) {
  std::vector<uint8_t> c;
  MovRaxImm64(c, value);
  c.push_back(0xC3);  // ret
  return c;
}

struct Case { const char* name; std::vector<uint8_t> code; uint64_t want; };

}  // namespace

static bool RunSelfTests() {
  setvbuf(stdout, nullptr, _IONBF, 0);
  setvbuf(stderr, nullptr, _IONBF, 0);
  std::fprintf(stderr, "[divaft-guest] main started\n");
  // 4MB 区域（代码+栈+1MB veneer）。优先低 4G 映射（规避 FEX-qemu 对 r8-r15 高地址指针截断）；
  // 低地址避开静态二进制装载段（约 0x200000-0x59a000），用 MAP_FIXED_NOREPLACE 安全尝试、失败回退。
  constexpr uint64_t kRegion = 4 * 1024 * 1024;
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif
  void* region = mmap(reinterpret_cast<void*>(0x1000000ULL), kRegion,
                      PROT_READ | PROT_WRITE | PROT_EXEC,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
  if (region == MAP_FAILED) {
    region = mmap(nullptr, kRegion, PROT_READ | PROT_WRITE | PROT_EXEC,
                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  }
  if (region == MAP_FAILED) { std::perror("mmap"); return false; }
  const uint64_t base = reinterpret_cast<uint64_t>(region);
  std::fprintf(stderr, "[divaft-guest] mmap ok base=0x%llx (%s), calling Init\n",
               (unsigned long long)base, base < 0x100000000ULL ? "低4G" : "高地址");

  divaft::GuestBridge bridge;
  if (!bridge.Init(base, kRegion)) {
    std::fprintf(stderr, "GuestBridge Init failed\n");
    return false;
  }
  std::fprintf(stderr, "[divaft-guest] Init ok\n");

  Case cases[] = {
      {"int_add", BuildInt(), 123},
      {"fp_mulsd", BuildMulsd(), Bits(10.0)},
  };

  int passed = 0, total = 0;
  for (auto& c : cases) {
    ++total;
    uint64_t code_addr = base + 0x1000 * total;
    std::memcpy(reinterpret_cast<void*>(code_addr), c.code.data(), c.code.size());
    uint64_t got = bridge.CallGuest(code_addr, nullptr, 0);
    bool ok = got == c.want;
    if (ok) ++passed;
    std::printf("[divaft-guest] %-10s : %s (got=0x%llx want=0x%llx)\n",
                c.name, ok ? "PASS" : "FAIL",
                (unsigned long long)got, (unsigned long long)c.want);
  }

  // 隔离诊断：自包含直接调用 call rel32（sub 返回 42），验证直接调用机制
  {
    ++total;
    uint64_t code_addr = base + 0x1000 * total;
    auto c = BuildDirectCallRet(code_addr);
    std::memcpy(reinterpret_cast<void*>(code_addr), c.data(), c.size());
    uint64_t got = bridge.CallGuest(code_addr, nullptr, 0);
    bool ok = got == 42;
    if (ok) ++passed;
    std::printf("[divaft-guest] %-10s : %s (got=0x%llx want=0x2a)\n",
                "direct_call", ok ? "PASS" : "FAIL", (unsigned long long)got);
  }

  // veneer 宿主调用：guest 调 host 函数（7+5），验证 guest↔host 桥接往返
  {
    ++total;
    uint64_t veneer = bridge.ExposeHost("host_add",
        [](const uint64_t* args, unsigned, const double*, divaft::HleReturn*) -> uint64_t {
          return args[0] + args[1];
        });
    uint64_t code_addr = base + 0x1000 * total;
    auto code = BuildHostCall(veneer, code_addr);
    std::memcpy(reinterpret_cast<void*>(code_addr), code.data(), code.size());
    uint64_t got = bridge.CallGuest(code_addr, nullptr, 0);
    bool ok = got == 12;
    if (ok) ++passed;
    std::printf("[divaft-guest] %-10s : %s (got=0x%llx want=0x%llx)\n",
                "host_call", ok ? "PASS" : "FAIL",
                (unsigned long long)got, 12ULL);
  }

  // 内存 HLE：guest 调 sceKernelAllocateDirectMemory，验证分配成功且输出 physAddr
  {
    ++total;
    divaft::GuestMemoryManager mem;
    mem.Init(base + 0x100000, 0x100000);  // 1MB 堆
    uint64_t alloc_veneer = bridge.ExposeHost("sceKernelAllocateDirectMemory",
        [&mem](const uint64_t* a, unsigned, const double*, divaft::HleReturn*) -> uint64_t {
          uint64_t phys = 0;
          int r = mem.AllocateDirect(a[2], a[3], &phys);
          std::fprintf(stderr, "[mem_hle] a0=%llx a1=%llx a2=%llx a3=%llx a4=%llx a5=%llx phys=%llx r=%d\n",
                       (unsigned long long)a[0], (unsigned long long)a[1],
                       (unsigned long long)a[2], (unsigned long long)a[3],
                       (unsigned long long)a[4], (unsigned long long)a[5],
                       (unsigned long long)phys, r);
          if (r == 0 && a[5]) *reinterpret_cast<uint64_t*>(a[5]) = phys;
          return static_cast<uint64_t>(r);
        });
    uint64_t code_addr = base + 0x1000 * total;
    uint64_t phys_out = base + 0x9000;
    *reinterpret_cast<uint64_t*>(phys_out) = 0;
    std::vector<uint8_t> c;
    MovRegImm64(c, 7, 0);             // rdi = searchStart
    MovRegImm64(c, 6, 0x100000);      // rsi = searchEnd
    MovRegImm64(c, 2, 0x4000);        // rdx = len
    MovRegImm64(c, 1, 0x4000);        // rcx = alignment
    MovRegImm64(c, 8, 0);             // r8 = memoryType
    MovRegImm64(c, 9, phys_out);      // r9 = physAddrOut
    EmitCallRel32(c, code_addr, alloc_veneer);  // 直接 call rel32（phase-2 验证过的形态）
    Emit(c, {0xF4});                  // hlt
    std::memcpy(reinterpret_cast<void*>(code_addr), c.data(), c.size());
    uint64_t got = bridge.CallGuest(code_addr, nullptr, 0);
    uint64_t phys = *reinterpret_cast<uint64_t*>(phys_out);
    bool ok = (got == 0) && (phys >= base + 0x100000) && (phys != 0);
    if (ok) ++passed;
    std::printf("[divaft-guest] %-10s : %s (ret=%llu phys=0x%llx)\n",
                "mem_hle", ok ? "PASS" : "FAIL",
                (unsigned long long)got, (unsigned long long)phys);
  }

  // 返回蹦床：guest 函数以 ret 结束（非 HLT），CallGuest 应取回 RAX。线程例程即此形态。
  {
    ++total;
    uint64_t code_addr = base + 0x1000 * total;
    auto c = BuildRetFunc(77);
    std::memcpy(reinterpret_cast<void*>(code_addr), c.data(), c.size());
    uint64_t got = bridge.CallGuest(code_addr, nullptr, 0);
    bool ok = got == 77;
    if (ok) ++passed;
    std::printf("[divaft-guest] %-10s : %s (got=0x%llx want=0x4d)\n",
                "ret_func", ok ? "PASS" : "FAIL", (unsigned long long)got);
  }

  // 并发 guest 线程：两个宿主线程各调 CallGuest 执行会返回的 guest 函数，
  // 验证 per-call 栈隔离（pthread_create 的底层机制）。
  {
    ++total;
    uint64_t ca = base + 0x1000 * total;
    uint64_t cb = base + 0x1000 * (total + 1);
    auto fa = BuildRetFunc(111);
    auto fb = BuildRetFunc(222);
    std::memcpy(reinterpret_cast<void*>(ca), fa.data(), fa.size());
    std::memcpy(reinterpret_cast<void*>(cb), fb.data(), fb.size());
    uint64_t ra = 0, rb = 0;
    std::thread ta([&]() { ra = bridge.CallGuest(ca, nullptr, 0); });
    std::thread tb([&]() { rb = bridge.CallGuest(cb, nullptr, 0); });
    ta.join(); tb.join();
    bool ok = (ra == 111) && (rb == 222);
    if (ok) ++passed;
    std::printf("[divaft-guest] %-10s : %s (a=%llu b=%llu)\n",
                "threads", ok ? "PASS" : "FAIL",
                (unsigned long long)ra, (unsigned long long)rb);
  }

  // FP 编组：guest 把 2.0 放 XMM0 调 veneer，宿主算 2.0*3.0=6.0 经 HleReturn 写回 XMM0，
  // guest 搬回 RAX。验证 XMM 输入(fp_args)与输出(HleReturn.xmm0)双向编组。
  {
    ++total;
    uint64_t fp_veneer = bridge.ExposeHost("fp_mul3",
        [](const uint64_t*, unsigned, const double* fp, divaft::HleReturn* ret) -> uint64_t {
          double r = fp[0] * 3.0;
          uint64_t bits; std::memcpy(&bits, &r, 8);
          ret->xmm0 = bits; ret->fp = true;
          return 0;
        });
    uint64_t code_addr = base + 0x10000;  // 独立页，避免与前面用例/phys_out 冲突
    std::vector<uint8_t> c;
    MovRaxImm64(c, Bits(2.0));
    Emit(c, {0x66, 0x48, 0x0F, 0x6E, 0xC0});  // movq xmm0, rax
    MovRaxImm64(c, fp_veneer);
    Emit(c, {0xFF, 0xD0});                     // call rax
    Emit(c, {0x66, 0x48, 0x0F, 0x7E, 0xC0});  // movq rax, xmm0
    Emit(c, {0xF4});                           // hlt
    std::memcpy(reinterpret_cast<void*>(code_addr), c.data(), c.size());
    uint64_t got = bridge.CallGuest(code_addr, nullptr, 0);
    bool ok = got == Bits(6.0);
    if (ok) ++passed;
    std::printf("[divaft-guest] %-10s : %s (got=0x%llx want=0x%llx)\n",
                "fp_hle", ok ? "PASS" : "FAIL",
                (unsigned long long)got, (unsigned long long)Bits(6.0));
  }

  bridge.Shutdown();
  std::printf("[divaft-guest] %s (%d/%d)\n",
              passed == total ? "GUEST_EXEC_PASS" : "GUEST_EXEC_FAIL", passed, total);
  return passed == total;
}


int main(int argc, char** argv) {
  setvbuf(stderr, nullptr, _IONBF, 0);  // 崩溃前日志不丢
  bool self_ok = RunSelfTests();
  if (argc >= 2) {
    if (!self_ok)
      std::fprintf(stderr, "[diva-boot] 自检未全过（诊断用），继续启动 eboot\n");
    std::fprintf(stderr, "[diva-boot] loading %s\n", argv[1]);
    divaft::DivaRuntime rt;
    const char* contract = argc >= 3 ? argv[2] : "";
    const char* libc_path = argc >= 4 ? argv[3] : "";
    if (!rt.Init(argv[1], contract, libc_path)) {
      std::fprintf(stderr, "[diva-boot] init failed\n");
      return 2;
    }
    rt.Boot();
    std::fprintf(stderr, "[diva-boot] boot returned\n");
    return 0;
  }
  return self_ok ? 0 : 1;
}
