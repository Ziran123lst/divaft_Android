// divaft_Android — 音频输出实现（骨架，AAudio 接入后填充）
// SPDX-License-Identifier: GPL-2.0-or-later
#include "divaft/audio_out.h"

namespace divaft {

bool AudioOut::Open(const AudioConfig& cfg) {
  cfg_ = cfg;
  // AAudio 低延迟路径接入点：
  //   AAudioStreamBuilder_setPerformanceMode(LOW_LATENCY)
  //   AAudioStreamBuilder_setSharingMode(EXCLUSIVE)（若设备支持 MMAP）
  //   sample_rate=48000, channels, buffer_frames
  // 见 docs/architecture.md §5 音频延迟风险。
  stream_ = nullptr;  // 待 AAudio 接入
  return true;
}

void AudioOut::Submit(const float* samples, size_t frames) {
  // AAudioStream_write(stream_, samples, frames, timeout)
  (void)samples; (void)frames;
}

void AudioOut::SetJudgmentOffset(int32_t ms) { cfg_.judgment_offset_ms = ms; }

void AudioOut::Close() { stream_ = nullptr; }

}  // namespace divaft
