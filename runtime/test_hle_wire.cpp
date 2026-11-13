// HLE 接线宿主集成测试：加载真实契约，注册 kernel+libc，
// 从契约读关键函数 NID → 验证 Resolve 命中 → 通过绑定实际调用。
// 编译: g++ -std=c++20 -O2 -I runtime/include test_hle_wire.cpp
//   runtime/hle_wire.cpp runtime/hle_kernel.cpp runtime/hle_libc.cpp runtime/guest_memory.cpp -o /tmp/tw
#include "divaft/hle_wire.h"
#include "divaft/hle_kernel.h"
#include "divaft/hle_libc.h"
#include "divaft/guest_memory.h"
#include "divaft/guest_bridge.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>

using namespace divaft;
static int g_fail = 0;
#define CHECK(c) do{ if(!(c)){ std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } }while(0)

int main(int argc, char** argv){
  const char* contract = argc>1 ? argv[1] : "runtime/data/contract.tsv";
  HleWire wire;
  CHECK(wire.LoadContract(contract));
  std::printf("契约条数: %zu\n", wire.contract_size());
  CHECK(wire.contract_size() > 600);

  // 读契约建 name->nid（供测试按名查 NID）
  std::unordered_map<std::string,std::string> nid_of;
  { std::ifstream f(contract); std::string line;
    while(std::getline(f,line)){
      if(line.empty()||line[0]=='#')continue;
      std::istringstream ss(line); std::string nid,lib,name,src;
      std::getline(ss,nid,'\t'); std::getline(ss,lib,'\t'); std::getline(ss,name,'\t');
      nid_of[lib+"\t"+name]=nid;
    } }

  alignas(16) static uint8_t memregion[4*1024*1024];
  alignas(16) static uint8_t heapregion[16*1024*1024];
  GuestMemoryManager mem; mem.Init((uint64_t)memregion, sizeof(memregion));
  GuestHeap heap; heap.Init((uint64_t)heapregion, sizeof(heapregion));
  GuestBridge bridge;  // 本测试不执行 guest，仅供 pthread_create 注册引用

  size_t nk = RegisterKernelHle(wire, &mem, &bridge);
  size_t nl = RegisterLibcHle(wire, &heap);
  std::printf("注册: kernel=%zu libc=%zu\n", nk, nl);
  CHECK(nk >= 30);
  CHECK(nl >= 60);

  // 关键函数 NID 必须被 Resolve 命中（证明 RegisterByName 接上真实 NID）
  const char* must_libc[] = {"malloc","free","calloc","realloc","internal_memcpy",
    "internal_memset","internal_strlen","internal_strcmp","strdup","puts","internal_fopen","internal_fread",
    "sceLibcMspaceMalloc","_Znwm","_ZdlPv"};
  for(auto n : must_libc){
    auto it = nid_of.find(std::string("libc\t")+n);
    CHECK(it!=nid_of.end());
    if(it!=nid_of.end()) CHECK(wire.Resolve(it->second)!=nullptr);
  }
  const char* must_kern[] = {"sceKernelAllocateDirectMemory","sceKernelMapDirectMemory",
    "scePthreadMutexInit","posix_pthread_mutex_lock","posix_pthread_mutex_unlock",
    "scePthreadCondInit","posix_pthread_cond_wait","sceKernelCreateEqueue","sceKernelWaitEqueue",
    "posix_pthread_self","sceKernelReadTsc"};
  for(auto n : must_kern){
    auto it = nid_of.find(std::string("libkernel\t")+n);
    CHECK(it!=nid_of.end());
    if(it!=nid_of.end()) CHECK(wire.Resolve(it->second)!=nullptr);
  }

  // 通过绑定实际调用 malloc/memcpy/strlen（guest 指针=宿主缓冲地址）
  auto call = [&](const char* lib, const char* name, std::initializer_list<uint64_t> args)->uint64_t{
    auto it = nid_of.find(std::string(lib)+"\t"+name);
    if(it==nid_of.end()) return UINT64_MAX;
    const HleBinding* b = wire.Resolve(it->second);
    if(!b) return UINT64_MAX;
    std::vector<uint64_t> a(args); a.resize(14,0);
    HleReturn hr{};
    return b->fn(a.data(), (unsigned)a.size(), nullptr, &hr);
  };
  // FP 调用：传 fp_args，取 HleReturn.xmm0 的 double 位（FP 返回函数）。
  auto callFp = [&](const char* name, std::initializer_list<double> fp)->uint64_t{
    auto it = nid_of.find(std::string("libc\t")+name);
    if(it==nid_of.end()) return UINT64_MAX;
    const HleBinding* b = wire.Resolve(it->second);
    if(!b) return UINT64_MAX;
    std::vector<uint64_t> a(14,0); std::vector<double> f(fp); f.resize(8,0.0);
    HleReturn hr{};
    b->fn(a.data(), (unsigned)a.size(), f.data(), &hr);
    return hr.xmm0;
  };
  // FP 参但返回 int 于 RAX（分类函数 __isnan 等）。
  auto callFpRax = [&](const char* name, std::initializer_list<double> fp)->uint64_t{
    auto it = nid_of.find(std::string("libc\t")+name);
    if(it==nid_of.end()) return UINT64_MAX;
    const HleBinding* b = wire.Resolve(it->second);
    if(!b) return UINT64_MAX;
    std::vector<uint64_t> a(14,0); std::vector<double> f(fp); f.resize(8,0.0);
    HleReturn hr{};
    return b->fn(a.data(), (unsigned)a.size(), f.data(), &hr);
  };
  auto bits = [](double d){ uint64_t u; std::memcpy(&u,&d,8); return u; };
  uint64_t p = call("libc","malloc",{256});
  CHECK(p && p%16==0);
  uint64_t src = (uint64_t)"hello diva";
  call("libc","internal_memcpy",{p, src, 11});
  CHECK(std::strcmp((char*)p,"hello diva")==0);
  uint64_t len = call("libc","internal_strlen",{p});
  CHECK(len==10);
  call("libc","free",{p});
  CHECK(heap.used()==0);

  // mutex：init/lock/unlock 通过绑定
  alignas(16) static uint8_t mtx[64];
  uint64_t m = (uint64_t)mtx;
  CHECK(call("libkernel","scePthreadMutexInit",{m,0,0})==0);
  CHECK(call("libkernel","posix_pthread_mutex_lock",{m})==0);
  CHECK(call("libkernel","posix_pthread_mutex_unlock",{m})==0);

  // FP 数学：NID 解析 + 经绑定实调（fp_args→HleReturn.xmm0）
  const char* must_fp[] = {"internal_pow","internal_acos","sqrtf","internal_powf","fmod","__isnan","fabsf"};
  for(auto n : must_fp){
    auto it = nid_of.find(std::string("libc\t")+n);
    CHECK(it!=nid_of.end());
    if(it!=nid_of.end()) CHECK(wire.Resolve(it->second)!=nullptr);
  }
  CHECK(callFp("internal_pow",{2.0,10.0})==bits(1024.0));
  CHECK(callFp("fmod",{7.5,2.0})==bits(1.5));
  CHECK(callFp("internal_acos",{1.0})==bits(0.0));
  // sqrtf(9.0f)=3.0f：float 参取低 32 位，返回低 32 位
  { double f9=0; uint32_t v9=0x41100000; std::memcpy(&f9,&v9,4);  // 9.0f 位型放低32位
    uint64_t r=callFp("sqrtf",{f9}); uint32_t lo=(uint32_t)r; float ff; std::memcpy(&ff,&lo,4);
    CHECK(ff==3.0f); }
  { double nan=std::nan(""); uint64_t r=callFpRax("__isnan",{nan}); CHECK(r==1); }
  { double one=1.0; uint64_t r=callFpRax("__isnan",{one}); CHECK(r==0); }

  // printf 变参族：经 internal_snprintf 绑定实调（跨 ABI 逐项格式化）
  {
    auto it = nid_of.find(std::string("libc\tinternal_snprintf"));
    CHECK(it!=nid_of.end());
    const HleBinding* b = (it!=nid_of.end()) ? wire.Resolve(it->second) : nullptr;
    CHECK(b!=nullptr);
    if (b) {
      char buf[512];
      // snp(fmt, int_args..., fp_args...)：a[0]=buf,a[1]=size,a[2]=fmt,a[3..5]=整数参
      auto snp = [&](const char* fmt, std::initializer_list<uint64_t> ia,
                     std::initializer_list<double> fa)->std::string{
        std::vector<uint64_t> a(14,0);
        a[0]=(uint64_t)buf; a[1]=sizeof(buf); a[2]=(uint64_t)fmt;
        int k=3; for(auto v:ia){ if(k<14) a[k++]=v; }
        std::vector<double> f(fa); f.resize(8,0.0);
        HleReturn hr{};
        b->fn(a.data(),(unsigned)a.size(),f.data(),&hr);
        return std::string(buf);
      };
      CHECK(snp("%d",{42},{})=="42");
      CHECK(snp("%s",{(uint64_t)"hello"},{})=="hello");
      CHECK(snp("%d %s %x",{255,(uint64_t)"hi",255},{})=="255 hi ff");
      CHECK(snp("%f",{},{3.14})=="3.140000");
      CHECK(snp("%.2f",{},{3.14159})=="3.14");
      CHECK(snp("%d %f %d",{1,3},{2.5})=="1 2.500000 3");
      CHECK(snp("%5d|%-5d|",{42,42},{})=="   42|42   |");
      CHECK(snp("%%",{},{})=="%");
      CHECK(snp("%c",{65},{})=="A");
      CHECK(snp("%llx",{0x123456789ABCULL},{})=="123456789abc");
      CHECK(snp("%lld",{1234567890123ULL},{})=="1234567890123");
      CHECK(snp("no spec",{},{})=="no spec");
      CHECK(snp("%s=%d",{(uint64_t)"x",7},{})=="x=7");
    }
  }

  if(g_fail==0) std::printf("HLE_WIRE_TEST_PASS\n");
  else std::printf("HLE_WIRE_TEST_FAIL (%d)\n", g_fail);
  return g_fail?1:0;
}