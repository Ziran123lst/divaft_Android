// divaft_Android — PM4 命令缓冲解析器实现（图形骨架，阶段3）
// SPDX-License-Identifier: GPL-2.0-or-later
#include "divaft/pm4_parser.h"

namespace divaft {

size_t PM4Parser::Parse(const uint32_t* cmdbuf, size_t dwords, IVideoBackend* backend) {
  if (!cmdbuf || !backend) return 0;
  size_t pos = 0, packets = 0;
  while (pos < dwords) {
    PM4Type3Header hdr{cmdbuf[pos]};
    if (hdr.type() != 3) { ++pos; continue; }  // 非 Type-3：逐 DWORD 跳过（骨架）
    uint32_t body = hdr.num_words();
    if (pos + 1 + body > dwords) break;         // 越界保护
    const uint32_t* p = cmdbuf + pos + 1;
    switch (hdr.opcode()) {
      case PM4_DRAW_INDEX_2:
        if (body >= 5) {
          auto* d = reinterpret_cast<const PM4DrawIndex2*>(p);
          backend->DrawIndex(d->index_count, d->index_base(), d->draw_initiator);
        }
        break;
      case PM4_DRAW_INDEX_AUTO:
        if (body >= 2) backend->DrawIndex(p[0], 0, p[1]);
        break;
      case PM4_DRAW_INDEX_OFFSET_2:
        if (body >= 4) backend->DrawIndex(p[2], 0, p[3]);
        break;
      case PM4_DISPATCH_DIRECT:
        if (body >= 4) {
          auto* d = reinterpret_cast<const PM4DispatchDirect*>(p);
          backend->Dispatch(d->dim_x, d->dim_y, d->dim_z);
        }
        break;
      case PM4_SET_SH_REG:
      case PM4_SET_CONTEXT_REG:
      case PM4_SET_CONFIG_REG:
      case PM4_SET_UCONFIG_REG: {
        // p[0]=寄存器偏移，其后为值。逐寄存器上报。
        bool is_sh = (hdr.opcode() == PM4_SET_SH_REG);
        for (uint32_t i = 1; i < body; ++i)
          backend->SetRegister(p[0] + (i - 1), p[i], is_sh);
        break;
      }
      default:
        break;  // NOP/标记/同步等：骨架暂跳过
    }
    ++packets;
    pos += 1 + body;
  }
  return packets;
}

}  // namespace divaft