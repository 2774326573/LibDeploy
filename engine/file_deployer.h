#pragma once
#include "engine/dep_result.h"
#include <string>
#include <vector>
#include <functional>

class FileDeployer {
public:
    using ProgressCallback = std::function<void(const std::string& msg, int pct)>;

    struct Options {
        bool copy_redist_dlls       = true;
        bool overwrite              = true;
        bool bundle_redist_installer = false; // true=捆绑 Redist DLL；false=安装后弹窗提示
    };

    // 将 DLL 复制到 dest_dir，返回成功复制数
    static int Deploy(const DepReport& report,
                      const std::string& dest_dir,
                      const Options& opts,
                      std::vector<std::string>& errors,
                      ProgressCallback on_progress = nullptr);

    // 将 exe + 所有依赖 DLL 打包成 ZIP，返回是否成功
    static bool PackZip(const DepReport& report,
                        const std::string& zip_path,
                        const Options& opts,
                        std::vector<std::string>& errors,
                        ProgressCallback on_progress = nullptr);

    // 生成 NSIS 脚本并运行 makensis，返回是否成功
    static bool GenInstaller(const DepReport& report,
                             const std::string& output_exe,   // 安装包输出路径
                             const std::string& app_name,
                             const std::string& makensis_path,
                             const Options& opts,
                             std::vector<std::string>& errors,
                             ProgressCallback on_progress = nullptr);

    // 收集所有需要打包的文件（exe + DLL），供外部预统计文件数量
    static std::vector<std::string> CollectFiles(const DepReport& report,
                                                  const Options& opts);
};
