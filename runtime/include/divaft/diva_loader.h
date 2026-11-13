// divaft_Android — DIVA 加载器（原创实现）
// 解析 PS4 SELF/ELF（eboot.bin），构建 guest 内存映像，处理重定位。
// 解析逻辑基于对 CUSA08053 eboot 的实测分析（见 docs/diva-analysis.md）。
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <memory>

namespace divaft {

// PS4 SELF 魔数与内容类型
constexpr uint32_t kSelfMagic = 0x1D3D154F;
constexpr uint8_t kPtFake = 0x1;       // 伪签名（已解密）
constexpr uint8_t kPtNpdrmExec = 0x4;  // 零售加密（拒绝）

// ELF 程序头类型（PS4 扩展）
constexpr uint32_t kPtLoad = 1;
constexpr uint32_t kPtDynamic = 2;
constexpr uint32_t kPtSceRelro = 0x61000010;
constexpr uint32_t kPtSceDynlibdata = 0x61000000;

// SCE 动态标签
constexpr uint64_t kDtSceNeededModule = 0x6100000F;
constexpr uint64_t kDtSceImportLib = 0x61000015;
constexpr uint64_t kDtSceJmprel = 0x61000029;
constexpr uint64_t kDtScePltrelsz = 0x6100002D;
constexpr uint64_t kDtSceRela = 0x6100002F;
constexpr uint64_t kDtSceRelasz = 0x61000031;
constexpr uint64_t kDtSceStrtab = 0x61000035;
constexpr uint64_t kDtSceStrsz = 0x61000037;
constexpr uint64_t kDtSceSymtab = 0x61000039;
constexpr uint64_t kDtSceSymtabsz = 0x6100003F;

struct Segment {
  uint32_t type;
  uint32_t flags;
  uint64_t offset;   // 文件内偏移
  uint64_t vaddr;    // guest 虚拟地址
  uint64_t filesz;
  uint64_t memsz;
};

struct ImportSymbol {
  std::string nid;       // NID（base64 变体）
  std::string lib_code;  // 库编码
  std::string mod_code;  // 模块编码
  uint64_t reloc_target; // 重定位写入的 guest 地址
};

struct ExportSymbol {
  std::string nid;   // NID
  uint64_t addr;     // guest 虚拟地址（st_value，相对映像基址）
};

struct RelativeReloc {
  uint64_t target;   // 映像内偏移（写入位置）
  uint64_t addend;   // 需加基址的值
};

struct LoadResult {
  bool ok = false;
  std::string error;
  uint64_t entry = 0;
  uint64_t image_size = 0;
  std::vector<uint8_t> image;       // guest 内存映像
  std::vector<Segment> segments;
  std::vector<std::string> needed_modules;
  std::vector<ImportSymbol> imports;
  std::vector<ExportSymbol> exports;  // 导出表（多模块交叉接线用）
  std::vector<RelativeReloc> relative_relocs;  // RELATIVE 重定位（映射时加基址）
  size_t reloc_count = 0;
};

class DivaLoader {
public:
  // 从文件加载 eboot（SELF 或 ELF）。guest_base 为映像映射基址（低 4G）。
  LoadResult Load(const std::string& path, uint64_t guest_base = 0x10000);

  // 判断文件是否为已解密可加载的 SELF/ELF（拒绝零售加密）。
  static bool IsLoadable(const std::vector<uint8_t>& data, std::string* reason);

private:
  // SELF 剥离：定位内嵌 ELF，校验未加密未压缩。
  bool UnwrapSelf(const std::vector<uint8_t>& data, size_t* elf_offset, std::string* err);
  bool ParseElf(const std::vector<uint8_t>& data, size_t elf_offset,
                uint64_t guest_base, LoadResult* out, std::string* err);
  bool Relocate(const std::vector<uint8_t>& file_data, size_t elf_offset,
                const std::vector<uint8_t>& dynlibdata, LoadResult* out, std::string* err);
};

}  // namespace divaft
