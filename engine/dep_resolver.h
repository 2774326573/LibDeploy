#pragma once
#include "engine/dep_result.h"
#include "engine/dll_classifier.h"
#include <string>
#include <vector>
#include <functional>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>

class DepResolver {
public:
    using ProgressCallback = std::function<void(const std::string& msg)>;

    // search_paths: 用户手动添加的路径（不含系统PATH）
    // resource_scan_paths: 同时扫描资源子目录的路径（通常同上）
    DepResolver(TargetOs target_os,
                const std::vector<std::string>& search_paths,
                bool follow_system_path,
                ProgressCallback on_progress = nullptr,
                const std::vector<std::string>& resource_scan_paths = {});

    DepReport Resolve(const std::string& target_exe);

    void AddOsCore(const std::string& dll_name_lower) {
        m_classifier.AddOsCore(dll_name_lower);
    }
    void AddRedistRule(const std::string& prefix,
                       const std::string& package,
                       bool always_deploy) {
        m_classifier.AddRedistRule(prefix, package, always_deploy);
    }
    void SetUserExcluded(const std::vector<std::string>& names) {
        m_user_excluded.insert(names.begin(), names.end());
    }
    void SetExtraExcludedDirs(const std::vector<std::string>& dirs) {
        m_extra_excluded_dirs.insert(dirs.begin(), dirs.end());
    }

    // 显式指定 windeployqt / linuxdeployqt 路径（空串 = 自动查找）
    void SetQtDeployTool(const std::string& tool_path) {
        m_qt_deploy_tool = tool_path;
    }
    // 启用：分析完成后自动运行 windeployqt/linuxdeployqt
    void SetRunQtDeployTool(bool enabled) {
        m_run_qt_deploy = enabled;
    }

private:
    DllClassifier              m_classifier;
    std::vector<std::string>   m_search_paths;
    std::vector<std::string>   m_resource_scan_paths;
    std::unordered_set<std::string> m_user_excluded;
    std::unordered_set<std::string> m_extra_excluded_dirs;
    bool                       m_follow_system;
    bool                       m_run_qt_deploy = false;
    std::string                m_qt_deploy_tool;       // 空 = 自动查找
    ProgressCallback           m_progress;

    // 全盘 DLL 索引：文件名小写 → 完整路径列表
    // 在 Resolve() 首次调用时懒建立，扫描 Program Files 等常用安装目录
    mutable std::unordered_map<std::string, std::vector<std::string>> m_dll_index;
    mutable bool m_dll_index_built = false;

    // Returns empty string if not found
    std::string FindDll(const std::string& dll_name) const;

    // 扫描系统常用安装目录，建立 DLL 文件名索引（懒初始化）
    void EnsureDllIndex() const;

    std::string FindDllInIndex(const std::string& lower_name,
                               uint16_t target_machine) const;

    // Qt 插件目录自动发现（Resolve() 后调用，检测 Qt5Core/Qt6Core）
    void DiscoverQtPlugins(DepReport& report) const;
    // 运行 windeployqt / linuxdeployqt，合并输出到 report
    void RunQtDeployTool(const std::string& target_exe, DepReport& report) const;
    // 检查 WebView2 fixed runtime 对 Win7/8.1 的兼容性
    void CheckWebView2Compatibility(DepReport& report) const;

    void Log(const std::string& msg) const {
        if (m_progress) m_progress(msg);
    }
};
