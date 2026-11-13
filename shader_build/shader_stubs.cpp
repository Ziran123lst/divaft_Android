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
  if (!inst) inst = std::make_shared<EmulatorSettingsImpl>();
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