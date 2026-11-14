// divaft_Android — shaders→Vulkan 消费端验证（host，软件 Vulkan lavapipe/SwiftShader 可跑）。
// 读取 shader_recompiler 落盘的 SPIR-V → VulkanVideoBackend 建 VkShaderModule→计算管线→dispatch。
// 证明重编译器产出的 SPIR-V 能被 Vulkan 真正消费（SPIR-V 架构无关，ARM64 产、host 消费）。
// 编译: g++ -std=c++20 -O2 -I runtime/include test_vulkan_backend.cpp vulkan_video_backend.cpp -lvulkan
// 运行: VK_ICD_FILENAMES=<lavapipe.json> ./test_vulkan_backend /tmp/diva_srt.spv
#include "divaft/vulkan_video_backend.h"
#include <cstdio>
#include <cstdlib>
#include <vector>

int main(int argc, char** argv) {
  const char* path = argc > 1 ? argv[1] : "/tmp/diva_srt.spv";
  FILE* f = std::fopen(path, "rb");
  if (!f) {
    std::fprintf(stderr, "cannot open SPIR-V file: %s\n", path);
    return 1;
  }
  std::fseek(f, 0, SEEK_END);
  long bytes = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  if (bytes <= 0 || bytes % 4 != 0) {
    std::fprintf(stderr, "bad SPIR-V size: %ld\n", bytes);
    std::fclose(f);
    return 1;
  }
  std::vector<uint32_t> spirv(bytes / 4);
  if (std::fread(spirv.data(), 1, bytes, f) != static_cast<size_t>(bytes)) {
    std::fprintf(stderr, "short read\n");
    std::fclose(f);
    return 1;
  }
  std::fclose(f);
  std::fprintf(stderr, "[test] loaded %s (%zu words)\n", path, spirv.size());

  divaft::VulkanVideoBackend backend;
  if (!backend.Init(256, 256)) {
    std::fprintf(stderr, "VULKAN_SPIRV_TEST_FAIL (init: %s)\n", backend.last_error());
    return 1;
  }
  if (!backend.CreateComputePipelineFromSpirv(spirv.data(), spirv.size())) {
    std::fprintf(stderr, "VULKAN_SPIRV_TEST_FAIL (pipeline: %s)\n", backend.last_error());
    return 1;
  }
  backend.Dispatch(1, 1, 1);
  std::printf("VULKAN_SPIRV_TEST_PASS (SPIR-V -> VkShaderModule -> compute pipeline -> dispatch OK)\n");
  return 0;
}