#include "engine/pe_parser.h"
#include <fstream>
#include <filesystem>
#include <cstdint>
#include <cstring>
#include <algorithm>

namespace fs = std::filesystem;

// ── PE结构定义（避免依赖<windows.h>，便于跨平台编译）────────────────────────
static const uint16_t IMAGE_DOS_SIGNATURE = 0x5A4D; // "MZ"
static const uint32_t IMAGE_NT_SIGNATURE  = 0x00004550; // "PE\0\0"

#pragma pack(push, 1)
struct DosHeader {
    uint16_t e_magic;
    uint8_t  _pad[58];
    uint32_t e_lfanew;
};

struct FileHeader {
    uint16_t Machine;
    uint16_t NumberOfSections;
    uint32_t TimeDateStamp;
    uint32_t PointerToSymbolTable;
    uint32_t NumberOfSymbols;
    uint16_t SizeOfOptionalHeader;
    uint16_t Characteristics;
};

struct DataDirectory {
    uint32_t VirtualAddress;
    uint32_t Size;
};

// IMAGE_OPTIONAL_HEADER32 = 224 bytes total
//   Magic(2) + fields_before_DataDir(94) + DataDirectory[16](128) = 224
struct OptionalHeader32 {
    uint16_t Magic;
    uint8_t  _pad[94];
    DataDirectory DataDirectory[16];
};

// IMAGE_OPTIONAL_HEADER64 = 240 bytes total
//   Magic(2) + fields_before_DataDir(110) + DataDirectory[16](128) = 240
struct OptionalHeader64 {
    uint16_t Magic;
    uint8_t  _pad[110];
    DataDirectory DataDirectory[16];
};

struct SectionHeader {
    char     Name[8];
    uint32_t VirtualSize;
    uint32_t VirtualAddress;
    uint32_t SizeOfRawData;
    uint32_t PointerToRawData;
    uint8_t  _pad[16];
};

struct ImportDescriptor {
    uint32_t OriginalFirstThunk;
    uint32_t TimeDateStamp;
    uint32_t ForwarderChain;
    uint32_t Name;
    uint32_t FirstThunk;
};

// IMAGE_DELAY_IMPORT_DESCRIPTOR（现代 RVA 形式，grAttrs bit0 = 1）
struct DelayImportDescriptor {
    uint32_t Attributes;       // bit0=1 → 所有字段均为 RVA
    uint32_t DllNameRVA;       // DLL 名字符串的 RVA
    uint32_t ModuleHandleRVA;
    uint32_t IATRVA;
    uint32_t INTRVA;
    uint32_t BoundIATRVA;
    uint32_t UnloadIATRVA;
    uint32_t TimeDateStamp;
};
#pragma pack(pop)

// RVA → file offset
// Uses max(VirtualSize, SizeOfRawData) to handle PEs where VirtualSize < SizeOfRawData.
static uint32_t RvaToOffset(uint32_t rva,
                            const std::vector<SectionHeader>& sections)
{
    for (const auto& sec : sections) {
        uint32_t mapped_size = std::max(sec.VirtualSize, sec.SizeOfRawData);
        if (rva >= sec.VirtualAddress &&
            rva <  sec.VirtualAddress + mapped_size)
        {
            return (rva - sec.VirtualAddress) + sec.PointerToRawData;
        }
    }
    return 0;
}

