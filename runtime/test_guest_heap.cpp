// GuestHeap 宿主单元测试（x86-64 本机运行，验证分配器正确性）
// 编译: g++ -std=c++20 -O2 -I runtime/include test_guest_heap.cpp runtime/guest_memory.cpp -o /tmp/th
#include "divaft/guest_memory.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <random>
#include <cassert>

using divaft::GuestHeap;

static int g_fail = 0;
#define CHECK(c) do{ if(!(c)){ std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } }while(0)

int main(){
  // 16 对齐的宿主缓冲模拟 guest 内存
  alignas(16) static uint8_t buf[8*1024*1024];
  uint64_t base = reinterpret_cast<uint64_t>(buf);

  { // 基本 malloc/free + 对齐
    GuestHeap h; h.Init(base, sizeof(buf));
    uint64_t a=h.Malloc(100), b=h.Malloc(200), c=h.Malloc(1);
    CHECK(a && b && c);
    CHECK(a%16==0 && b%16==0 && c%16==0);
    CHECK(a!=b && b!=c);
    std::memset((void*)a,0xAA,100); std::memset((void*)b,0xBB,200);
    CHECK(((uint8_t*)a)[99]==0xAA && ((uint8_t*)b)[199]==0xBB);
    CHECK(h.UsableSize(a)>=100 && h.UsableSize(b)>=200);
    h.Free(b); h.Free(a); h.Free(c);
    CHECK(h.used()==0);
  }
  { // calloc 清零
    GuestHeap h; h.Init(base,sizeof(buf));
    uint64_t p=h.Calloc(64,16);
    CHECK(p);
    bool zero=true; for(int i=0;i<1024;i++) if(((uint8_t*)p)[i]) zero=false;
    CHECK(zero);
    h.Free(p);
  }
  { // realloc 增长保留内容
    GuestHeap h; h.Init(base,sizeof(buf));
    uint64_t p=h.Malloc(32);
    for(int i=0;i<32;i++) ((uint8_t*)p)[i]=(uint8_t)i;
    uint64_t q=h.Realloc(p,4096);
    CHECK(q);
    bool ok=true; for(int i=0;i<32;i++) if(((uint8_t*)q)[i]!=(uint8_t)i) ok=false;
    CHECK(ok);
    h.Free(q); CHECK(h.used()==0);
  }
  { // memalign 对齐
    GuestHeap h; h.Init(base,sizeof(buf));
    for(uint64_t al : {64,256,4096,65536}){
      uint64_t p=h.Memalign(al, 1000);
      CHECK(p && p%al==0);
      std::memset((void*)p,0x5A,1000);
      CHECK(h.UsableSize(p)==1000);
      h.Free(p);
    }
    CHECK(h.used()==0);
  }
  { // 全量分配->全释放->再大分配（验证合并）
    GuestHeap h; h.Init(base,sizeof(buf));
    std::vector<uint64_t> ps;
    for(int i=0;i<1000;i++){ uint64_t p=h.Malloc(1024); if(!p)break; ps.push_back(p); }
    CHECK(ps.size()>100);
    for(auto p:ps) h.Free(p);
    CHECK(h.used()==0);
    uint64_t big=h.Malloc(4*1024*1024);
    CHECK(big); // 合并后应能再分大块
    h.Free(big); CHECK(h.used()==0);
  }
  { // 随机压力 + 内容完整性
    GuestHeap h; h.Init(base,sizeof(buf));
    std::mt19937_64 rng(12345);
    struct Blk{uint64_t p; uint64_t sz; uint8_t tag;};
    std::vector<Blk> live;
    for(int it=0; it<20000; it++){
      int op = rng()%3;
      if(op==0 || live.empty()){
        uint64_t sz = 1 + rng()%8192;
        uint64_t p = h.Malloc(sz);
        if(p){ uint8_t tag=(uint8_t)(rng()&0xFF); std::memset((void*)p,tag,sz);
               live.push_back({p,sz,tag}); }
      } else if(op==1){
        size_t i = rng()%live.size();
        Blk b=live[i];
        bool ok=true; for(uint64_t k=0;k<b.sz;k++) if(((uint8_t*)b.p)[k]!=b.tag){ok=false;break;}
        CHECK(ok); // 内容未被相邻块踩踏
        h.Free(b.p); live[i]=live.back(); live.pop_back();
      } else {
        size_t i=rng()%live.size(); Blk b=live[i];
        uint64_t ns=1+rng()%16384;
        uint64_t np=h.Realloc(b.p,ns);
        if(np){ uint64_t keep=b.sz<ns?b.sz:ns;
                bool ok=true; for(uint64_t k=0;k<keep;k++) if(((uint8_t*)np)[k]!=b.tag){ok=false;break;}
                CHECK(ok);
                std::memset((void*)np,b.tag,ns);
                live[i]={np,ns,b.tag}; }
      }
    }
    for(auto&b:live) h.Free(b.p);
    CHECK(h.used()==0);
  }
  { // 溢出/边界
    GuestHeap h; h.Init(base,sizeof(buf));
    CHECK(h.Calloc(SIZE_MAX,2)==0);
    CHECK(h.Malloc(sizeof(buf)*2)==0); // 超大失败
    uint64_t p=h.Malloc(0); CHECK(p!=0); h.Free(p);
    h.Free(0); // 不崩
  }

  if(g_fail==0) std::printf("GUEST_HEAP_TEST_PASS\n");
  else std::printf("GUEST_HEAP_TEST_FAIL (%d)\n", g_fail);
  return g_fail?1:0;
}