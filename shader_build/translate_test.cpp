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
  if (hw == Shader::HwStage::Fragment) {
    // ExportRenderTarget 在 export_format==Zero 时丢弃导出；须设有效 MRT0 格式。
    runtime_info.hw.fs.color_buffers[0].export_format = AmdGpu::ShaderExportFormat::ABGR_32;
  }

  Shader::Profile profile{};
  profile.supported_spirv = 0x00010600;  // SPIR-V 1.6（对齐 shadPS4 SpirvVersion1_6）；
                                         // 1.0 太低：GroupNonUniformBallot(1.3+)/StorageBuffer8BitAccess(1.5+) 会非法
  profile.subgroup_size = 64;
  profile.max_viewport_width = 16384;
  profile.max_viewport_height = 16384;
  profile.max_shared_memory_size = 32768;
  profile.support_int8 = true;
  profile.support_int16 = true;
  profile.support_int64 = true;
  profile.support_float16 = true;
  profile.support_float64 = true;
  profile.supports_denorm_behavior_independence = true;
  profile.supports_rounding_mode_independence = true;

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

  // 用例3：逐顶点顶点 shader——v0=顶点索引(EmitPrologue自动设)，抛物线算位置：
  // x=float(v0)-1 (-1,0,1)，y=2x²-1 (1,-1,1)，pos=(0.5x,0.5y,0,1)→三角形(-0.5,0.5)(0,-0.5)(0.5,0.5)。
  // VOP2=(op<<25)|(vdst<<17)|(vsrc1<<9)|src0；VOP1=0x7E000000|(vdst<<17)|(op<<9)|src0。
  // 内联常量:0.5=240,1.0=242,-1.0=243,2.0=244,0=128；VGPR vN src0=256+N。
  const uint32_t vert[] = {
      0xBEEB03FFu, 0x00000000u, // s_mov_b32 vcc_hi, 0（prologue token 需紧跟 literal dword！）
      0x7E020D00u,              // v_cvt_f32_u32 v1, v0      ; v1=float(vertex_index)
      0x060402F3u,              // v_add_f32 v2, -1.0, v1    ; x=v1-1
      0x10060502u,              // v_mul_f32 v3, v2, v2      ; x*x
      0x100606F4u,              // v_mul_f32 v3, 2.0, v3     ; 2*x*x
      0x060606F3u,              // v_add_f32 v3, -1.0, v3    ; y=2*x*x-1
      0x100804F0u,              // v_mul_f32 v4, 0.5, v2     ; pos.x=0.5x
      0x100A06F0u,              // v_mul_f32 v5, 0.5, v3     ; pos.y=0.5y
      0x7E0C0280u,              // v_mov_b32 v6, 0.0         ; pos.z=0
      0x7E0E02F2u,              // v_mov_b32 v7, 1.0         ; pos.w=1
      0xF80018CFu, 0x07060504u, // exp pos0, v4,v5,v6,v7 (vm=1,done=1,target=12,en=0xF)
      0xBF810000u,              // s_endpgm
  };
  bool ok3 = TranslateOne("vertex(per-vertex parabola)", vert, sizeof(vert) / 4, "/tmp/diva_vert.spv",
                          Shader::HwStage::Vertex, Shader::SwStage::Vertex);

  // 用例4：最简像素 shader——v0..v3=(1,0,0,1)，EXP mrt0 导出颜色。
  const uint32_t frag[] = {
      0xBEEB03FFu, 0x00000000u,             // s_mov_b32 vcc_hi, 0（prologue 需紧跟 literal）
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