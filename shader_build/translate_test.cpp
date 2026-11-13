// divaft_Android — shaders→Vulkan 端到端验证：GCN shader → IR → SPIR-V。
// 用最简 GCN compute shader（S_ENDPGM）走完整 TranslateProgram + EmitSPIRV 管线，
// 证明 shader 重编译引擎能真正产出 Vulkan SPIR-V（对照 vk_pipeline_cache.CompileModule）。
#include "shader_recompiler/recompiler.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/backend/bindings.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/runtime_info.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/params.h"
#include <cstdio>
#include <vector>
#include <array>
#include <span>

int main() {
  // 最简 GCN compute shader：S_ENDPGM (0xBF810000)。
  std::array<uint32_t, 1> code = {0xBF810000u};
  std::array<uint32_t, 16> user_data{};

  Shader::ShaderParams params{
      .user_data = std::span<const uint32_t, 16>(user_data),
      .code = std::span<const uint32_t>(code),
      .hash = 0x1234,
  };
  Shader::Info info(Shader::HwStage::Compute, Shader::SwStage::Compute, params);
  Shader::RuntimeInfo runtime_info;
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

  const auto ir = Shader::TranslateProgram(std::span<const uint32_t>(code), pools, info,
                                           runtime_info, profile);
  auto spv = Shader::Backend::SPIRV::EmitSPIRV(profile, runtime_info, ir, binding);

  std::printf("SPIR-V words=%zu\n", spv.size());
  bool ok = spv.size() >= 5 && spv[0] == 0x07230203u;  // SPIR-V magic
  if (ok) {
    std::printf("SPIR-V magic=0x%08x version=0x%08x\n", spv[0], spv[1]);
    std::printf("SHADER_TRANSLATE_TEST_PASS (GCN->SPIR-V OK)\n");
    return 0;
  }
  std::printf("SHADER_TRANSLATE_TEST_FAIL\n");
  return 1;
}