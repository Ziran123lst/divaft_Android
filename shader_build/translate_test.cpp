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

static bool TranslateOne(const char* name, const uint32_t* code, size_t count,
                         const char* spv_out = nullptr,
                         Shader::HwStage hw = Shader::HwStage::Compute,
                         Shader::SwStage sw = Shader::SwStage::Compute) {
  std::array<uint32_t, 16> user_data{};
  std::span<const uint32_t> code_span(code, count);
  Shader::ShaderParams params{
      .user_data = std::span<const uint32_t, 16>(user_data),
      .code = code_span,
      .hash = 0x1234,
  };
  Shader::Info info(hw, sw, params);
  Shader::RuntimeInfo runtime_info{};
  runtime_info.Initialize(hw, sw);
  runtime_info.props.num_user_data = 16;  // EmitPrologue 据此把 s0-s15 初始化为 GetUserData（SRT 根来源）

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
  if (ok && spv_out) {
    if (FILE* f = std::fopen(spv_out, "wb")) {
      std::fwrite(spv.data(), sizeof(uint32_t), spv.size(), f);
      std::fclose(f);
      std::fprintf(stderr, "%s: SPIR-V dumped to %s\n", name, spv_out);
      std::fflush(stderr);
    }
  }
  return ok;
}

int main() {
  Common::Log::Setup("shader_translate_test");
  std::fprintf(stderr, "[ckpt] logging ready\n"); std::fflush(stderr);

  // 用例1：最简 compute shader（S_ENDPGM）。
  const uint32_t minimal[] = {0xBF810000u};
  bool ok1 = TranslateOne("minimal(S_ENDPGM)", minimal, 1, "/tmp/diva_minimal.spv");

  // 用例2：S_LOAD_DWORD s20, s[0:1], offset0(imm) + S_ENDPGM。
  // SMRD: 0xC0000000 | (sdst=20)<<15 | (sbase=0)<<9 | (imm=1)<<8 | offset0 = 0xC0100100。
  // base=GetUserData(s0,s1) → SRT 根 → 触发 ARM64 解释执行 walker。
  const uint32_t srt[] = {0xC0100100u, 0xBF810000u};
  bool ok2 = TranslateOne("srt(S_LOAD_DWORD)", srt, 2, "/tmp/diva_srt.spv");

  // 用例3：最简顶点 shader——v_mov_b32 设 v0..v3=(0,0,0,1)，EXP pos0 导出位置。
  // VOP1 v_mov_b32: 0x7E000000|(vdst<<17)|(op=1<<9)|src0=255(literal)，后随 literal dword。
  // EXP(64bit): low=0xF8000000|(vm<<12)|(done<<11)|(target<<4)|en；high=vsrc0|vsrc1<<8|vsrc2<<16|vsrc3<<24。
  const uint32_t vert[] = {
      0xBEEB03FFu,                          // s_mov_b32 vcc_hi, imm（prologue）
      0x7E0002FFu, 0x00000000u,             // v_mov_b32 v0, 0.0
      0x7E0202FFu, 0x00000000u,             // v_mov_b32 v1, 0.0
      0x7E0402FFu, 0x00000000u,             // v_mov_b32 v2, 0.0
      0x7E0602FFu, 0x3F800000u,             // v_mov_b32 v3, 1.0
      0xF80018CFu, 0x03020100u,             // exp pos0(target=12), en=0xF, vm=1, done=1, v0..v3
      0xBF810000u,                          // s_endpgm
  };
  bool ok3 = TranslateOne("vertex(EXP pos0)", vert, sizeof(vert) / 4, "/tmp/diva_vert.spv",
                          Shader::HwStage::Vertex, Shader::SwStage::Vertex);

  // 用例4：最简像素 shader——v0..v3=(1,0,0,1)，EXP mrt0 导出颜色。
  const uint32_t frag[] = {
      0xBEEB03FFu,
      0x7E0002FFu, 0x3F800000u,             // v0 = 1.0 (r)
      0x7E0202FFu, 0x00000000u,             // v1 = 0.0 (g)
      0x7E0402FFu, 0x00000000u,             // v2 = 0.0 (b)
      0x7E0602FFu, 0x3F800000u,             // v3 = 1.0 (a)
      0xF800080Fu, 0x03020100u,             // exp mrt0(target=0), en=0xF, done=1, v0..v3
      0xBF810000u,
  };
  bool ok4 = TranslateOne("fragment(EXP mrt0)", frag, sizeof(frag) / 4, "/tmp/diva_frag.spv",
                          Shader::HwStage::Fragment, Shader::SwStage::Fragment);

  if (ok1 && ok2 && ok3 && ok4) {
    std::printf("SHADER_TRANSLATE_TEST_PASS (GCN->SPIR-V OK: compute+SRT+vertex+fragment)\n");
    return 0;
  }
  std::printf("SHADER_TRANSLATE_TEST_FAIL\n");
  return 1;
}