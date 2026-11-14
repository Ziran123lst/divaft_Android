// divaft_Android — 图形渲染验证（host，lavapipe 软件 Vulkan）。
// 证明1：重编译器产的 vertex/fragment SPIR-V 能被 Vulkan 图形管线接受。
// 证明2：VulkanVideoBackend 离屏渲染可见三角形并读回非黑像素。
// 编译: g++ -std=c++20 -O2 -I runtime/include test_vulkan_render.cpp vulkan_video_backend.cpp -lvulkan
// 运行: ./test_vulkan_render <rec_vert.spv> <rec_frag.spv> <tri_vert.spv> <tri_frag.spv>
#include "divaft/vulkan_video_backend.h"
#include <cstdio>
#include <cstdlib>
#include <vector>

static bool LoadSpv(const char* path, std::vector<uint32_t>& out) {
  FILE* f = std::fopen(path, "rb");
  if (!f) {
    std::fprintf(stderr, "cannot open %s\n", path);
    return false;
  }
  std::fseek(f, 0, SEEK_END);
  long bytes = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  if (bytes <= 0 || bytes % 4 != 0) {
    std::fclose(f);
    return false;
  }
  out.resize(bytes / 4);
  bool ok = std::fread(out.data(), 1, bytes, f) == static_cast<size_t>(bytes);
  std::fclose(f);
  return ok;
}

int main(int argc, char** argv) {
  const char* rec_vert = argc > 1 ? argv[1] : "/tmp/diva_vert.spv";
  const char* rec_frag = argc > 2 ? argv[2] : "/tmp/diva_frag.spv";
  const char* tri_vert = argc > 3 ? argv[3] : "/tmp/tri_vert.spv";
  const char* tri_frag = argc > 4 ? argv[4] : "/tmp/tri_frag.spv";

  divaft::VulkanVideoBackend backend;
  if (!backend.Init(256, 256)) {
    std::fprintf(stderr, "VULKAN_RENDER_TEST_FAIL (init: %s)\n", backend.last_error());
    return 1;
  }

  // 证明1：重编译器 graphics SPIR-V → 图形管线。
  std::vector<uint32_t> rv, rf;
  if (!LoadSpv(rec_vert, rv) || !LoadSpv(rec_frag, rf)) {
    std::fprintf(stderr, "VULKAN_RENDER_TEST_FAIL (load recompiler spv)\n");
    return 1;
  }
  if (!backend.CreateGraphicsPipelineFromSpirv(rv.data(), rv.size(), rf.data(), rf.size())) {
    std::fprintf(stderr, "VULKAN_RENDER_TEST_FAIL (recompiler graphics pipeline: %s)\n",
                 backend.last_error());
    return 1;
  }
  std::fprintf(stderr, "[test] recompiler vertex+fragment SPIR-V accepted by graphics pipeline\n");

  // 证明2：可见三角形离屏渲染 + 像素读回。
  std::vector<uint32_t> tv, tf;
  if (!LoadSpv(tri_vert, tv) || !LoadSpv(tri_frag, tf)) {
    std::fprintf(stderr, "VULKAN_RENDER_TEST_FAIL (load triangle spv)\n");
    return 1;
  }
  std::vector<uint8_t> pixels;
  if (!backend.RenderGraphicsOffscreen(64, 64, tv.data(), tv.size(), tf.data(), tf.size(), pixels)) {
    std::fprintf(stderr, "VULKAN_RENDER_TEST_FAIL (offscreen render: %s)\n", backend.last_error());
    return 1;
  }
  size_t nonblack = 0;
  for (size_t i = 0; i + 3 < pixels.size(); i += 4) {
    if (pixels[i] || pixels[i + 1] || pixels[i + 2]) {
      ++nonblack;
    }
  }
  std::fprintf(stderr, "[test] rendered 64x64, non-black pixels=%zu / %zu\n", nonblack,
               pixels.size() / 4);
  if (nonblack == 0) {
    std::fprintf(stderr, "VULKAN_RENDER_TEST_FAIL (no visible pixels)\n");
    return 1;
  }
  std::printf("VULKAN_RENDER_TEST_PASS (recompiler graphics SPIR-V accepted + triangle rendered, "
              "%zu non-black pixels)\n", nonblack);
  return 0;
}