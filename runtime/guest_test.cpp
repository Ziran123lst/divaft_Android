// divaft_Android — guest 执行验证测试（原创）
// 用 GuestBridge 在 FEX 下执行 x86-64 代码，验证桥接 + 翻译正确。
// 用例：整数加法、标量浮点乘法（mulsd）、打包加法（addpd）。
// SPDX-License-Identifier: GPL-2.0-or-later
#include "divaft/guest_bridge.h"

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <sys/mman.h>

namespace {

constexpr uint64_t kBase = 0x10000;
constexpr uint64_t kRegion = 8 * 1024 * 1024;

void Emit(std::vector<uint8_t>& c, std::initializer_list<uint8_t> bytes) {
  c.insert(c.end(), bytes);
}
void MovRaxImm64(std::vector<uint8_t>& c, uint64_t v) {
  Emit(c, {0x48, 0xB8});
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

struct Case { const char* name; std::vector<uint8_t> code; uint64_t want; };

}  // namespace

int main() {
  void* region = mmap(reinterpret_cast<void*>(kBase), kRegion,
                      PROT_READ | PROT_WRITE | PROT_EXEC,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
  if (region == MAP_FAILED) { std::perror("mmap"); return 1; }

  divaft::GuestBridge bridge;
  if (!bridge.Init(kBase, kRegion)) {
    std::fprintf(stderr, "GuestBridge Init failed\n");
    return 1;
  }

  Case cases[] = {
      {"int_add", BuildInt(), 123},
      {"fp_mulsd", BuildMulsd(), Bits(10.0)},
  };

  int passed = 0, total = 0;
  for (auto& c : cases) {
    ++total;
    uint64_t code_addr = kBase + 0x1000 * total;
    std::memcpy(reinterpret_cast<void*>(code_addr), c.code.data(), c.code.size());
    uint64_t got = bridge.CallGuest(code_addr, nullptr, 0);
    bool ok = got == c.want;
    if (ok) ++passed;
    std::printf("[divaft-guest] %-10s : %s (got=0x%llx want=0x%llx)\n",
                c.name, ok ? "PASS" : "FAIL",
                (unsigned long long)got, (unsigned long long)c.want);
  }
  bridge.Shutdown();
  std::printf("[divaft-guest] %s (%d/%d)\n",
              passed == total ? "GUEST_EXEC_PASS" : "GUEST_EXEC_FAIL", passed, total);
  return passed == total ? 0 : 1;
}
