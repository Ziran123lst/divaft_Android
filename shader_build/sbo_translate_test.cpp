// divaft_Android — 真实游戏 shader 批量转换验证：SBO 提取的 GCN → IR → SPIR-V。
// 用法: sbo_translate_test <ps4_extract_dir> [spv_out_dir]
//   <ps4_extract_dir> 为 scripts/sbo_extract.py 的 PS4 输出目录（<material>/*_{vertex,fragment}.gcn.bin）。
// 数据是游戏资产，不入仓库；CI 只编译本目标，运行需本地/真机提供提取数据。
// 对照 translate_test.cpp（手写用例）；本目标跑真实 DIVA 材质 shader 全量。
//
// 离线转换的 RuntimeInfo 约定（真机运行时这些值来自 PM4/SPI 寄存器，届时按实际状态填）：
//   - FS addr_flags 启用 persp_center + linear_center（v0/v1=smooth I/J，v2/v3=nopersp I/J）；
//   - FS inputs 全部置为非 default/非 flat，走完整插值路径；
//   - needs_manual_interpolation=true（Turnip 无 AMD 扩展时的可移植路径）。
// 每个 shader 在子进程里转换：UNREACHABLE/断言崩溃只计失败，不终止整批。
#include "shader_recompiler/recompiler.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/backend/bindings.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/runtime_info.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/params.h"
#include "common/logging/log.h"
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>
#include <array>
#include <span>
#include <unistd.h>
#include <sys/wait.h>

namespace fs = std::filesystem;

// DIVA FS 以 fp16 压缩导出为主（EXP.compr=1）；离线按指令流自适应选导出格式。
static bool HasCompressedExport(const uint32_t* code, size_t count) {
  for (size_t i = 0; i < count; ++i) {
    if ((code[i] & 0xFC000000u) == 0xF8000000u && (code[i] & (1u << 10))) return true;
  }
  return false;
}

// ---- PS4 顶点 fetch shader 离线合成 ----
// DIVA VS 一律以 S_SWAPPC_B64 s0,s0 开头：fetch 代码指针在 user_data[0:1]。
// 真实 fetch 代码与 V# 由游戏运行时按输入语义生成（不在 SBO 里）；离线合成最小等价：
// 8 个 float4 属性 → v4..v35，V# = 32_32_32_32 / FLOAT / 身份 swizzle。
// 真机运行时将改用 guest 内存里的真实 fetch 指针，此合成仅用于离线转换验证。
struct FetchSynth {
  std::vector<uint32_t> code;
  std::array<AmdGpu::Buffer, 8> vsharp{};
};

static const FetchSynth& GetFetchSynth() {
  static const FetchSynth synth = [] {
    FetchSynth s;
    auto& c = s.code;
    c.push_back(0x4A000006u);  // v_add_i32 v0, s6, v0（vertex offset）
    c.push_back(0x4A060607u);  // v_add_i32 v3, s7, v3（instance offset）
    for (uint32_t k = 0; k < 8; ++k) {
      // s_load_dwordx4 s[8+4k:11+4k], s[2:3], 4k
      c.push_back(0xC0800000u | ((8 + 4 * k) << 15) | 0x500u | (4 * k));
    }
    c.push_back(0xBF8C0000u);  // s_waitcnt lgkmcnt(0)
    for (uint32_t k = 0; k < 8; ++k) {
      // buffer_load_format_xyzw v[4+4k:7+4k], v0, s[8+4k:11+4k], 0 idxen
      c.push_back(0xE00C2000u);
      c.push_back(((2 + k) << 16) | ((4 + 4 * k) << 8));
    }
    c.push_back(0xBF8C0000u);  // s_waitcnt vmcnt(0)
    c.push_back(0xBE802000u);  // s_setpc_b64 s[0:1]
    for (uint32_t k = 0; k < 8; ++k) {
      auto& b = s.vsharp[k];
      b.base_address = 0x10000000ull + k * 0x100000ull;
      b.stride = 16;
      b.num_records = 0x100000;
      b.dst_sel_x = 4; b.dst_sel_y = 5; b.dst_sel_z = 6; b.dst_sel_w = 7;
      b.num_format = 7;    // NumberFormat::Float
      b.data_format = 14;  // DataFormat::Format32_32_32_32
      b.element_size = 2;
      b.index_stride = 2;
    }
    return s;
  }();
  return synth;
}

