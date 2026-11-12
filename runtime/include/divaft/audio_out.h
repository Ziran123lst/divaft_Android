// divaft_Android — 音频输出（Ngs2 → AAudio，音游低延迟关键路径）
// 原创接口。Ngs2 DSP 由 shadPS4 HLE 提供，本模块负责把混音输出低延迟送到 AAudio，
// 并提供判定偏移校准（音游刚需）。
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <cstddef>

namespace divaft {

struct AudioConfig {
  uint32_t sample_rate = 48000;   // PS4 AudioOut 强制 48kHz
  uint32_t channels = 2;
  uint32_t buffer_frames = 256;   // 低延迟缓冲（待真机调优）
  int32_t judgment_offset_ms = 0; // 判定偏移校准（±100ms）
};

class AudioOut {
public:
  bool Open(const AudioConfig& cfg);
  // 提交一帧混音输出（float interleaved）。由 Ngs2 render 回调驱动。
  void Submit(const float* samples, size_t frames);
  void SetJudgmentOffset(int32_t ms);
  int32_t judgment_offset_ms() const { return cfg_.judgment_offset_ms; }
  void Close();

private:
  AudioConfig cfg_;
  void* stream_ = nullptr;  // AAudioStream*（AAudio 接入后填充）
};

}  // namespace divaft
