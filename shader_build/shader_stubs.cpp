// divaft_Android — shader_recompiler 独立构建的最小基础设施 stub。
// shader 引擎只用到 EmulatorSettings 的少数布尔开关与线程名；此处提供最小实现，
// 避免拉进完整 shadPS4 模拟器（SDL3/音频/输入等）。
#include "core/emulator_settings.h"
#include "common/thread.h"
#include <memory>
#include <mutex>
#include <string>

// EmulatorSettingsImpl：shader 路径仅读 IsDirectMemoryAccessEnabled/IsInlineFetchShader/IsDumpShaders
// （均为头文件内联 getter），这里只需提供实例生命周期。
EmulatorSettingsImpl::EmulatorSettingsImpl() = default;
EmulatorSettingsImpl::~EmulatorSettingsImpl() = default;

std::shared_ptr<EmulatorSettingsImpl> EmulatorSettingsImpl::GetInstance() {
  static std::mutex mu;
  static std::shared_ptr<EmulatorSettingsImpl> inst;
  std::lock_guard<std::mutex> lk(mu);
  if (!inst) {
    inst = std::make_shared<EmulatorSettingsImpl>();
    inst->SetDumpShaders(true);  // 诊断：dump IR 到 ShaderDir/dumps，定位顶点索引 prologue 消除点
  }
  return inst;
}

void EmulatorSettingsImpl::SetInstance(std::shared_ptr<EmulatorSettingsImpl> instance) {
  // 独立构建不需要替换实例。
  (void)instance;
}

namespace Common {
std::string GetCurrentThreadName() {
  return "shader";
}
}  // namespace Common

// shadPS4 main 新增：path_util 的 FindGameByID 引用 zar 归档接口。
// 独立 shader 构建不读游戏归档：IsZArchiveFile 照抄原实现，ReadGameFile 桩返回空。
#include <filesystem>
#include <optional>
#include <vector>
#include <string_view>
#include <cstdint>
namespace Core::FileSys {
bool IsZArchiveFile(const std::filesystem::path& path) {
  std::error_code ec;
  return path.extension() == ".zar" && std::filesystem::is_regular_file(path, ec) && !ec;
}
std::optional<std::vector<std::uint8_t>> ReadGameFile(const std::filesystem::path&,
                                                      std::string_view) {
  return std::nullopt;
}
}  // namespace Core::FileSys

#ifdef __ANDROID__
// qemu 静态测试剔除 -llog（仅动态版）；若 spdlog/基础设施引用安卓日志符号，由此桩解析。
#include <cstdarg>
extern "C" int __android_log_print(int prio, const char* tag, const char* fmt, ...) {
  (void)prio; (void)tag; (void)fmt;
  return 0;
}
extern "C" int __android_log_write(int prio, const char* tag, const char* text) {
  (void)prio; (void)tag; (void)text;
  return 0;
}
extern "C" int __android_log_buf_print(int bufID, int prio, const char* tag, const char* fmt, ...) {
  (void)bufID; (void)prio; (void)tag; (void)fmt;
  return 0;
}
#endif