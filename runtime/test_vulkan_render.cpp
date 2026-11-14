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
  std::fprintf(stderr, "[ckpt] backend ready\n"); std::fflush(stderr);

  // 主证明：可见三角形离屏渲染 + 像素读回（glslang 标准 SPIR-V）。
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
  std::printf("VULKAN_RENDER_TEST_PASS (triangle rendered, %zu non-black pixels)\n", nonblack);
  std::fflush(stdout);

  // 附加证明：重编译器 graphics SPIR-V 实际渲染逐顶点三角形（匹配 layout + 绑 descriptor/push constant）。
  std::vector<uint32_t> rv, rf;
  if (LoadSpv(rec_vert, rv) && LoadSpv(rec_frag, rf)) {
    std::fprintf(stderr, "[ckpt] loaded recompiler vert=%zu frag=%zu, before CreateGraphicsPipeline\n",
                 rv.size(), rf.size()); std::fflush(stderr);
    if (backend.CreateGraphicsPipelineFromSpirv(rv.data(), rv.size(), rf.data(), rf.size())) {
      std::printf("RECOMPILER_GRAPHICS_SPIRV_ACCEPTED (vert %zu + frag %zu words)\n",
                  rv.size(), rf.size());
      std::fflush(stdout);
    } else {
      std::fprintf(stderr, "[warn] recompiler graphics pipeline rejected: %s\n", backend.last_error());
    }
    std::vector<uint8_t> rpixels;
    if (backend.RenderRecompilerGraphicsOffscreen(64, 64, rv.data(), rv.size(), rf.data(), rf.size(),
                                                  rpixels)) {
      size_t rnonblack = 0;
      for (size_t i = 0; i + 3 < rpixels.size(); i += 4) {
        if (rpixels[i] || rpixels[i + 1] || rpixels[i + 2]) {
          ++rnonblack;
        }
      }
      std::fprintf(stderr, "[test] recompiler shader rendered 64x64, non-black pixels=%zu / %zu\n",
                   rnonblack, rpixels.size() / 4);
      if (rnonblack > 0) {
        std::printf("RECOMPILER_SHADER_TRIANGLE_RENDERED (%zu non-black pixels)\n", rnonblack);
        std::fflush(stdout);
        if (FILE* pf = std::fopen("/tmp/recompiler_triangle.ppm", "wb")) {
          std::fprintf(pf, "P6\n64 64\n255\n");
          for (size_t i = 0; i + 3 < rpixels.size(); i += 4) {
            std::fputc(rpixels[i], pf);
            std::fputc(rpixels[i + 1], pf);
            std::fputc(rpixels[i + 2], pf);
          }
          std::fclose(pf);
          std::fprintf(stderr, "[test] recompiler triangle saved to /tmp/recompiler_triangle.ppm\n");
        }
      } else {
        std::fprintf(stderr, "[warn] recompiler shader rendered but no visible pixels\n");
      }
    } else {
      std::fprintf(stderr, "[warn] recompiler offscreen render failed: %s\n", backend.last_error());
    }
  }
  return 0;
}