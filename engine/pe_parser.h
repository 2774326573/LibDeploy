#pragma once
#include <string>
#include <vector>
#include <cstdint>

// 轻量级PE导入表解析器，无需第三方库
// 只读取IMAGE_IMPORT_DESCRIPTOR，返回该PE文件直接依赖的DLL名列表（不递归）
class PEParser {
public:
    // 成功返回true，imports填充dll名（原始大小写）
    static bool GetImports(const std::string& path,
                           std::vector<std::string>& imports,
                           std::string& error);

    // 返回 PE Machine 字段：0x8664=x64, 0x014c=x86, 0=解析失败
    static uint16_t GetMachineType(const std::string& path);
};