// ── ELF Import Parser ─────────────────────────────────────────────────────────
// 解析 .dynamic 节的 DT_NEEDED 条目，支持 ELF32/64 LE。
// 使用 PT_LOAD 段建立 VA→文件偏移映射，避免依赖节头表（stripped binary 兼容）。
static bool GetImportsELF(std::ifstream& f,
                           std::vector<std::string>& imports,
                           std::string& error)
{
    f.seekg(0);
    uint8_t ident[16];
    f.read(reinterpret_cast<char*>(ident), 16);
    if (!f) { error = "ELF: cannot read ident"; return false; }

    bool is64 = (ident[4] == 2);
    bool isLE = (ident[5] == 1);
    if (!isLE) { error = "ELF: big-endian not supported"; return true; }

    auto r16 = [&]() -> uint16_t {
        uint8_t b[2]; f.read(reinterpret_cast<char*>(b), 2);
        return static_cast<uint16_t>(b[0] | (b[1] << 8));
    };
    auto r32 = [&]() -> uint32_t {
        uint8_t b[4]; f.read(reinterpret_cast<char*>(b), 4);
        return static_cast<uint32_t>(b[0] | (b[1]<<8) | (b[2]<<16) | (b[3]<<24));
    };
    auto r64 = [&]() -> uint64_t {
        uint64_t lo = r32(), hi = r32();
        return lo | (hi << 32);
    };

    // ELF header 字段（ident 之后）
    r16(); r16(); r32(); // e_type, e_machine, e_version

    uint64_t e_phoff;
    uint16_t e_phentsize, e_phnum;
    if (is64) {
        r64();            // e_entry
        e_phoff = r64();
        r64();            // e_shoff
        r32();            // e_flags
        r16();            // e_ehsize
        e_phentsize = r16();
        e_phnum     = r16();
    } else {
        r32();            // e_entry
        e_phoff     = r32();
        r32();            // e_shoff
        r32();            // e_flags
        r16();            // e_ehsize
        e_phentsize = r16();
        e_phnum     = r16();
    }
    if (!f) { error = "ELF: header truncated"; return false; }

    // 读取 program headers，构建 VA→文件偏移映射，定位 PT_DYNAMIC
    struct LoadSeg { uint64_t vaddr, filesz, file_off; };
    std::vector<LoadSeg> loads;
    uint64_t dyn_off = 0, dyn_sz = 0;

    for (uint16_t i = 0; i < e_phnum; ++i) {
        f.seekg(static_cast<std::streamoff>(e_phoff + static_cast<uint64_t>(i) * e_phentsize));
        if (!f) break;
        uint32_t p_type = r32();
        uint64_t p_offset, p_vaddr, p_filesz;
        if (is64) {
            r32();           // p_flags
            p_offset = r64();
            p_vaddr  = r64();
            r64();           // p_paddr
            p_filesz = r64();
        } else {
            p_offset = r32();
            p_vaddr  = r32();
            r32();           // p_paddr
            p_filesz = r32();
        }
        if (!f) break;
        if (p_type == 1 && p_filesz > 0) loads.push_back({p_vaddr, p_filesz, p_offset});
        if (p_type == 2) { dyn_off = p_offset; dyn_sz = p_filesz; } // PT_DYNAMIC
    }
    if (dyn_off == 0) return true; // 静态链接，无动态节

    auto va2off = [&](uint64_t va) -> uint64_t {
        for (const auto& s : loads)
            if (va >= s.vaddr && va < s.vaddr + s.filesz)
                return s.file_off + (va - s.vaddr);
        return 0;
    };

    // 遍历动态节：收集 DT_STRTAB 和 DT_NEEDED
    const uint64_t entry_sz = is64 ? 16 : 8;
    uint64_t strtab_va = 0, strtab_sz = 0;
    std::vector<uint64_t> needed;

    f.seekg(static_cast<std::streamoff>(dyn_off));
    for (uint64_t pos = 0; pos + entry_sz <= dyn_sz; pos += entry_sz) {
        int64_t  d_tag;
        uint64_t d_val;
        if (is64) { d_tag = static_cast<int64_t>(r64()); d_val = r64(); }
        else       { d_tag = static_cast<int32_t>(r32()); d_val = r32(); }
        if (!f) break;
        if (d_tag == 0)  break;              // DT_NULL
        if (d_tag == 1)  needed.push_back(d_val); // DT_NEEDED
        if (d_tag == 5)  strtab_va = d_val;  // DT_STRTAB
        if (d_tag == 10) strtab_sz = d_val;  // DT_STRSZ
    }
    if (strtab_va == 0 || needed.empty()) return true;

    uint64_t strtab_off = va2off(strtab_va);
    if (strtab_off == 0) { error = "ELF: cannot map DT_STRTAB VA to file offset"; return false; }

    for (uint64_t idx : needed) {
        if (strtab_sz > 0 && idx >= strtab_sz) continue;
        f.seekg(static_cast<std::streamoff>(strtab_off + idx));
        std::string name;
        std::getline(f, name, '\0');
        if (!name.empty()) imports.push_back(name);
    }
    return true;
}

