// divaft_Android — 真机 Vulkan 扩展裁决工具（ARM64 动态链接，设备上跑）。
// 关键问题：DIVA FS 的 per-vertex 插值必需 VK_KHR_fragment_shader_barycentric
// （或 VK_AMD_shader_explicit_vertex_parameter）。Turnip/Adreno 732 是否支持，
// 只有真机能回答。用法: ./vk_ext_check（Termux 内直接跑，无需 surface）。
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
  VkApplicationInfo app = {0};
  app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  app.apiVersion = VK_API_VERSION_1_1;
  VkInstanceCreateInfo ci = {0};
  ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  ci.pApplicationInfo = &app;
  VkInstance inst;
  if (vkCreateInstance(&ci, NULL, &inst) != VK_SUCCESS) {
    printf("VK_EXT_CHECK_FAIL createInstance\n");
    return 1;
  }
  uint32_t n = 0;
  vkEnumeratePhysicalDevices(inst, &n, NULL);
  if (n == 0) {
    printf("VK_EXT_CHECK_FAIL no physical device\n");
    return 1;
  }
  VkPhysicalDevice* devs = malloc(n * sizeof(*devs));
  vkEnumeratePhysicalDevices(inst, &n, devs);
  for (uint32_t d = 0; d < n; ++d) {
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(devs[d], &props);
    uint32_t en = 0;
    vkEnumerateDeviceExtensionProperties(devs[d], NULL, &en, NULL);
    VkExtensionProperties* exts = malloc(en * sizeof(*exts));
    vkEnumerateDeviceExtensionProperties(devs[d], NULL, &en, exts);
    int bary = 0, amd = 0;
    for (uint32_t i = 0; i < en; ++i) {
      if (strcmp(exts[i].extensionName, "VK_KHR_fragment_shader_barycentric") == 0) bary = 1;
      if (strcmp(exts[i].extensionName, "VK_AMD_shader_explicit_vertex_parameter") == 0) amd = 1;
    }
    printf("device[%u]: %s (Vulkan %u.%u.%u)\n", d, props.deviceName,
           VK_VERSION_MAJOR(props.apiVersion), VK_VERSION_MINOR(props.apiVersion),
           VK_VERSION_PATCH(props.apiVersion));
    printf("VK_KHR_fragment_shader_barycentric: %s\n", bary ? "YES" : "NO");
    printf("VK_AMD_shader_explicit_vertex_parameter: %s\n", amd ? "YES" : "NO");
    free(exts);
  }
  free(devs);
  printf("VK_EXT_CHECK_DONE\n");
  return 0;
}
