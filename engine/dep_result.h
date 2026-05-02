#pragma once
#include <string>
#include <vector>
#include <memory>

enum class DllCategory {
    OsCore,      // Windows OS核心，目标机必然存在
    Redist,      // 可再发行运行时（VC++/UCRT/DirectX）
    ThirdParty,  // 第三方库（Qt/wx/用户自己的dll）
    ApiSet,      // api-ms-win-* 虚拟转发dll
    Unknown,
};

enum class DepStatus {
    Found,         // 在搜索路径中已找到
    SystemPresent, // OsCore，目标机必然存在，跳过
    MaybeAbsent,   // Redist/ApiSet，本机有但目标机可能没有
    Missing,       // 所有路径均未找到
    UserExcluded,  // 用户手动排除
};

struct DepNode {
    std::string name;            // dll文件名（小写）
    std::string resolved_path;   // 找到时的完整路径
    DllCategory category  = DllCategory::Unknown;
    DepStatus   status    = DepStatus::Missing;
    std::string redist_package;  // Redist时填写来源，如 "VC++ 2022 x64"
    bool        need_deploy = false;

    std::vector<std::shared_ptr<DepNode>> children;
};

struct RedistRequirement {
    std::string name;    // "Visual C++ 2022 Redistributable (x64)"
    std::string url;     // 微软官方下载链接
};

// 需要随 exe 一并部署的资源目录（locale/, plugins/, scripts/ 等）
struct ResourceDir {
    std::string src_path;  // 源目录完整路径
    std::string dir_name;  // 目录名（用于放置到目标的相对路径）
};

struct DepReport {
    std::string                      target_exe;
    std::shared_ptr<DepNode>         root;
    std::vector<DepNode*>            missing;       // 完全找不到
    std::vector<DepNode*>            maybe_absent;  // Redist，可能缺失
    std::vector<DepNode*>            to_copy;       // 需要复制到发布目录
    std::vector<RedistRequirement>   redist_needed;
    std::vector<ResourceDir>         resource_dirs; // 需要部署的资源子目录
    std::vector<std::string>         warnings;      // 兼容性/部署风险提示
    // exe 目录内的数据文件（非 DLL/EXE，但需随包部署）
    // 如：python38.zip, python38._pth, nodeflow.db, nodeflow.lic, *.def 等
    std::vector<std::string>         data_files;

    // 持有所有 DepNode 的 shared_ptr，保证 missing/to_copy 等裸指针在
    // DepReport 析构前始终有效（Resolve() 返回后 visited map 已销毁）
    std::vector<std::shared_ptr<DepNode>> node_pool;
};
