// PM4 解析器宿主测试：合成命令缓冲 → 验证分发到 NullVideoBackend 的计数。
#include "divaft/pm4.h"
#include "divaft/pm4_parser.h"
#include "divaft/video_backend.h"
#include <cstdio>
#include <vector>

using namespace divaft;
static int g_fail = 0;
#define CHECK(c) do{ if(!(c)){ std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } }while(0)

static uint32_t Hdr(uint32_t opcode, uint32_t num_words) {
  return (3u << 30) | (opcode << 8) | ((num_words - 1) << 16);
}

int main(){
  NullVideoBackend vb;
  std::vector<uint32_t> cb;
  // Nop（1 DWORD 体）
  cb.push_back(Hdr(PM4_NOP, 1)); cb.push_back(0);
  // DrawIndex2：index_count=100, base=0x1000, initiator=1
  cb.push_back(Hdr(PM4_DRAW_INDEX_2, 5));
  cb.push_back(100);            // max_size
  cb.push_back(0x1000);         // index_base_lo
  cb.push_back(0);              // index_base_hi
  cb.push_back(100);            // index_count
  cb.push_back(1);              // draw_initiator
  // DispatchDirect：8x4x2
  cb.push_back(Hdr(PM4_DISPATCH_DIRECT, 4));
  cb.push_back(8); cb.push_back(4); cb.push_back(2); cb.push_back(0);
  // SetShReg：reg=0x100, 2 个值
  cb.push_back(Hdr(PM4_SET_SH_REG, 3));
  cb.push_back(0x100); cb.push_back(0xAA); cb.push_back(0xBB);
  // DrawIndexAuto：index_count=50
  cb.push_back(Hdr(PM4_DRAW_INDEX_AUTO, 2));
  cb.push_back(50); cb.push_back(1);

  size_t packets = PM4Parser::Parse(cb.data(), cb.size(), &vb);
  std::printf("packets=%zu draws=%llu dispatches=%llu reg_writes=%llu\n",
      packets, (unsigned long long)vb.draws(),
      (unsigned long long)vb.dispatches(), (unsigned long long)vb.reg_writes());
  CHECK(packets == 5);
  CHECK(vb.draws() == 2);        // DrawIndex2 + DrawIndexAuto
  CHECK(vb.dispatches() == 1);
  CHECK(vb.reg_writes() == 2);   // SetShReg 2 值

  // 越界保护：截断的包不崩
  NullVideoBackend vb2;
  std::vector<uint32_t> bad = {Hdr(PM4_DRAW_INDEX_2, 5), 1, 2};  // 体不足
  size_t p2 = PM4Parser::Parse(bad.data(), bad.size(), &vb2);
  CHECK(p2 == 0);  // 越界即停
  CHECK(vb2.draws() == 0);

  if(g_fail==0) std::printf("PM4_PARSER_TEST_PASS\n");
  else std::printf("PM4_PARSER_TEST_FAIL (%d)\n", g_fail);
  return g_fail?1:0;
}