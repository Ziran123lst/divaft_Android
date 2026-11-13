// divaft_Android — 输入 HLE（libScePad，阶段5 骨架）
// scePadRead/ReadState 从 InputPad 快照填 OrbisPadData（buttons/touchData/connected）。
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "divaft/hle_wire.h"
#include "divaft/input_pad.h"

namespace divaft {

// 注册 libScePad(12)。pad 为触屏输入合成器。返回注册条数。
size_t RegisterPadHle(HleWire& wire, InputPad* pad);

}  // namespace divaft