// ── Mach-O Import Parser ──────────────────────────────────────────────────────
// 解析 LC_LOAD_DYLIB / LC_LOAD_WEAK_DYLIB / LC_REEXPORT_DYLIB load commands。
// 支持 32/64 位、大小端、fat binary（选取第一个 slice）。
static bool GetImportsMachO(std::ifstream& f,
                             std::vector<std::string>& imports,
                             std::string& error)
{
    f.seekg(0);
    uint8_t raw[4]; f.read(reinterpret_cast<char*>(raw), 4);
    if (!f) { error = "MachO: cannot read magic"; return false; }

    // 按 LE 解读 magic
    uint32_t magic = static_cast<uint32_t>(
        raw[0] | (raw[1]<<8) | (raw[2]<<16) | (raw[3]<<24));

    static const uint32_t MH_MAGIC    = 0xFEEDFACEu; // 32-bit BE
    static const uint32_t MH_MAGIC_64 = 0xFEEDFACFu; // 64-bit BE
    static const uint32_t MH_CIGAM    = 0xCEFAEDFEu; // 32-bit LE
    static const uint32_t MH_CIGAM_64 = 0xCFFAEDFEu; // 64-bit LE
    static const uint32_t FAT_MAGIC   = 0xCAFEBABEu;
    static const uint32_t FAT_CIGAM   = 0xBEBAFECAu;

    // Fat binary → 取第一个 arch slice
    if (magic == FAT_MAGIC || magic == FAT_CIGAM) {
        bool fat_be = (magic == FAT_MAGIC);
        auto r32fat = [&]() -> uint32_t {
            uint8_t b[4]; f.read(reinterpret_cast<char*>(b), 4);
            return fat_be
                ? static_cast<uint32_t>(b[0]<<24 | b[1]<<16 | b[2]<<8 | b[3])
                : static_cast<uint32_t>(b[0] | b[1]<<8 | b[2]<<16 | b[3]<<24);
        };
        uint32_t narch = r32fat();
        if (!f || narch == 0) { error = "MachO: invalid fat binary"; return false; }
        r32fat(); r32fat();                    // cputype, cpusubtype
        uint32_t arch_offset = r32fat();       // offset of first slice
        if (!f) { error = "MachO: truncated fat arch"; return false; }
        f.seekg(static_cast<std::streamoff>(arch_offset));
        f.read(reinterpret_cast<char*>(raw), 4);
        if (!f) { error = "MachO: cannot read slice magic"; return false; }
        magic = static_cast<uint32_t>(
            raw[0] | (raw[1]<<8) | (raw[2]<<16) | (raw[3]<<24));
    }

    bool is64, isLE;
    if      (magic == MH_CIGAM)    { is64 = false; isLE = true;  }
    else if (magic == MH_CIGAM_64) { is64 = true;  isLE = true;  }
    else if (magic == MH_MAGIC)    { is64 = false; isLE = false; }
    else if (magic == MH_MAGIC_64) { is64 = true;  isLE = false; }
    else { error = "MachO: unknown magic"; return false; }

    auto r32m = [&]() -> uint32_t {
        uint8_t b[4]; f.read(reinterpret_cast<char*>(b), 4);
        return isLE
            ? static_cast<uint32_t>(b[0] | b[1]<<8 | b[2]<<16 | b[3]<<24)
            : static_cast<uint32_t>(b[0]<<24 | b[1]<<16 | b[2]<<8 | b[3]);
    };

    // Mach-O 头部 (magic 已读) cputype, cpusubtype, filetype, ncmds, sizeofcmds, flags, [reserved 64]
    r32m(); r32m(); r32m();       // cputype, cpusubtype, filetype
    uint32_t ncmds = r32m();
    r32m(); r32m();               // sizeofcmds, flags
    if (is64) r32m();             // reserved
    if (!f) { error = "MachO: truncated header"; return false; }

    static const uint32_t LC_LOAD_DYLIB      = 0x0000000Cu;
    static const uint32_t LC_LOAD_WEAK_DYLIB = 0x80000018u;
    static const uint32_t LC_REEXPORT_DYLIB  = 0x8000001Fu;

    for (uint32_t i = 0; i < ncmds; ++i) {
        auto cmd_pos = f.tellg();
        uint32_t cmd     = r32m();
        uint32_t cmdsize = r32m();
        if (!f || cmdsize < 8) break;

        if (cmd == LC_LOAD_DYLIB || cmd == LC_LOAD_WEAK_DYLIB || cmd == LC_REEXPORT_DYLIB) {
            uint32_t name_off = r32m(); // offset from start of load command
            if (f && name_off >= 8 && name_off < cmdsize) {
                f.seekg(cmd_pos + static_cast<std::streamoff>(name_off));
                std::string full_path;
                std::getline(f, full_path, '\0');
                if (!full_path.empty()) {
                    // 框架路径：*.framework/... → 提取 Foo.framework
                    std::string name;
                    auto fw = full_path.find(".framework");
                    if (fw != std::string::npos) {
                        auto start = full_path.rfind('/', fw);
                        start = (start == std::string::npos) ? 0 : start + 1;
                        name = full_path.substr(start, fw - start + 10);
                    } else {
                        auto slash = full_path.rfind('/');
                        name = (slash != std::string::npos)
                               ? full_path.substr(slash + 1)
                               : full_path;
                    }
                    if (!name.empty()) imports.push_back(name);
                }
            }
        }
        f.seekg(cmd_pos + static_cast<std::streamoff>(cmdsize));
        if (!f) break;
    }
    return true;
}

