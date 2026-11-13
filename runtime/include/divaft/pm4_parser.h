// divaft_Android — PM4 命令缓冲解析器（图形骨架，阶段3）
// 遍历 GnmDriver 提交的 PM4 包，解析 Type-3 头并按 opcode 分发到 IVideoBackend。
// 对照 shadPS4 video_core/amdgpu/liverpool 的 ProcessCmd 逻辑（骨架节选）。
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "divaft/pm4.h"
#include "divaft/video_backend.h"
#include <cstddef>

namespace divaft {

class PM4Parser {
public:
  // 解析 dwords 个 DWORD 的命令缓冲，分发到 backend。返回处理的 Type-3 包数。
  // 安全边界：越界即停，不读越界内存。
  static size_t Parse(const uint32_t* cmdbuf, size_t dwords, IVideoBackend* backend);
};

}  // namespace divaft