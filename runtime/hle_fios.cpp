// divaft_Android — libSceFios2 HLE 实现（原创）
// 文件/目录/状态 → POSIX；异步 op 内联完成（假句柄 + 计数表 + guest 回调）。
// Archive mount 映射到游戏根目录（提取版 dump 无真实 psarc，参照 AstroQuest 做法）。
// SPDX-License-Identifier: GPL-2.0-or-later
#include "divaft/hle_fios.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cerrno>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/uio.h>

namespace divaft {

namespace {

// ---- 路径映射：guest 路径 → 宿主路径 ----
std::string g_root;                       // 游戏根（DIVA_GAME_ROOT 或 CWD）
std::vector<std::pair<std::string, std::string>> g_mounts;  // 挂载点前缀 → 宿主目录

std::string GameRoot() {
  if (!g_root.empty()) return g_root;
  if (const char* e = std::getenv("DIVA_GAME_ROOT")) g_root = e;
  else {
    char buf[4096];
    g_root = getcwd(buf, sizeof(buf)) ? buf : ".";
  }
  return g_root;
}

std::string MapPath(const char* p) {
  if (!p) return {};
  std::string s(p);
  if (s == "/app0") return GameRoot();
  if (s.rfind("/app0/", 0) == 0) return GameRoot() + s.substr(5);
  for (const auto& m : g_mounts) {
    if (s == m.first) return m.second;
    if (s.rfind(m.first + "/", 0) == 0)
      return m.second + s.substr(m.first.size());
  }
  return s;  // 相对路径按宿主 CWD；其余绝对路径直通
}

// ---- FH / DH / Op 表 ----
std::mutex g_mu;
std::unordered_map<int, int> g_fh;        // guest FH → host fd
std::unordered_map<int, DIR*> g_dh;       // guest DH → host DIR*
std::unordered_map<int, int64_t> g_ops;   // op 句柄 → actual_count
int g_next_fh = 1000, g_next_dh = 2000, g_next_op = 1;

// FiosOpAttr（0x30，布局经 AstroQuest 对照 Vita SDK 确认）
struct FiosOpAttr {
  int64_t deadline;          // 0x00
  uint64_t p_callback;       // 0x08 void(*)(ctx, op, err, result)
  uint64_t p_callback_ctx;   // 0x10
  uint32_t priority_flags;   // 0x18
  uint32_t user_tag;         // 0x1c
  uint64_t user_ptr;         // 0x20
  uint64_t p_reserved;       // 0x28
};

// FiosStat（布局照 AstroQuest）
struct FiosStat {
  int64_t size;
  uint64_t atime, mtime, ctime;
  uint32_t flags, reserved;
  int64_t uid, gid, dev, ino, mode;
};

GuestBridge* g_bridge = nullptr;

int64_t S64(uint64_t v) { return static_cast<int64_t>(v); }
uint64_t R(int64_t v) { return static_cast<uint64_t>(v); }

void FireCallback(const uint64_t* attr, int op, int err, int64_t result) {
  if (!attr || !attr[0]) return;
  auto* a = reinterpret_cast<const FiosOpAttr*>(attr[0]);
  if (!a->p_callback) return;
  std::fprintf(stderr, "[fios] op-callback cb=%llx op=%d err=%d result=%lld\n",
               (unsigned long long)a->p_callback, op, err, (long long)result);
  if (g_bridge) {
    uint64_t args[4] = {a->p_callback_ctx, (uint64_t)(int64_t)op,
                        (uint64_t)(int64_t)err, (uint64_t)result};
    g_bridge->CallGuest(a->p_callback, args, 4);
  }
}

// 异步 op 内联完成：记计数、发回调、返回假 op 句柄（失败返回负错误码）
int CompleteOp(const uint64_t* attr, int64_t count) {
  if (count < 0) {
    FireCallback(attr, (int)count, (int)count, 0);
    return (int)count;
  }
  int op;
  { std::lock_guard<std::mutex> lk(g_mu); op = g_next_op++; g_ops[op] = count; }
  FireCallback(attr, op, 0, count);
  return op;
}

int OpenFH(const char* path, int flags) {
  std::string hp = MapPath(path);
  int fd = open(hp.c_str(), flags, 0644);
  if (fd < 0) {
    std::fprintf(stderr, "[fios] FHOpen '%s'→'%s' 失败 errno=%d\n", path ? path : "?", hp.c_str(), errno);
    return -errno;
  }
  int h;
  { std::lock_guard<std::mutex> lk(g_mu); h = g_next_fh++; g_fh[h] = fd; }
  std::fprintf(stderr, "[fios] FHOpen '%s' fh=%d fd=%d\n", path ? path : "?", h, fd);
  return h;
}

int HostFd(int h) {
  std::lock_guard<std::mutex> lk(g_mu);
  auto it = g_fh.find(h);
  return it == g_fh.end() ? -1 : it->second;
}

void FillStat(FiosStat* out, const struct stat& st) {
  std::memset(out, 0, sizeof(*out));
  out->size = st.st_size;
  out->atime = (uint64_t)st.st_atime;
  out->mtime = (uint64_t)st.st_mtime;
  out->ctime = (uint64_t)st.st_ctime;
  out->mode = st.st_mode;
  out->ino = st.st_ino;
  out->dev = st.st_dev;
  out->uid = st.st_uid;
  out->gid = st.st_gid;
  out->flags = S_ISDIR(st.st_mode) ? 1u : 0u;  // 目录标志位（待真机日志校准）
}

}  // namespace

size_t RegisterFiosHle(HleWire& wire, GuestBridge* bridge) {
  g_bridge = bridge;
  size_t n = 0;
  auto reg = [&](const char* name, HostFn fn) {
    if (wire.RegisterByNameExt("libSceFios2", name, std::move(fn))) ++n;
    else std::fprintf(stderr, "[fios] 契约缺 %s\n", name);
  };

  reg("sceFiosInitialize", [](const uint64_t* a, unsigned, const double*, HleReturn*) -> uint64_t {
    std::fprintf(stderr, "[fios] Initialize params=%llx root='%s'\n",
                 (unsigned long long)a[0], GameRoot().c_str());
    return 0;
  });
  reg("sceFiosTerminate", [](const uint64_t*, unsigned, const double*, HleReturn*) -> uint64_t {
    std::lock_guard<std::mutex> lk(g_mu);
    for (auto& kv : g_fh) close(kv.second);
    for (auto& kv : g_dh) closedir(kv.second);
    g_fh.clear(); g_dh.clear(); g_ops.clear();
    return 0;
  });

  // ---- FH 同步 ----
  reg("sceFiosFHOpenSync", [](const uint64_t* a, unsigned, const double*, HleReturn*) -> uint64_t {
    int h = OpenFH(reinterpret_cast<const char*>(a[2]), O_RDONLY);
    if (h < 0) return R(h);
    if (a[1]) *reinterpret_cast<int32_t*>(a[1]) = h;
    return 0;
  });
  reg("sceFiosFHCloseSync", [](const uint64_t* a, unsigned, const double*, HleReturn*) -> uint64_t {
    int fd = HostFd((int)a[1]);
    if (fd < 0) return R(-EBADF);
    close(fd);
    std::lock_guard<std::mutex> lk(g_mu); g_fh.erase((int)a[1]);
    return 0;
  });
  reg("sceFiosFHReadSync", [](const uint64_t* a, unsigned, const double*, HleReturn*) -> uint64_t {
    int fd = HostFd((int)a[1]);
    if (fd < 0) return R(-EBADF);
    ssize_t r = read(fd, reinterpret_cast<void*>(a[2]), (size_t)S64(a[3]));
    return R(r < 0 ? -errno : r);
  });
  reg("sceFiosFHWriteSync", [](const uint64_t* a, unsigned, const double*, HleReturn*) -> uint64_t {
    int fd = HostFd((int)a[1]);
    if (fd < 0) return R(-EBADF);
    ssize_t r = write(fd, reinterpret_cast<const void*>(a[2]), (size_t)S64(a[3]));
    return R(r < 0 ? -errno : r);
  });
  reg("sceFiosFHWritevSync", [](const uint64_t* a, unsigned, const double*, HleReturn*) -> uint64_t {
    int fd = HostFd((int)a[1]);
    if (fd < 0) return R(-EBADF);
    // SceFiosIOVec = { void* data; s64 size; }
    struct GV { uint64_t p; int64_t n; };
    auto* gv = reinterpret_cast<const GV*>(a[2]);
    int cnt = (int)a[3];
    std::vector<struct iovec> iov(cnt);
    for (int i = 0; i < cnt; ++i) { iov[i].iov_base = reinterpret_cast<void*>(gv[i].p); iov[i].iov_len = (size_t)gv[i].n; }
    ssize_t r = writev(fd, iov.data(), cnt);
    return R(r < 0 ? -errno : r);
  });
  reg("sceFiosFHSeek", [](const uint64_t* a, unsigned, const double*, HleReturn*) -> uint64_t {
    int fd = HostFd((int)a[0]);
    if (fd < 0) return R(-EBADF);
    off_t r = lseek(fd, (off_t)S64(a[1]), (int)a[2]);
    return R(r < 0 ? -errno : r);
  });
  reg("sceFiosFHTell", [](const uint64_t* a, unsigned, const double*, HleReturn*) -> uint64_t {
    int fd = HostFd((int)a[0]);
    if (fd < 0) return R(-EBADF);
    off_t r = lseek(fd, 0, SEEK_CUR);
    return R(r < 0 ? -errno : r);
  });
  reg("sceFiosIsValidHandle", [](const uint64_t* a, unsigned, const double*, HleReturn*) -> uint64_t {
    return HostFd((int)a[0]) >= 0 ? 1 : 0;
  });

  // ---- 路径操作 ----
  reg("sceFiosStatSync", [](const uint64_t* a, unsigned, const double*, HleReturn*) -> uint64_t {
    std::string hp = MapPath(reinterpret_cast<const char*>(a[1]));
    struct stat st;
    if (stat(hp.c_str(), &st) != 0) return R(-errno);
    if (a[2]) FillStat(reinterpret_cast<FiosStat*>(a[2]), st);
    return 0;
  });
  reg("sceFiosExistsSync", [](const uint64_t* a, unsigned, const double*, HleReturn*) -> uint64_t {
    std::string hp = MapPath(reinterpret_cast<const char*>(a[1]));
    int ex = access(hp.c_str(), F_OK) == 0 ? 1 : 0;
    if (a[2]) *reinterpret_cast<uint32_t*>(a[2]) = (uint32_t)ex;
    return 0;
  });
  reg("sceFiosDeleteSync", [](const uint64_t* a, unsigned, const double*, HleReturn*) -> uint64_t {
    std::string hp = MapPath(reinterpret_cast<const char*>(a[1]));
    return remove(hp.c_str()) == 0 ? 0 : R(-errno);
  });
  reg("sceFiosDirectoryCreateSync", [](const uint64_t* a, unsigned, const double*, HleReturn*) -> uint64_t {
    std::string hp = MapPath(reinterpret_cast<const char*>(a[1]));
    if (mkdir(hp.c_str(), 0755) == 0 || errno == EEXIST) return 0;
    return R(-errno);
  });

  // ---- DH 目录枚举 ----
  auto dh_open = [](const uint64_t* a, bool async) -> uint64_t {
    std::string hp = MapPath(reinterpret_cast<const char*>(a[2]));
    DIR* d = opendir(hp.c_str());
    if (!d) {
      std::fprintf(stderr, "[fios] DHOpen '%s' 失败 errno=%d\n", hp.c_str(), errno);
      if (a[1]) *reinterpret_cast<int32_t*>(a[1]) = -1;
      return async ? R(CompleteOp(a, -errno)) : R(-errno);
    }
    int h;
    { std::lock_guard<std::mutex> lk(g_mu); h = g_next_dh++; g_dh[h] = d; }
    if (a[1]) *reinterpret_cast<int32_t*>(a[1]) = h;
    std::fprintf(stderr, "[fios] DHOpen '%s' dh=%d\n", hp.c_str(), h);
    return async ? R(CompleteOp(a, 0)) : 0;
  };
  reg("sceFiosDHOpenSync", [dh_open](const uint64_t* a, unsigned, const double*, HleReturn*) -> uint64_t { return dh_open(a, false); });
  reg("sceFiosDHOpen", [dh_open](const uint64_t* a, unsigned, const double*, HleReturn*) -> uint64_t { return dh_open(a, true); });
  reg("sceFiosDHReadSync", [](const uint64_t* a, unsigned, const double*, HleReturn*) -> uint64_t {
    DIR* d = nullptr;
    { std::lock_guard<std::mutex> lk(g_mu); auto it = g_dh.find((int)a[1]); if (it != g_dh.end()) d = it->second; }
    if (!d) return R(-EBADF);
    struct dirent* e;
    do { e = readdir(d); } while (e && (!std::strcmp(e->d_name, ".") || !std::strcmp(e->d_name, "..")));
    if (!e) return R(-1);  // 枚举结束
    // 条目布局：name[256] @0，type u32 @0x100（1=文件 2=目录；待真机日志校准）
    char* out = reinterpret_cast<char*>(a[2]);
    std::memset(out, 0, 0x108);
    std::snprintf(out, 256, "%s", e->d_name);
    uint32_t type = (e->d_type == DT_DIR) ? 2u : 1u;
    std::memcpy(out + 0x100, &type, 4);
    return 0;
  });
  reg("sceFiosDHCloseSync", [](const uint64_t* a, unsigned, const double*, HleReturn*) -> uint64_t {
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_dh.find((int)a[1]);
    if (it == g_dh.end()) return R(-EBADF);
    closedir(it->second); g_dh.erase(it);
    return 0;
  });

  // ---- Op（同步模型：全部内联完成）----
  reg("sceFiosOpWait", [](const uint64_t* a, unsigned, const double*, HleReturn*) -> uint64_t {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_ops.count((int)a[0]) ? 0 : R(-EINVAL);
  });
  reg("sceFiosOpGetActualCount", [](const uint64_t* a, unsigned, const double*, HleReturn*) -> uint64_t {
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_ops.find((int)a[0]);
    return it == g_ops.end() ? 0 : R(it->second);
  });
  reg("sceFiosOpDelete", [](const uint64_t* a, unsigned, const double*, HleReturn*) -> uint64_t {
    std::lock_guard<std::mutex> lk(g_mu);
    g_ops.erase((int)a[0]);
    return 0;
  });

  // ---- Archive / IOFilter（提取版 dump：挂载点映射游戏根，psarc 滤镜占位）----
  reg("sceFiosArchiveGetMountBufferSizeSync", [](const uint64_t*, unsigned, const double*, HleReturn*) -> uint64_t {
    return 0x100000;
  });
  reg("sceFiosArchiveMountSync", [](const uint64_t* a, unsigned, const double*, HleReturn*) -> uint64_t {
    const char* mp = reinterpret_cast<const char*>(a[3]);
    std::string guest_mp = (mp && mp[0]) ? mp : "/archive";
    g_mounts.emplace_back(guest_mp, GameRoot());
    if (a[1]) *reinterpret_cast<int32_t*>(a[1]) = 1;
    std::fprintf(stderr, "[fios] ArchiveMount '%s' → '%s'（提取版映射游戏根）\n",
                 guest_mp.c_str(), GameRoot().c_str());
    return 0;
  });
  reg("sceFiosArchiveUnmountSync", [](const uint64_t*, unsigned, const double*, HleReturn*) -> uint64_t { return 0; });
  reg("sceFiosIOFilterAdd", [](const uint64_t* a, unsigned, const double*, HleReturn*) -> uint64_t {
    std::fprintf(stderr, "[fios] IOFilterAdd index=%d filter=%llx\n", (int)a[0], (unsigned long long)a[1]);
    return 0;
  });
  reg("sceFiosIOFilterRemove", [](const uint64_t*, unsigned, const double*, HleReturn*) -> uint64_t { return 0; });
  reg("sceFiosIOFilterCache", [](const uint64_t*, unsigned, const double*, HleReturn*) -> uint64_t {
    static int dummy = 1;  // 非空滤镜描述符占位
    return reinterpret_cast<uint64_t>(&dummy);
  });
  reg("sceFiosIOFilterPsarcDearchiver", [](const uint64_t*, unsigned, const double*, HleReturn*) -> uint64_t {
    static int dummy = 1;  // 真 psarc 解压未实现；提取版 dump 不需要
    return reinterpret_cast<uint64_t>(&dummy);
  });

  std::fprintf(stderr, "[fios] libSceFios2 注册 %zu/28\n", n);
  return n;
}

}  // namespace divaft
