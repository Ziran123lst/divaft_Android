// divaft_Android — DIVA 加载器实现（原创）
// SPDX-License-Identifier: GPL-2.0-or-later
#include "divaft/diva_loader.h"

#include <cstring>
#include <fstream>
#include <unordered_map>

namespace divaft {
namespace {

template <typename T>
T ReadLE(const uint8_t* p) {
  T v;
  std::memcpy(&v, p, sizeof(T));
  return v;  // 目标 ARM64 与源 x86-64 均小端
}

std::string CStr(const uint8_t* base, size_t off, size_t limit) {
  size_t end = off;
  while (end < limit && base[end] != 0) ++end;
  return std::string(reinterpret_cast<const char*>(base + off), end - off);
}

}  // namespace

bool DivaLoader::IsLoadable(const std::vector<uint8_t>& data, std::string* reason) {
  if (data.size() < 64) {
    *reason = "file too small";
    return false;
  }
  if (ReadLE<uint32_t>(data.data()) == 0x464C457F) return true;  // ELF
  if (ReadLE<uint32_t>(data.data()) != kSelfMagic) {
    *reason = "not SELF/ELF";
    return false;
  }
  uint8_t content_type = data[8];
  if (content_type == kPtNpdrmExec || content_type == 0x5) {
    *reason = "retail encrypted SELF (need decrypted dump)";
    return false;
  }
  // 校验各段未加密未压缩
  uint16_t entry_count = ReadLE<uint16_t>(data.data() + 24);
  for (int i = 0; i < entry_count; ++i) {
    size_t off = 0x20 + i * 32;
    if (off + 32 > data.size()) break;
    uint64_t props = ReadLE<uint64_t>(data.data() + off);
    if ((props >> 1) & 1) { *reason = "encrypted segment"; return false; }
    if ((props >> 3) & 1) { *reason = "compressed segment"; return false; }
  }
  return true;
}

bool DivaLoader::UnwrapSelf(const std::vector<uint8_t>& data, size_t* elf_offset, std::string* err) {
  if (ReadLE<uint32_t>(data.data()) == 0x464C457F) {
    *elf_offset = 0;
    return true;
  }
  // 内嵌 ELF 头紧随 SELF 头+条目表；扫描定位
  for (size_t off = 0x20; off + 4 <= data.size(); off += 16) {
    if (ReadLE<uint32_t>(data.data() + off) == 0x464C457F) {
      *elf_offset = off;
      return true;
    }
  }
  *err = "embedded ELF not found";
  return false;
}

bool DivaLoader::ParseElf(const std::vector<uint8_t>& data, size_t elf_offset,
                          uint64_t guest_base, LoadResult* out, std::string* err) {
  const uint8_t* e = data.data() + elf_offset;
  size_t remain = data.size() - elf_offset;
  if (remain < 64 || e[4] != 2) { *err = "not ELF64"; return false; }

  out->entry = ReadLE<uint64_t>(e + 24);
  uint64_t phoff = ReadLE<uint64_t>(e + 32);
  uint16_t phentsize = ReadLE<uint16_t>(e + 54);
  uint16_t phnum = ReadLE<uint16_t>(e + 56);

  for (int i = 0; i < phnum; ++i) {
    size_t off = phoff + i * phentsize;
    if (off + 56 > remain) break;
    Segment s;
    s.type = ReadLE<uint32_t>(e + off);
    s.flags = ReadLE<uint32_t>(e + off + 4);
    s.offset = ReadLE<uint64_t>(e + off + 8);
    s.vaddr = ReadLE<uint64_t>(e + off + 16);
    s.filesz = ReadLE<uint64_t>(e + off + 32);
    s.memsz = ReadLE<uint64_t>(e + off + 40);
    out->segments.push_back(s);
  }

  // 构建映像：PT_LOAD + PT_SCE_RELRO
  uint64_t max_va = 0;
  for (const auto& s : out->segments)
    if (s.type == kPtLoad || s.type == kPtSceRelro)
      if (s.vaddr + s.memsz > max_va) max_va = s.vaddr + s.memsz;
  out->image_size = max_va;
  out->image.assign(max_va, 0);
  for (const auto& s : out->segments) {
    if (s.type != kPtLoad && s.type != kPtSceRelro) continue;
    if (s.offset + s.filesz > remain || s.vaddr + s.filesz > max_va) {
      *err = "segment out of bounds";
      return false;
    }
    std::memcpy(out->image.data() + s.vaddr, e + s.offset, s.filesz);
  }
  return true;
}

bool DivaLoader::Relocate(const std::vector<uint8_t>& file_data, size_t elf_offset,
                          const std::vector<uint8_t>& blob, LoadResult* out, std::string* err) {
  // 定位 PT_DYNAMIC 与 PT_SCE_DYNLIBDATA（在 segments 中）
  const Segment* dyn = nullptr;
  const Segment* dl = nullptr;
  for (const auto& s : out->segments) {
    if (s.type == kPtDynamic) dyn = &s;
    if (s.type == kPtSceDynlibdata) dl = &s;
  }
  if (!dyn || !dl) { *err = "missing dynamic/dynlibdata"; return false; }

  // 动态段直接从文件数据读（PT_DYNAMIC 未必被 PT_LOAD 覆盖进 image）
  const uint8_t* dyn_base = file_data.data() + elf_offset + dyn->offset;
  std::unordered_map<uint64_t, std::vector<uint64_t>> tags;
  for (size_t off = 0; off + 16 <= dyn->filesz; off += 16) {
    uint64_t tag = ReadLE<uint64_t>(dyn_base + off);
    uint64_t val = ReadLE<uint64_t>(dyn_base + off + 8);
    if (tag == 0) break;
    tags[tag].push_back(val);
  }
  auto get = [&](uint64_t t) -> uint64_t { return tags.count(t) ? tags[t][0] : 0; };

  uint64_t strtab = get(kDtSceStrtab), strsz = get(kDtSceStrsz);
  uint64_t symtab = get(kDtSceSymtab), symsz = get(kDtSceSymtabsz);
  if (strtab + strsz > blob.size() || symtab + symsz > blob.size()) {
    *err = "strtab/symtab out of bounds";
    return false;
  }
  auto sym_name = [&](uint32_t no) { return CStr(blob.data(), strtab + no, strtab + strsz); };

  // 依赖模块
  for (uint64_t v : tags[kDtSceNeededModule]) {
    uint32_t name_off = static_cast<uint32_t>(v);
    out->needed_modules.push_back(CStr(blob.data(), strtab + name_off, strtab + strsz));
  }

  // 符号表（含 st_value，供导出表）
  struct Sym { std::string name; uint16_t shndx; uint64_t value; };
  std::vector<Sym> syms;
  for (size_t off = symtab; off + 24 <= symtab + symsz; off += 24) {
    uint32_t no = ReadLE<uint32_t>(blob.data() + off);
    uint16_t shndx = ReadLE<uint16_t>(blob.data() + off + 6);
    uint64_t value = ReadLE<uint64_t>(blob.data() + off + 8);
    syms.push_back({sym_name(no), shndx, value});
  }

  // 导出表：shndx != 0 的具名符号（NID#库#模块 或纯 NID），addr=st_value。
  for (const auto& s : syms) {
    if (s.shndx == 0 || s.name.empty()) continue;
    size_t h1 = s.name.find('#');
    std::string nid = (h1 == std::string::npos) ? s.name : s.name.substr(0, h1);
    if (nid.size() != 11) continue;  // 仅 NID 形态的导出
    out->exports.push_back({nid, s.value});
  }

  // 重定位
  auto do_relocs = [&](uint64_t toff, uint64_t tsz) {
    for (size_t off = toff; off + 24 <= toff + tsz; off += 24) {
      uint64_t target = ReadLE<uint64_t>(blob.data() + off);
      uint64_t info = ReadLE<uint64_t>(blob.data() + off + 8);
      uint32_t kind = info & 0xFFFFFFFF;
      uint32_t si = info >> 32;
      out->reloc_count++;
      if (kind == 8) {  // RELATIVE：记录 target+addend，映射时加基址
        uint64_t addend = ReadLE<uint64_t>(blob.data() + off + 16);
        out->relative_relocs.push_back({target, addend});
        continue;
      }
      if ((kind == 1 || kind == 6 || kind == 7) && si < syms.size() && syms[si].shndx == 0) {
        ImportSymbol im;
        std::string n = syms[si].name;
        size_t h1 = n.find('#'), h2 = n.find('#', h1 + 1);
        im.nid = n.substr(0, h1);
        if (h1 != std::string::npos) im.lib_code = n.substr(h1 + 1, h2 - h1 - 1);
        if (h2 != std::string::npos) im.mod_code = n.substr(h2 + 1);
        im.reloc_target = target;
        out->imports.push_back(im);
      }
    }
  };
  if (tags.count(kDtSceJmprel)) do_relocs(get(kDtSceJmprel), get(kDtScePltrelsz));
  if (tags.count(kDtSceRela)) do_relocs(get(kDtSceRela), get(kDtSceRelasz));
  return true;
}

LoadResult DivaLoader::Load(const std::string& path, uint64_t guest_base) {
  LoadResult res;
  std::ifstream f(path, std::ios::binary);
  if (!f) { res.error = "cannot open " + path; return res; }
  std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

  std::string reason;
  if (!IsLoadable(data, &reason)) { res.error = reason; return res; }

  size_t elf_offset = 0;
  if (!UnwrapSelf(data, &elf_offset, &res.error)) return res;
  if (!ParseElf(data, elf_offset, guest_base, &res, &res.error)) return res;

  // dynlibdata blob
  for (const auto& s : res.segments) {
    if (s.type == kPtSceDynlibdata) {
      std::vector<uint8_t> blob(data.data() + elf_offset + s.offset,
                                data.data() + elf_offset + s.offset + s.filesz);
      if (!Relocate(data, elf_offset, blob, &res, &res.error)) return res;
      break;
    }
  }
  res.ok = true;
  return res;
}

}  // namespace divaft
