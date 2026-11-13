// divaft_Android — PM4 命令包定义（图形骨架，阶段3）
// 位域/opcode 对照 shadPS4 video_core/amdgpu/pm4_cmds.h、pm4_opcodes.h（AMD Liverpool）。
// GnmDriver 提交的命令缓冲由 PM4 包组成；本头提供解析所需的结构与常量。
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>

namespace divaft {

// PM4 Type-3 包头（32 位）：
//   bit0 predicate | bit1 shader_type(0=graphics,1=compute) | bit8-15 opcode
//   bit16-29 count(=DWORD数-1) | bit30-31 type(=3)
struct PM4Type3Header {
  uint32_t raw;
  uint32_t type() const { return (raw >> 30) & 0x3; }
  uint32_t opcode() const { return (raw >> 8) & 0xFF; }
  uint32_t num_words() const { return ((raw >> 16) & 0x3FFF) + 1; }  // 信息体 DWORD 数
  uint32_t shader_type() const { return (raw >> 1) & 0x1; }
  uint32_t predicate() const { return raw & 0x1; }
};

// 关键 IT opcode（对照 pm4_opcodes.h，节选启动/绘制常用）。
enum PM4Opcode : uint32_t {
  PM4_NOP = 0x10,
  PM4_SET_BASE = 0x11,
  PM4_CLEAR_STATE = 0x12,
  PM4_DISPATCH_DIRECT = 0x15,
  PM4_DISPATCH_INDIRECT = 0x16,
  PM4_SET_PREDICATION = 0x20,
  PM4_REG_RMW = 0x21,
  PM4_COND_EXEC = 0x22,
  PM4_DRAW_INDIRECT = 0x24,
  PM4_DRAW_INDEX_INDIRECT = 0x25,
  PM4_INDEX_BASE = 0x26,
  PM4_DRAW_INDEX_2 = 0x27,
  PM4_CONTEXT_CONTROL = 0x28,
  PM4_INDEX_TYPE = 0x2A,
  PM4_DRAW_INDEX_AUTO = 0x2D,
  PM4_NUM_INSTANCES = 0x2F,
  PM4_DRAW_INDEX_OFFSET_2 = 0x35,
  PM4_WRITE_DATA = 0x37,
  PM4_MEM_SEMAPHORE = 0x39,
  PM4_WAIT_REG_MEM = 0x3C,
  PM4_INDIRECT_BUFFER = 0x3F,
  PM4_COPY_DATA = 0x40,
  PM4_SURFACE_SYNC = 0x43,
  PM4_EVENT_WRITE = 0x46,
  PM4_EVENT_WRITE_EOP = 0x47,
  PM4_EVENT_WRITE_EOS = 0x48,
  PM4_RELEASE_MEM = 0x49,
  PM4_ACQUIRE_MEM = 0x58,
  PM4_SET_CONFIG_REG = 0x68,
  PM4_SET_CONTEXT_REG = 0x69,
  PM4_SET_SH_REG = 0x76,
  PM4_SET_UCONFIG_REG = 0x79,
};

// DrawIndex2（0x27）：索引绘制。
struct PM4DrawIndex2 {
  uint32_t max_size;
  uint32_t index_base_lo;   // [31:1]
  uint32_t index_base_hi;   // [39:32]
  uint32_t index_count;
  uint32_t draw_initiator;
  uint64_t index_base() const {
    return ((uint64_t)index_base_hi << 32) | (index_base_lo & ~1u);
  }
};

// DispatchDirect（0x15）：计算分发。
struct PM4DispatchDirect {
  uint32_t dim_x;
  uint32_t dim_y;
  uint32_t dim_z;
  uint32_t control;
};

}  // namespace divaft