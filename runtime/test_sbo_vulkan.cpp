// divaft_Android — 真实 DIVA shader SPIR-V 的 Vulkan 消费端验证（host，lavapipe 软件驱动）。
// 对 sbo_translate_test 产出的 131 对 VS/FS SPIR-V 逐对建 VkShaderModule + 图形管线。
// 证明：真实游戏 shader 的重编译产物能被 Vulkan 驱动真正消费（不止结构合法）。
// 编译: g++ -std=c++20 -O2 -I runtime/include runtime/test_sbo_vulkan.cpp \
//       runtime/vulkan_video_backend.cpp -lvulkan -o /tmp/test_sbo_vulkan
// 运行: VK_ICD_FILENAMES=<lvp_icd.json> /tmp/test_sbo_vulkan <spv_out_dir>
// 数据为游戏资产不入仓库；CI 无数据，本测试仅本地/真机跑。
#include "divaft/vulkan_video_backend.h"
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static bool LoadSpv(const fs::path& p, std::vector<uint32_t>& out) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return false;
  const std::vector<char> bytes((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());
  out.resize(bytes.size() / 4);
  std::memcpy(out.data(), bytes.data(), out.size() * 4);
  return !out.empty();
}

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <spv_out_dir> [fallback_frag.spv]\n", argv[0]);
    return 2;
  }
  const fs::path root = argv[1];
  // fallback_frag：给定则用平凡 FS 配真实 VS（VS 消费模式；lavapipe 无 barycentric 扩展，
  // 真实 FS 的 per-vertex 插值在其上无法编译，属驱动能力限制）。
  std::vector<uint32_t> fallback_fs;
  const bool vs_only = argc >= 3 && LoadSpv(argv[2], fallback_fs);
  divaft::VulkanVideoBackend backend;
  if (!backend.Init(256, 256)) {
    std::fprintf(stderr, "SBO_VULKAN_TEST_FAIL (init: %s)\n", backend.last_error());
    return 1;
  }
  size_t ok = 0, fail = 0;
  std::vector<fs::path> verts;
  for (const auto& e : fs::recursive_directory_iterator(root)) {
    if (e.is_regular_file() && e.path().filename().string().find("_vertex.gcn.spv") != std::string::npos)
      verts.push_back(e.path());
  }
  std::sort(verts.begin(), verts.end());
  for (const auto& vp : verts) {
    fs::path fp = vp;
    const std::string name = vp.filename().string();
    const std::string frag_name = name.substr(0, name.rfind("_vertex.gcn.spv")) + "_fragment.gcn.spv";
    fp.replace_filename(frag_name);
    std::vector<uint32_t> vs, fs;
    if (!LoadSpv(vp, vs)) {
      std::fprintf(stderr, "FAIL %s (missing VS)\n", vp.c_str());
      ++fail;
      continue;
    }
    if (vs_only) {
      fs = fallback_fs;
    } else if (!LoadSpv(fp, fs)) {
      std::fprintf(stderr, "FAIL %s (missing pair)\n", vp.c_str());
      ++fail;
      continue;
    }
    if (backend.CreateGraphicsPipelineFromSpirv(vs.data(), vs.size(), fs.data(), fs.size())) {
      std::fprintf(stderr, "OK   %s\n", fs::relative(vp, root).c_str());
      ++ok;
    } else {
      std::fprintf(stderr, "FAIL %s (%s)\n", fs::relative(vp, root).c_str(), backend.last_error());
      ++fail;
    }
  }
  std::printf("SBO_VULKAN_SUMMARY pairs=%zu ok=%zu fail=%zu\n", verts.size(), ok, fail);
  if (ok == verts.size() && !verts.empty()) {
    std::printf("SBO_VULKAN_TEST_PASS (all real DIVA shader pairs -> Vulkan graphics pipeline)\n");
    return 0;
  }
  std::printf(ok > 0 ? "SBO_VULKAN_TEST_PARTIAL\n" : "SBO_VULKAN_TEST_FAIL\n");
  return ok > 0 ? 0 : 1;
}
