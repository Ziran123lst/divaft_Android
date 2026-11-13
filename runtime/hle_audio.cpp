// divaft_Android — 音频 HLE 实现（阶段4 骨架）
// AudioOut 接 AudioOut（AAudio 低延迟接入点）；Ngs2 DSP 整库记录型 stub（待接 shadPS4 ngs2）。
// SPDX-License-Identifier: GPL-2.0-or-later
#include "divaft/hle_audio.h"

#include <cstdio>
#include <atomic>

namespace divaft {

namespace {
std::atomic<uint64_t> g_ngs2_calls{0};
}

size_t RegisterAudioHle(HleWire& wire, AudioOut* audio) {
  size_t n = 0;
  auto rega = [&](const char* name, HleFn fn) {
    if (wire.RegisterByName("libSceAudioOut", name, std::move(fn))) ++n;
  };

  rega("sceAudioOutInit", [audio](const uint64_t*, unsigned) -> uint64_t {
    if (audio) audio->Open(AudioConfig{});
    return 0;
  });
  // sceAudioOutOpen(userId, type, index, len, freq, format) -> 端口句柄
  rega("sceAudioOutOpen", [](const uint64_t*, unsigned) -> uint64_t {
    return 1;  // 音频端口句柄
  });
  // sceAudioOutOutput(handle, buf)：提交一帧（buf 为 guest 采样缓冲）
  rega("sceAudioOutOutput", [audio](const uint64_t* a, unsigned) -> uint64_t {
    if (audio && a[1]) {
      // 骨架：按 float interleaved 提交 256 帧（真实格式/帧长待 Ngs2 对接后精确化）
      audio->Submit(reinterpret_cast<const float*>(a[1]), 256);
    }
    return 0;
  });
  rega("sceAudioOutOutputs", [audio](const uint64_t* a, unsigned) -> uint64_t {
    (void)audio; (void)a;
    return 0;
  });
  rega("sceAudioOutSetVolume", [](const uint64_t*, unsigned) -> uint64_t { return 0; });
  rega("sceAudioOutClose", [](const uint64_t*, unsigned) -> uint64_t { return 0; });

  // ---- libSceNgs2：DSP 混音，整库记录型 stub（11 函数）----
  n += wire.RegisterLibrary("libSceNgs2",
      [](const uint64_t*, unsigned) -> uint64_t {
        g_ngs2_calls.fetch_add(1);
        return 0;
      });

  return n;
}

}  // namespace divaft