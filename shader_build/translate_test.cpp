// divaft_Android — shaders→Vulkan 端到端验证：GCN shader → IR → SPIR-V。
// 用例1：最简 GCN compute shader（S_ENDPGM）走完整 TranslateProgram + EmitSPIRV。
// 用例2：S_LOAD_DWORD s20, s[0:1], 0（base=GetUserData→SRT 根）锻炼 ARM64 解释执行 walker。
// 对照 vk_pipeline_cache.CompileModule。
#include "shader_recompiler/recompiler.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/backend/bindings.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/runtime_info.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/params.h"
#include "common/logging/log.h"
#include <cstdio>
#include <vector>
#include <array>
#include <span>

static bool TranslateOne(const char* name, const uint32_t* code, size_t count) {
  std::array<uint32_t, 16> user_data{};
  std::span<const uint32_t> code_span(code, count);
  Shader::ShaderParams params{
      .user_data = std::span<const uint32_t, 16>(user_data),
      .code = code_span,
      .hash = 0x1234,
  };
  Shader::Info info(Shader::HwStage::Compute, Shader::SwStage::Compute, params);
  Shader::RuntimeInfo runtime_info{};
  runtime_info.Initialize(Shader::HwStage::Compute, Shader::SwStage::Compute);

  Shader::Profile profile{};
  profile.supported_spirv = 0x00010000;  // Vulkan 1.0
  profile.subgroup_size = 64;
  profile.max_viewport_width = 16384;
  profile.max_viewport_height = 16384;
  profile.max_shared_memory_size = 32768;
  profile.support_int16 = true;
  profile.support_int64 = true;

  Shader::Pools pools;
  Shader::Backend::Bindings binding{};

  std::fprintf(stderr, "[ckpt] %s: before TranslateProgram\n", name); std::fflush(stderr);
  const auto ir = Shader::TranslateProgram(code_span, pools, info, runtime_info, profile);
  std::fprintf(stderr, "[ckpt] %s: before EmitSPIRV\n", name); std::fflush(stderr);
  auto spv = Shader::Backend::SPIRV::EmitSPIRV(profile, runtime_info, ir, binding);
  std::fprintf(stderr, "[ckpt] %s: after EmitSPIRV\n", name); std::fflush(stderr);

  bool ok = spv.size() >= 5 && spv[0] == 0x07230203u;  // SPIR-V magic
  std::fprintf(stderr, "%s: SPIR-V words=%zu magic=0x%08x version=0x%08x -> %s\n", name,
               spv.size(), spv[0], spv[1], ok ? "OK" : "FAIL");
  std::fflush(stderr);
  return ok;
}

int main() {
  Common::Log::Setup("shader_translate_test");
  std::fprintf(stderr, "[ckpt] logging ready\n"); std::fflush(stderr);

  // 用例1：最简 compute shader（S_ENDPGM）。
  const uint32_t minimal[] = {0xBF810000u};
  bool ok1 = TranslateOne("minimal(S_ENDPGM)", minimal, 1);

  // 用例2：S_LOAD_DWORD s20, s[0:1], offset0(imm) + S_ENDPGM。
  // SMRD: 0xC0000000 | (sdst=20)<<15 | (sbase=0)<<9 | (imm=1)<<8 | offset0 = 0xC0100100。
  // base=GetUserData(s0,s1) → SRT 根 → 触发 ARM64 解释执行 walker。
  const uint32_t srt[] = {0xC0100100u, 0xBF810000u};
  bool ok2 = TranslateOne("srt(S_LOAD_DWORD)", srt, 2);

  if (ok1 && ok2) {
    std::printf("SHADER_TRANSLATE_TEST_PASS (GCN->SPIR-V OK, incl SRT walker)\n");
    return 0;
  }
  std::printf("SHADER_TRANSLATE_TEST_FAIL\n");
  return 1;
}