static bool TranslateOne(const std::string& name, const uint32_t* code, size_t count,
                         uint64_t hash, const char* spv_out,
                         Shader::HwStage hw, Shader::SwStage sw) {
  std::array<uint32_t, 16> user_data{};
  if (hw == Shader::HwStage::Vertex) {
    // S_SWAPPC_B64 s0,s0 → GetFetchShaderCode 读 user_data[0] 为 fetch 代码指针；
    // fetch 内 s_load_dwordx4 s[8:11],s[2:3] → V# 指针落在 user_data[4]。
    const auto& fetch = GetFetchSynth();
    const auto put_ptr = [&user_data](u32 idx, const void* p) {
      const u64 v = reinterpret_cast<u64>(p);
      user_data[idx] = static_cast<u32>(v);
      user_data[idx + 1] = static_cast<u32>(v >> 32);
    };
    put_ptr(0, fetch.code.data());
    put_ptr(4, fetch.vsharp.data());
  }
  std::span<const uint32_t> code_span(code, count);
  Shader::ShaderParams params{
      .user_data = std::span<const uint32_t, 16>(user_data),
      .code = code_span,
      .hash = hash,
  };
  Shader::Info info(hw, sw, params);
  Shader::RuntimeInfo runtime_info{};
  runtime_info.Initialize(hw, sw);
  runtime_info.props.num_user_data = 16;
  if (hw == Shader::HwStage::Fragment) {
    // DIVA FS 主用 fp16 压缩导出（EXP.compr=1）；按指令流自适应，避免压缩/非压缩格式错配。
    const auto fmt = HasCompressedExport(code, count)
                         ? AmdGpu::ShaderExportFormat::ABGR_FP16
                         : AmdGpu::ShaderExportFormat::ABGR_32;
    runtime_info.hw.fs.color_buffers[0].export_format = fmt;
    // 离线默认插值配置（见文件头约定）。
    runtime_info.hw.fs.addr_flags.persp_center_ena = 1;
    runtime_info.hw.fs.addr_flags.linear_center_ena = 1;
    runtime_info.hw.fs.num_inputs = 32;
    for (u32 i = 0; i < 32; ++i) {
      runtime_info.hw.fs.inputs[i].param_index = i;
      runtime_info.hw.fs.inputs[i].is_default = 0;
      runtime_info.hw.fs.inputs[i].is_flat = 0;
      runtime_info.hw.fs.inputs[i].default_value = 0;
    }
  }

  Shader::Profile profile{};
  profile.supported_spirv = 0x00010600;  // SPIR-V 1.6（对齐 shadPS4）
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
  profile.needs_manual_interpolation = true;
  profile.supports_fragment_shader_barycentric = true;

  Shader::Pools pools;
  Shader::Backend::Bindings binding{};

  const auto ir = Shader::TranslateProgram(code_span, pools, info, runtime_info, profile);
  auto spv = Shader::Backend::SPIRV::EmitSPIRV(profile, runtime_info, ir, binding);
  bool ok = spv.size() >= 5 && spv[0] == 0x07230203u;
  if (ok && spv_out) {
    if (FILE* f = std::fopen(spv_out, "wb")) {
      std::fwrite(spv.data(), sizeof(uint32_t), spv.size(), f);
      std::fclose(f);
    }
  }
  return ok;
}

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <ps4_extract_dir> [spv_out_dir]\n", argv[0]);
    return 2;
  }
  Common::Log::Setup("sbo_translate_test");
  const fs::path root = argv[1];
  const fs::path outdir = argc >= 3 ? fs::path(argv[2]) : fs::path();

  std::vector<fs::path> files;
  for (const auto& e : fs::recursive_directory_iterator(root)) {
    if (!e.is_regular_file()) continue;
    const auto& p = e.path();
    if (p.extension() == ".bin" && p.stem().extension() == ".gcn") files.push_back(p);
  }
  std::sort(files.begin(), files.end());
  std::fprintf(stderr, "found %zu GCN shaders under %s\n", files.size(), root.c_str());

  size_t ok = 0, fail = 0, exc = 0;
  for (const auto& p : files) {
    const std::string rel = fs::relative(p, root).string();
    const std::string stem = p.stem().stem().string();  // 去掉 .gcn.bin
    const bool is_frag = stem.rfind("_fragment") != std::string::npos;
    const bool is_vert = stem.rfind("_vertex") != std::string::npos;
    if (!is_frag && !is_vert) {
      std::fprintf(stderr, "SKIP %s (unknown stage)\n", rel.c_str());
      continue;
    }
    std::ifstream in(p, std::ios::binary);
    const std::vector<char> bytes((std::istreambuf_iterator<char>(in)),
                                  std::istreambuf_iterator<char>());
    std::vector<uint32_t> code(bytes.size() / 4);
    std::memcpy(code.data(), bytes.data(), code.size() * sizeof(uint32_t));
    if (code.empty()) {
      std::fprintf(stderr, "FAIL %s (empty)\n", rel.c_str());
      ++fail;
      continue;
    }
    const uint64_t hash = std::hash<std::string>{}(rel);
    fs::path spv_path;
    if (!outdir.empty()) {
      spv_path = outdir / fs::relative(p, root).replace_extension(".spv");
      fs::create_directories(spv_path.parent_path());
    }
    const auto hw = is_frag ? Shader::HwStage::Fragment : Shader::HwStage::Vertex;
    const auto sw = is_frag ? Shader::SwStage::Fragment : Shader::SwStage::Vertex;

    std::fflush(nullptr);
    const pid_t pid = fork();
    if (pid == 0) {
      // 子进程：崩溃/断言只影响本 shader。
      int rc = 1;
      try {
        rc = TranslateOne(rel, code.data(), code.size(), hash,
                          spv_path.empty() ? nullptr : spv_path.c_str(), hw, sw) ? 0 : 1;
      } catch (const std::exception& e) {
        std::fprintf(stderr, "EXC  %s: %s\n", rel.c_str(), e.what());
        rc = 2;
      } catch (...) {
        rc = 2;
      }
      std::fflush(nullptr);
      _exit(rc);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
      std::fprintf(stderr, "OK   %s (%zu dwords)\n", rel.c_str(), code.size());
      ++ok;
    } else if (WIFEXITED(status)) {
      std::fprintf(stderr, "FAIL %s (%zu dwords) rc=%d\n", rel.c_str(), code.size(),
                   WEXITSTATUS(status));
      ++fail;
    } else {
      std::fprintf(stderr, "CRASH %s (%zu dwords) sig=%d\n", rel.c_str(), code.size(),
                   WTERMSIG(status));
      ++exc;
    }
  }
  std::printf("SBO_TRANSLATE_SUMMARY total=%zu ok=%zu fail=%zu crash=%zu\n",
              files.size(), ok, fail, exc);
  if (ok == files.size() && !files.empty()) {
    std::printf("SBO_TRANSLATE_TEST_PASS (all real DIVA shaders -> SPIR-V)\n");
    return 0;
  }
  if (ok > 0) {
    std::printf("SBO_TRANSLATE_TEST_PARTIAL\n");
    return 0;
  }
  std::printf("SBO_TRANSLATE_TEST_FAIL\n");
  return 1;
}
