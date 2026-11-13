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

struct Case { const char* name; std::vector<uint8_t> code; uint64_t want; };

}  // namespace

static bool RunSelfTests() {
  setvbuf(stdout, nullptr, _IONBF, 0);
  setvbuf(stderr, nullptr, _IONBF, 0);
  std::fprintf(stderr, "[divaft-guest] main started\n");
  // 不用 MAP_FIXED 低地址：8MB@0x10000 会覆盖静态二进制自身装载段导致 SIGILL。
  // 让 OS 选安全地址，4MB 区域（代码+栈+1MB veneer）。
  constexpr uint64_t kRegion = 4 * 1024 * 1024;
  void* region = mmap(nullptr, kRegion, PROT_READ | PROT_WRITE | PROT_EXEC,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (region == MAP_FAILED) { std::perror("mmap"); return false; }
  const uint64_t base = reinterpret_cast<uint64_t>(region);
  std::fprintf(stderr, "[divaft-guest] mmap ok base=0x%llx, calling Init\n",
               (unsigned long long)base);

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
        [](const uint64_t* args, unsigned, const double*) -> uint64_t {
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
        [&mem](const uint64_t* a, unsigned, const double*) -> uint64_t {
          uint64_t phys = 0;
          int r = mem.AllocateDirect(a[2], a[3], &phys);
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
    EmitCallRel32(c, code_addr, alloc_veneer);
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

  bridge.Shutdown();
  std::printf("[divaft-guest] %s (%d/%d)\n",
              passed == total ? "GUEST_EXEC_PASS" : "GUEST_EXEC_FAIL", passed, total);
  return passed == total;
}


int main(int argc, char** argv) {
  bool self_ok = RunSelfTests();
  if (!self_ok) return 1;
  if (argc >= 2) {
    std::fprintf(stderr, "[diva-boot] loading %s\n", argv[1]);
    divaft::DivaRuntime rt;
    const char* contract = argc >= 3 ? argv[2] : "";
    if (!rt.Init(argv[1], contract)) {
      std::fprintf(stderr, "[diva-boot] init failed\n");
      return 2;
    }
    rt.Boot();
    std::fprintf(stderr, "[diva-boot] boot returned\n");
  }
  return 0;
}
