// 图形/音频/输入 HLE 骨架宿主测试：验证注册解析 + Pad 数据填充 + 视频后端。
#include "divaft/hle_wire.h"
#include "divaft/hle_graphics.h"
#include "divaft/hle_audio.h"
#include "divaft/hle_pad.h"
#include "divaft/video_backend.h"
#include "divaft/audio_out.h"
#include "divaft/input_pad.h"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

using namespace divaft;
static int g_fail = 0;
#define CHECK(c) do{ if(!(c)){ std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } }while(0)

int main(int argc, char** argv){
  const char* contract = argc>1 ? argv[1] : "runtime/data/contract.tsv";
  HleWire wire;
  CHECK(wire.LoadContract(contract));
  std::unordered_map<std::string,std::string> nid_of;
  { std::ifstream f(contract); std::string line;
    while(std::getline(f,line)){ if(line.empty()||line[0]=='#')continue;
      std::istringstream ss(line); std::string nid,lib,name;
      std::getline(ss,nid,'\t');std::getline(ss,lib,'\t');std::getline(ss,name,'\t');
      nid_of[lib+"\t"+name]=nid; } }

  NullVideoBackend vb; AudioOut ao; InputPad pad;
  size_t ng = RegisterGraphicsHle(wire, &vb);
  size_t na = RegisterAudioHle(wire, &ao);
  size_t np = RegisterPadHle(wire, &pad);
  std::printf("注册 graphics=%zu audio=%zu pad=%zu\n", ng, na, np);
  CHECK(ng >= 100);  // GnmDriver 103 + VideoOut 8
  CHECK(na >= 15);   // AudioOut 6 + Ngs2 11
  CHECK(np >= 10);   // Pad 12

  auto call = [&](const char* lib, const char* name, std::initializer_list<uint64_t> args)->uint64_t{
    auto it = nid_of.find(std::string(lib)+"\t"+name);
    if(it==nid_of.end()) return UINT64_MAX;
    const HleBinding* b = wire.Resolve(it->second);
    if(!b) return UINT64_MAX;
    std::vector<uint64_t> a(args); a.resize(8,0);
    HleReturn hr{};
    return b->fn(a.data(),(unsigned)a.size(),nullptr,&hr);
  };
  // 关键函数解析
  CHECK(call("libSceVideoOut","sceVideoOutOpen",{0,0,0,0})==1);
  CHECK(call("libSceGnmDriver","sceGnmDrawIndex",{0,0,0,0,0,0})==0);
  CHECK(call("libSceAudioOut","sceAudioOutOpen",{0,0,0,256,48000,0})==1);
  CHECK(call("libSceNgs2","sceNgs2SystemCreate",{0,0})==0);

  // Pad：模拟触屏 → scePadRead 填 OrbisPadData
  pad.OnTouchDown(0, 100, 100, 1000, 1000);  // 左上=△
  uint8_t padbuf[128]; std::memset(padbuf,0,sizeof(padbuf));
  uint64_t r = call("libScePad","scePadRead",{1,(uint64_t)padbuf,1});
  CHECK(r==1);
  uint32_t buttons = *reinterpret_cast<uint32_t*>(padbuf);
  CHECK(buttons == kPadTriangle);
  CHECK(padbuf[76]==1);  // connected
  CHECK(padbuf[52]==1);  // touchNum
  pad.OnTouchUp(0);

  // 视频后端：Flip / 命令缓冲计数
  call("libSceVideoOut","sceVideoOutWaitVblank",{1});
  CHECK(vb.flips()==0);  // WaitVblank 不计 flip
  vb.Flip(0,0); vb.SubmitCommandBuffer(nullptr,0);
  CHECK(vb.flips()==1 && vb.cmdbufs()==1);

  if(g_fail==0) std::printf("HLE_MEDIA_TEST_PASS\n");
  else std::printf("HLE_MEDIA_TEST_FAIL (%d)\n", g_fail);
  return g_fail?1:0;
}