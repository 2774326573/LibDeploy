#pragma once
#include "engine/dll_classifier.h"
#include <string>
#include <vector>

struct AppConfig {
    std::vector<std::string> search_paths;
    TargetOs    target_os          = TargetOs::Win10;
    bool        follow_system_path = true; // 启用系统 PATH 搜索，找到 System32 中的系统 DLL（如 ucrtbase.dll、api-ms-win-crt-*）
    bool        copy_redist_dlls   = true;
    bool        bundle_redist_installer = false;
    std::string last_target;
    // makensis_path: NSIS 工具路径。默认为空 ""，会自动查找内嵌版本 tools/nsis/Bin/makensis.exe;
    // 如需使用系统安装的 NSIS，设置为系统路径（如 "C:/Program Files (x86)/NSIS/makensis.exe"）
    std::string makensis_path = "";
    std::string redist_cache_dir;

    std::string ui_language = ""; // "" = auto-detect from system, "en_US", "zh_CN"
    std::string ui_theme = "system"; // "system", "light", "dark"

    struct RecentTarget {
        std::string path;
        std::vector<std::string> search_paths;
    };
    std::vector<RecentTarget> recent_targets; // recently analyzed exe paths + their search paths, max 10
    static constexpr int kMaxRecentTargets = 10;

    // User-defined extra classification rules
    std::vector<std::string> extra_os_core;
    struct RedistRule { std::string prefix; std::string package; bool always_deploy; };
    std::vector<RedistRule> extra_redist;
    std::vector<std::string> user_excluded; // 用户手动排除，不分析也不部署
    std::vector<std::string> extra_excluded_dirs; // 资源扫描时排除的目录名（小写）
    std::string qt_deploy_tool; // 空 = 自动查找 windeployqt / linuxdeployqt
};

class ConfigManager {
public:
    static const std::string kDefaultPath;

    static AppConfig Load(const std::string& path = kDefaultPath);
    static bool      Save(const AppConfig& cfg, const std::string& path = kDefaultPath);

    // Convert TargetOs <-> string for JSON
    static std::string  OsToString(TargetOs os);
    static TargetOs     OsFromString(const std::string& s);
};
