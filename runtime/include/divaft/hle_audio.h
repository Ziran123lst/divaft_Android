// divaft_Android — 音频 HLE（libSceAudioOut + libSceNgs2，阶段4 骨架）
// AudioOut：输出端口，接 AudioOut（AAudio 低延迟路径）。
// Ngs2：DSP 混音 rack/voice，骨架记录型 stub，后续接 shadPS4 ngs2 HLE。
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "divaft/hle_wire.h"
#include "divaft/audio_out.h"

namespace divaft {

// 注册 AudioOut(6) + Ngs2(11)。audio 为输出后端（AAudio 接入点）。返回注册条数。
size_t RegisterAudioHle(HleWire& wire, AudioOut* audio);

}  // namespace divaft