// ── GetMachineType ─────────────────────────────────────────────────────────────
uint16_t PEParser::GetMachineType(const std::string& path)
{
    std::ifstream f(fs::path(path), std::ios::binary);
    if (!f) return 0;
    DosHeader dos{};
    f.read(reinterpret_cast<char*>(&dos), sizeof(dos));
    if (!f || dos.e_magic != IMAGE_DOS_SIGNATURE) return 0;
    f.seekg(dos.e_lfanew);
    uint32_t sig = 0;
    f.read(reinterpret_cast<char*>(&sig), 4);
    if (!f || sig != IMAGE_NT_SIGNATURE) return 0;
    FileHeader fh{};
    f.read(reinterpret_cast<char*>(&fh), sizeof(fh));
    return fh.Machine;
}

bool PEParser::GetImports(const std::string& path,
                          std::vector<std::string>& imports,
                          std::string& error)
{
    imports.clear();
    std::ifstream f(fs::path(path), std::ios::binary);
    if (!f) { error = "Cannot open: " + path; return false; }

    // DOS header — 先读 4 字节判断格式，再决定走哪条解析路径
    uint8_t magic4[4] = {};
    f.read(reinterpret_cast<char*>(magic4), 4);
    if (!f) { error = "Cannot read file header: " + path; return false; }

    // ELF: 0x7f 'E' 'L' 'F'
    if (magic4[0] == 0x7f && magic4[1] == 'E' && magic4[2] == 'L' && magic4[3] == 'F')
        return GetImportsELF(f, imports, error);

    // Mach-O / fat binary
    {
        uint32_t m32 = static_cast<uint32_t>(
            magic4[0] | (magic4[1]<<8) | (magic4[2]<<16) | (magic4[3]<<24));
        static const uint32_t kMachMagics[] = {
            0xFEEDFACEu, 0xFEEDFACFu, 0xCEFAEDFEu, 0xCFFAEDFEu, 0xCAFEBABEu, 0xBEBAFECAu
        };
        for (auto mm : kMachMagics) {
            if (m32 == mm) return GetImportsMachO(f, imports, error);
        }
    }

    // PE: 'M' 'Z'
    if (magic4[0] != 'M' || magic4[1] != 'Z') {
        error = "Not a valid PE/ELF/Mach-O file: " + path;
        return false;
    }
    f.seekg(0);

    DosHeader dos{};
    f.read(reinterpret_cast<char*>(&dos), sizeof(dos));
    if (!f || dos.e_magic != IMAGE_DOS_SIGNATURE) {
        error = "Not a valid PE file (bad MZ header)";
        return false;
    }

    // PE signature
    f.seekg(dos.e_lfanew);
    uint32_t sig = 0;
    f.read(reinterpret_cast<char*>(&sig), 4);
    if (!f || sig != IMAGE_NT_SIGNATURE) {
        error = "Not a valid PE file (bad PE signature)";
        return false;
    }

    // File header
    FileHeader fh{};
    f.read(reinterpret_cast<char*>(&fh), sizeof(fh));

    // Optional header – read Magic only first
    uint16_t magic = 0;
    f.read(reinterpret_cast<char*>(&magic), 2);
    f.seekg(-2, std::ios::cur);

    DataDirectory importDir{};
    DataDirectory delayDir{};
    if (magic == 0x010B) { // PE32
        OptionalHeader32 oh{};
        f.read(reinterpret_cast<char*>(&oh), sizeof(oh));
        importDir = oh.DataDirectory[1];
        delayDir  = oh.DataDirectory[13]; // IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT
    } else if (magic == 0x020B) { // PE32+
        OptionalHeader64 oh{};
        f.read(reinterpret_cast<char*>(&oh), sizeof(oh));
        importDir = oh.DataDirectory[1];
        delayDir  = oh.DataDirectory[13];
    } else {
        error = "Unknown optional header magic: " + std::to_string(magic);
        return false;
    }

    if (importDir.VirtualAddress == 0 && delayDir.VirtualAddress == 0)
        return true; // no imports at all (e.g. pure resource DLL)

    // Section headers
    std::vector<SectionHeader> sections(fh.NumberOfSections);
    for (auto& s : sections)
        f.read(reinterpret_cast<char*>(&s), sizeof(s));

    // ── 普通 Import Table（DataDirectory[1]）──────────────────────────────────
    if (importDir.VirtualAddress != 0 && importDir.Size != 0) {
        uint32_t offset = RvaToOffset(importDir.VirtualAddress, sections);
        if (offset == 0) { error = "Cannot resolve import directory RVA"; return false; }

        f.seekg(offset);
        ImportDescriptor desc{};
        while (true) {
            f.read(reinterpret_cast<char*>(&desc), sizeof(desc));
            if (!f || desc.Name == 0) break;

            uint32_t nameOffset = RvaToOffset(desc.Name, sections);
            if (nameOffset == 0) continue;

            auto saved = f.tellg();
            f.seekg(nameOffset);
            std::string dllName;
            std::getline(f, dllName, '\0');
            f.seekg(saved);

            if (!dllName.empty())
                imports.push_back(dllName);
        }
    }

    // ── Delay Import Table（DataDirectory[13]）────────────────────────────────
    // 检测 delay-load DLL（/DELAYLOAD 链接选项）——同样需要部署
    if (delayDir.VirtualAddress != 0 && delayDir.Size != 0) {
        uint32_t offset = RvaToOffset(delayDir.VirtualAddress, sections);
        if (offset != 0) {
            f.seekg(offset);
            DelayImportDescriptor ddesc{};
            while (true) {
                f.read(reinterpret_cast<char*>(&ddesc), sizeof(ddesc));
                if (!f || ddesc.DllNameRVA == 0) break;
                // 只处理现代 RVA 形式（bit0 = 1）
                if (!(ddesc.Attributes & 1)) continue;

                uint32_t nameOffset = RvaToOffset(ddesc.DllNameRVA, sections);
                if (nameOffset == 0) continue;

                auto saved = f.tellg();
                f.seekg(nameOffset);
                std::string dllName;
                std::getline(f, dllName, '\0');
                f.seekg(saved);

                if (!dllName.empty())
                    imports.push_back(dllName);
            }
        }
    }

    return true;
}
