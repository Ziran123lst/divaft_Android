// divaft_Android — shader_recompiler 独立构建的精简 assert 实现。
// 原版 common/assert.cpp 依赖 Core::Emulator/Signals（拉进 SDL3），shader 库不需要。
// 此处仅提供 assert_fail_impl/unreachable_impl/assert_fail_debug_msg，失败即 crash。
#include "common/assert.h"
#include "common/logging/log.h"
#include <stdexcept>
#include <cstdlib>

#if defined(__aarch64__) || defined(_M_ARM64)
#define SHADER_CRASH() __asm__ __volatile__("brk 0")
#elif defined(__x86_64__) || defined(_M_X64)
#define SHADER_CRASH() __asm__ __volatile__("int $3")
#else
#define SHADER_CRASH() std::abort()
#endif

void assert_fail_impl() {
  SHADER_CRASH();
  std::abort();
}

[[noreturn]] void unreachable_impl() {
  assert_fail_impl();
  throw std::runtime_error("Unreachable code");
}

void assert_fail_debug_msg(const char* msg) {
  LOG_CRITICAL(Debug, "Assertion Failed!\n{}", msg);
  assert_fail_impl();
}