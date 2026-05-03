#include "engine/file_deployer.h"
#include "third_party/minizip/zip.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <cstdio>
#include <unordered_set>
#include <cctype>
#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

namespace fs = std::filesystem;

static std::string ToLower(std::string s)
{
    for (auto& c : s) c = static_cast<char>(::tolower((unsigned char)c));
    return s;
}

static bool IsWindowsSystemPath(const std::string& path)
{
    const char* sysroot = std::getenv("SystemRoot");
    std::string win_dir = sysroot ? sysroot : "C:\\Windows";
    std::string p = ToLower(path);
    std::string w = ToLower(win_dir);
    for (auto& c : p) if (c == '/') c = '\\';
    for (auto& c : w) if (c == '/') c = '\\';
    return p.size() >= w.size() && p.substr(0, w.size()) == w;
}

static bool SameFileOrPath(const fs::path& a, const fs::path& b)
{
    std::error_code ec;
    if (fs::exists(a, ec) && fs::exists(b, ec) && fs::equivalent(a, b, ec))
        return true;

    fs::path aa = a.lexically_normal();
    fs::path bb = b.lexically_normal();
    return ToLower(aa.string()) == ToLower(bb.string());
}

static bool IsGeneratedPackageArtifact(const fs::path& file,
                                       const DepReport& report,
                                       const fs::path& current_output)
{
    if (SameFileOrPath(file, current_output))
        return true;

    fs::path target = report.target_exe;
    std::error_code ec;
    if (!target.empty() && fs::exists(file.parent_path(), ec) &&
        fs::exists(target.parent_path(), ec) &&
        fs::equivalent(file.parent_path(), target.parent_path(), ec))
    {
        std::string stem = ToLower(target.stem().string());
        std::string name = ToLower(file.filename().string());
        std::string ext = ToLower(file.extension().string());
        if (name == stem + ".zip" || name == stem + "_setup.exe")
            return true;
        if (ext == ".zip" && name.rfind(stem + "_", 0) == 0)
            return true;
        if (ext == ".exe" && name.rfind(stem + "_setup_", 0) == 0)
            return true;
    }

    return false;
}

#ifdef _WIN32
// 直接用 CreateProcessW 运行子进程，捕获 stdout+stderr，避免 cmd.exe 经由的引号问题
static int RunProcessCapture(const std::wstring& exe_w,
                             const std::wstring& arg_w,
                             std::string& output,
                             FileDeployer::ProgressCallback on_progress)
{
    // 命令行：makensis.exe 直接接收参数，无需 cmd.exe 中转
    std::wstring cmdline = L"\"" + exe_w + L"\" \"" + arg_w + L"\"";

    SECURITY_ATTRIBUTES sa{};
    sa.nLength        = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE hRead = nullptr, hWrite = nullptr;
    if (!CreatePipe(&hRead, &hWrite, &sa, 0)) return -1;
    // 读端不让子进程继承
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb          = sizeof(si);
    si.dwFlags     = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput  = hWrite;
    si.hStdError   = hWrite;           // stderr 也重定向到同一管道
    si.hStdInput   = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> cmd(cmdline.begin(), cmdline.end());
    cmd.push_back(0);

    BOOL ok = CreateProcessW(
        nullptr, cmd.data(),
        nullptr, nullptr,
        TRUE,                   // bInheritHandles
        CREATE_NO_WINDOW,       // 不弹出控制台窗口
        nullptr,                // 继承父进程环境（含 NSISDIR）
        nullptr,                // 继承当前目录
        &si, &pi);

    CloseHandle(hWrite);        // 父进程必须关闭写端，读端才能得到 EOF
    if (!ok) { CloseHandle(hRead); return -1; }

    // 读取子进程输出
    char buf[512];
    DWORD nread = 0;
    while (ReadFile(hRead, buf, sizeof(buf) - 1, &nread, nullptr) && nread > 0) {
        buf[nread] = 0;
        output += buf;
        if (on_progress) {
            std::string chunk(buf, nread);
            size_t pos = 0, nl;
            while ((nl = chunk.find('\n', pos)) != std::string::npos) {
                std::string line = chunk.substr(pos, nl - pos);
                while (!line.empty() &&
                       (line.back() == '\r' || line.back() == '\n'))
                    line.pop_back();
                if (!line.empty()) on_progress(line, 75);
                pos = nl + 1;
            }
        }
    }
    CloseHandle(hRead);

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return static_cast<int>(exitCode);
}
#endif // _WIN32

// ── 收集所有需要打包的文件 ─────────────────────────────────────────────────────
std::vector<std::string>
FileDeployer::CollectFiles(const DepReport& report, const Options& opts)
{
    std::vector<std::string> files;
    std::unordered_set<std::string> seen;   // 用文件名去重

    auto add = [&](const std::string& path) {
        if (path.empty()) return;
        if (IsWindowsSystemPath(path)) return;
        std::string fname = fs::path(path).filename().string();
        // 转小写用于去重
        std::string key = ToLower(fname);
        if (seen.insert(key).second)
            files.push_back(path);
    };

    // exe 本身排第一
    if (!report.target_exe.empty())
        add(report.target_exe);

    // ThirdParty Found：必须复制
    for (auto* n : report.to_copy)
        add(n->resolved_path);

    // Redist MaybeAbsent：仅当 copy_redist_dlls=true 时打包
    if (opts.copy_redist_dlls) {
        for (auto* n : report.maybe_absent)
            add(n->resolved_path);
    }

    // Missing：无法找到，跳过（无法打包没有的文件）
    // OsCore / ApiSet(Win10+)：系统自带，无需打包

    return files;
}

// ── Deploy: 复制到目录 ────────────────────────────────────────────────────────
int FileDeployer::Deploy(const DepReport& report,
                         const std::string& dest_dir,
                         const Options& opts,
                         std::vector<std::string>& errors,
                         ProgressCallback on_progress)
{
    std::error_code ec;
    fs::create_directories(dest_dir, ec);

    int copied = 0;

    // 复制 exe 本身
    if (!report.target_exe.empty()) {
        fs::path src_exe = report.target_exe;
        fs::path dst_exe = fs::path(dest_dir) / src_exe.filename();
        std::error_code ec_eq;
        bool same = fs::exists(dst_exe, ec_eq) && fs::equivalent(src_exe, dst_exe, ec_eq);
        if (!same) {
            if (on_progress) on_progress("Copying " + src_exe.filename().string(), 0);
            std::error_code ec_cp;
            fs::copy_file(src_exe, dst_exe,
                          opts.overwrite ? fs::copy_options::overwrite_existing
                                         : fs::copy_options::skip_existing, ec_cp);
            if (ec_cp)
                errors.push_back("Failed to copy exe: " + ec_cp.message());
            else
                ++copied;
        }
    }

    std::unordered_set<std::string> seen;
    std::vector<std::string> src_paths;
    auto add_node = [&](const DepNode* n) {
        if (n->resolved_path.empty()) return;
        if (IsWindowsSystemPath(n->resolved_path)) return;
        std::string key = fs::path(n->resolved_path).filename().string();
        key = ToLower(key);
        if (seen.insert(key).second)
            src_paths.push_back(n->resolved_path);
    };
    for (auto* n : report.to_copy) add_node(n);
    if (opts.copy_redist_dlls) {
        for (auto* n : report.maybe_absent) add_node(n);
    }

    int total  = static_cast<int>(src_paths.size());

    for (int i = 0; i < total; ++i) {
        fs::path src = src_paths[i];
        fs::path dst = fs::path(dest_dir) / src.filename();

        if (on_progress)
            on_progress("Copying " + src.filename().string(),
                        total ? (i * 100) / total : 100);

        auto copy_opts = opts.overwrite
            ? fs::copy_options::overwrite_existing
            : fs::copy_options::skip_existing;

        fs::copy_file(src, dst, copy_opts, ec);
        if (ec) {
            errors.push_back("Failed to copy " + src.string() +
                             " -> " + dst.string() + ": " + ec.message());
        } else {
            ++copied;
        }
    }

    // ── 复制数据文件（exe目录中非DLL文件：.pth/.zip/.db/.lic 等）──────────────
    for (const auto& df : report.data_files) {
        fs::path src = df;
        fs::path dst = fs::path(dest_dir) / src.filename();
        // 跳过目标位置就是源位置的情况（部署到自身目录）
        std::error_code ec_eq;
        if (fs::exists(dst, ec_eq) && fs::equivalent(src, dst, ec_eq)) continue;
        if (on_progress)
            on_progress("Copying " + src.filename().string(), 94);
        auto copy_opts = opts.overwrite
            ? fs::copy_options::overwrite_existing
            : fs::copy_options::skip_existing;
        std::error_code ec_cp;
        fs::copy_file(src, dst, copy_opts, ec_cp);
        if (ec_cp)
            errors.push_back("Failed to copy " + src.string() + ": " + ec_cp.message());
    }

    // ── 复制资源子目录（逐文件递归，保证每个文件都触发回调）─────────────────────
    for (const auto& rd : report.resource_dirs) {
        fs::path src_root = fs::path(rd.src_path);
        fs::path dst_root = fs::path(dest_dir) / rd.dir_name;

        // 先收集目录下所有文件，以便计算百分比
        std::vector<fs::path> dir_files;
        {
            std::error_code ec_it;
            for (const auto& entry :
                     fs::recursive_directory_iterator(src_root, ec_it)) {
                if (entry.is_regular_file())
                    dir_files.push_back(entry.path());
            }
        }

        int dir_total = static_cast<int>(dir_files.size());
        for (int di = 0; di < dir_total; ++di) {
            const fs::path& src_file = dir_files[di];
            // 目标路径：保留相对于 src_root 父目录的结构
            fs::path rel;
            {
                std::error_code ec_rel;
                rel = fs::relative(src_file, src_root.parent_path(), ec_rel);
            }
            fs::path dst_file = fs::path(dest_dir) / rel;

            // 建立目标父目录
            std::error_code ec_mk;
            fs::create_directories(dst_file.parent_path(), ec_mk);

            if (on_progress) {
                // 显示 "dir_name/sub/file.ext"
                std::error_code ec_disp;
                std::string display = rd.dir_name + "/" +
                    fs::relative(src_file, src_root, ec_disp).generic_string();
                on_progress("Copying " + display,
                            95 + (dir_total ? (di * 4) / dir_total : 4));
            }

            auto copy_opts = opts.overwrite
                ? fs::copy_options::overwrite_existing
                : fs::copy_options::skip_existing;
            std::error_code ec_cp;
            fs::copy_file(src_file, dst_file, copy_opts, ec_cp);
            if (ec_cp)
                errors.push_back("Failed to copy " + src_file.string() +
                                 ": " + ec_cp.message());
        }
    }

    if (on_progress) on_progress("Done", 100);
    return copied;
}

// ── PackZip: 打包成 ZIP ───────────────────────────────────────────────────────
bool FileDeployer::PackZip(const DepReport& report,
                           const std::string& zip_path,
                           const Options& opts,
                           std::vector<std::string>& errors,
                           ProgressCallback on_progress)
{
    auto files = CollectFiles(report, opts);
    if (files.empty()) {
        errors.push_back("No files to pack.");
        return false;
    }

    fs::path output_path = fs::path(zip_path);
    std::vector<std::string> data_files;
    data_files.reserve(report.data_files.size());
    for (const auto& df : report.data_files) {
        if (IsGeneratedPackageArtifact(fs::path(df), report, output_path))
            continue;
        data_files.push_back(df);
    }

    zipFile zf = zipOpen64(zip_path.c_str(), APPEND_STATUS_CREATE);
    if (!zf) {
        errors.push_back("Cannot create ZIP file: " + zip_path);
        return false;
    }

    // ── 预先统计所有条目数，用于统一计算进度百分比 ─────────────────────────────
    int total = (int)files.size() + (int)data_files.size();
    for (const auto& rd : report.resource_dirs) {
        std::error_code ec_cnt;
        for (const auto& e : fs::recursive_directory_iterator(rd.src_path, ec_cnt))
            if (e.is_regular_file()) ++total;
    }
    int done   = 0;
    int packed = 0;

    auto report_progress = [&](const std::string& label) {
        if (on_progress)
            on_progress(label, total > 0 ? (done * 100 / total) : 0);
    };

    // ── 打包 exe / DLL 文件 ───────────────────────────────────────────────────
    for (const auto& src_path : files) {
        std::string filename = fs::path(src_path).filename().string();
        report_progress("Packing " + filename);

        zip_fileinfo zi{};
        int err = zipOpenNewFileInZip(zf, filename.c_str(), &zi,
                                      nullptr, 0, nullptr, 0, nullptr,
                                      Z_DEFLATED, Z_DEFAULT_COMPRESSION);
        ++done;
        if (err != ZIP_OK) {
            errors.push_back("Cannot add entry: " + filename);
            continue;
        }
        std::ifstream f(src_path, std::ios::binary);
        if (!f) {
            errors.push_back("Cannot open: " + src_path);
            zipCloseFileInZip(zf);
            continue;
        }
        char buf[65536];
        while (f.read(buf, sizeof(buf)) || f.gcount() > 0)
            zipWriteInFileInZip(zf, buf, (unsigned int)f.gcount());
        zipCloseFileInZip(zf);
        ++packed;
    }

    // ── 打包数据文件 ──────────────────────────────────────────────────────────
    for (const auto& df : data_files) {
        std::string filename = fs::path(df).filename().string();
        report_progress("Packing " + filename);

        zip_fileinfo zi_d{};
        ++done;
        if (zipOpenNewFileInZip(zf, filename.c_str(), &zi_d,
                                nullptr, 0, nullptr, 0, nullptr,
                                Z_DEFLATED, Z_DEFAULT_COMPRESSION) != ZIP_OK)
            continue;
        std::ifstream fd(df, std::ios::binary);
        char bufd[65536];
        while (fd.read(bufd, sizeof(bufd)) || fd.gcount() > 0)
            zipWriteInFileInZip(zf, bufd, (unsigned int)fd.gcount());
        zipCloseFileInZip(zf);
        ++packed;
    }

    // ── 打包资源子目录（每个文件单独上报进度）────────────────────────────────
    std::function<void(const fs::path&)> pack_dir =
        [&](const fs::path& src_dir)
    {
        std::error_code ec;
        for (const auto& entry : fs::recursive_directory_iterator(src_dir, ec)) {
            if (!entry.is_regular_file()) continue;
            fs::path rel = fs::relative(entry.path(), src_dir.parent_path(), ec);
            std::string zip_entry = rel.generic_string();
            report_progress("Packing " + zip_entry);
            zip_fileinfo zi2{};
            ++done;
            if (zipOpenNewFileInZip(zf, zip_entry.c_str(), &zi2,
                                    nullptr, 0, nullptr, 0, nullptr,
                                    Z_DEFLATED, Z_DEFAULT_COMPRESSION) != ZIP_OK)
                continue;
            std::ifstream ff(entry.path(), std::ios::binary);
            char buf2[65536];
            while (ff.read(buf2, sizeof(buf2)) || ff.gcount() > 0)
                zipWriteInFileInZip(zf, buf2, (unsigned int)ff.gcount());
            zipCloseFileInZip(zf);
            ++packed;
        }
    };
    for (const auto& rd : report.resource_dirs)
        pack_dir(fs::path(rd.src_path));

    zipClose(zf, "Created by LibDeploy");
    if (on_progress) on_progress("Done", 100);
    return packed > 0;
}

// ── GenInstaller: 生成 NSIS 安装包 ───────────────────────────────────────────
bool FileDeployer::GenInstaller(const DepReport& report,
                                const std::string& output_exe,
                                const std::string& app_name,
                                const std::string& makensis_path,
                                const Options& opts,
                                std::vector<std::string>& errors,
                                ProgressCallback on_progress)
{
    // 检查 makensis 是否存在
    if (!fs::exists(makensis_path)) {
        errors.push_back("makensis not found: " + makensis_path +
                         "\nInstall NSIS from https://nsis.sourceforge.io/");
        return false;
    }

    auto files = CollectFiles(report, opts);
    if (files.empty()) {
        errors.push_back("No files to package.");
        return false;
    }

    fs::path installer_output = fs::path(output_exe);
    std::vector<std::string> data_files;
    data_files.reserve(report.data_files.size());
    for (const auto& df : report.data_files) {
        if (IsGeneratedPackageArtifact(fs::path(df), report, installer_output))
            continue;
        data_files.push_back(df);
    }

    std::string exe_name = fs::path(report.target_exe).filename().string();

    // 生成临时 NSI 脚本
    std::string nsi_path = (fs::temp_directory_path() / "libdeploy_setup.nsi").string();

    if (on_progress) on_progress("Generating NSI script...", 10);

    std::ofstream nsi(fs::path(nsi_path), std::ios::binary);
    if (!nsi) {
        errors.push_back("Cannot write NSI script to: " + nsi_path);
        return false;
    }
    // UTF-8 BOM — NSIS 3.06+ 解析 Unicode 文件名需要 UTF-8+BOM
    nsi.write("\xEF\xBB\xBF", 3);

    // 把路径转换为 NSIS 可用的 UTF-8 字符串（反斜杠）
    auto to_nsis_path = [](const fs::path& p) -> std::string {
        std::string s = p.u8string();          // UTF-8，保留 Unicode 字符
        for (auto& c : s) if (c == '/') c = '\\';
        return s;
    };

    auto nsis_escape = [](const std::string& in) -> std::string {
        std::string out;
        out.reserve(in.size());
        for (char c : in) {
            switch (c) {
                case '$': out += "$$"; break;
                case '"': out += "$\\\""; break;
                case '\r': break;
                case '\n': out += "$\\r$\\n"; break;
                default: out += c; break;
            }
        }
        return out;
    };

    // 递归添加目录的辅助 lambda：写 SetOutPath + File 命令，返回所有写入的相对路径
    // rel_in_instdir: 安装目录中的相对路径，如 "locale\zh_CN"
    std::function<void(const fs::path&, const std::string&,
                       std::vector<std::string>&)>
    add_dir_recursive = [&](const fs::path& src_dir,
                            const std::string& rel_in_instdir,
                            std::vector<std::string>& rel_dirs_written)
    {
        std::error_code ec;
        if (!fs::exists(src_dir, ec)) return;
        nsi << "    SetOutPath \"$INSTDIR\\" << rel_in_instdir << "\"\n";
        rel_dirs_written.push_back(rel_in_instdir);
        for (const auto& entry : fs::directory_iterator(src_dir, ec)) {
            if (entry.is_regular_file()) {
                std::error_code ec_ex;
                if (fs::exists(entry.path(), ec_ex))
                    nsi << "    File \"" << to_nsis_path(entry.path()) << "\"\n";
            } else if (entry.is_directory()) {
                add_dir_recursive(entry.path(),
                                  rel_in_instdir + "\\" + entry.path().filename().u8string(),
                                  rel_dirs_written);
            }
        }
    };

    nsi << "; Generated by LibDeploy\n";
    nsi << "Unicode True\n";
    nsi << "SetCompressor lzma\n\n"; // bundled NSIS has lzma-x86-unicode stub, not zlib
    std::string app_name_esc = nsis_escape(app_name);
    nsi << "Name \"" << app_name_esc << "\"\n";
    nsi << "OutFile \"" << to_nsis_path(fs::path(output_exe)) << "\"\n";
    nsi << "InstallDir \"$PROGRAMFILES64\\" << app_name_esc << "\"\n";
    nsi << "InstallDirRegKey HKLM \"Software\\" << app_name_esc << "\" \"Install_Dir\"\n";
    nsi << "RequestExecutionLevel admin\n\n";

    // 页面
    nsi << "Page directory\n";
    nsi << "Page instfiles\n";
    nsi << "UninstPage uninstConfirm\n";
    nsi << "UninstPage instfiles\n\n";

    // 安装 Section
    nsi << "Section \"Install\"\n";
    nsi << "    SetOutPath \"$INSTDIR\"\n";
    for (const auto& f : files) {
        std::error_code ec_ex;
        if (!fs::exists(fs::path(f), ec_ex)) {
            errors.push_back("Installer: file not found (skipped): " + f);
            continue;
        }
        nsi << "    File \"" << to_nsis_path(fs::path(f)) << "\"\n";
    }

    // 数据文件（.pth/.zip/.db/.lic 等非 DLL 文件）
    for (const auto& df : data_files) {
        std::error_code ec_ex;
        if (!fs::exists(fs::path(df), ec_ex)) continue;
        nsi << "    File \"" << to_nsis_path(fs::path(df)) << "\"\n";
    }

    // 附加资源目录（来自 report.resource_dirs，自动发现）
    std::vector<std::string> extra_rel_dirs;   // 用于 Uninstall
    for (const auto& rd : report.resource_dirs)
        add_dir_recursive(fs::path(rd.src_path), rd.dir_name, extra_rel_dirs);

    nsi << "    SetOutPath \"$INSTDIR\"\n";   // 恢复
    nsi << "    WriteRegStr HKLM \"Software\\" << app_name_esc << "\" \"Install_Dir\" \"$INSTDIR\"\n";
    nsi << "    WriteUninstaller \"$INSTDIR\\Uninstall.exe\"\n";
    // 开始菜单 + 桌面快捷方式
    nsi << "    SetShellVarContext all\n";
    nsi << "    CreateDirectory \"$SMPROGRAMS\\" << app_name_esc << "\"\n";
    nsi << "    CreateShortcut \"$SMPROGRAMS\\" << app_name_esc << "\\"
        << app_name_esc << ".lnk\" \"$INSTDIR\\" << exe_name
        << "\" \"\" \"$INSTDIR\\" << exe_name << "\" 0 SW_SHOWNORMAL \"\" \""
        << app_name_esc << "\"\n";
    nsi << "    CreateShortcut \"$DESKTOP\\" << app_name_esc
        << ".lnk\" \"$INSTDIR\\" << exe_name
        << "\" \"\" \"$INSTDIR\\" << exe_name << "\" 0 SW_SHOWNORMAL \"\" \""
        << app_name_esc << "\"\n";
    // 注册卸载信息（用 $\" 而非 \" 避免 NSIS 参数解析错误）
    nsi << "    WriteRegStr HKLM "
           "\"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\"
        << app_name_esc << "\" \"DisplayName\" \"" << app_name_esc << "\"\n";
    nsi << "    WriteRegStr HKLM "
           "\"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\"
        << app_name_esc << "\" \"InstallLocation\" \"$INSTDIR\"\n";
    nsi << "    WriteRegStr HKLM "
           "\"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\"
        << app_name_esc << "\" \"DisplayIcon\" \"$INSTDIR\\" << exe_name << "\"\n";
    nsi << "    WriteRegStr HKLM "
           "\"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\"
        << app_name_esc << "\" \"UninstallString\" \"$\\\"$INSTDIR\\Uninstall.exe$\\\"\"\n";
    nsi << "    WriteRegStr HKLM "
           "\"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\"
        << app_name_esc << "\" \"DisplayVersion\" \"1.0\"\n";
    nsi << "    WriteRegDWORD HKLM "
           "\"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\"
        << app_name_esc << "\" \"NoModify\" 1\n";
    nsi << "    WriteRegDWORD HKLM "
           "\"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\"
        << app_name_esc << "\" \"NoRepair\" 1\n";
    nsi << "SectionEnd\n\n";

    // 卸载 Section
    nsi << "Section \"Uninstall\"\n";
    nsi << "    SetShellVarContext all\n";
    nsi << "    Delete \"$INSTDIR\\Uninstall.exe\"\n";
    for (const auto& f : files)
        nsi << "    Delete \"$INSTDIR\\" << fs::path(f).filename().string() << "\"\n";
    for (const auto& df : data_files)
        nsi << "    Delete \"$INSTDIR\\" << fs::path(df).filename().string() << "\"\n";
    // 卸载附加资源目录（从最深层开始）
    for (auto it = extra_rel_dirs.rbegin(); it != extra_rel_dirs.rend(); ++it)
        nsi << "    RMDir /r \"$INSTDIR\\" << *it << "\"\n";
    nsi << "    Delete \"$DESKTOP\\" << app_name_esc << ".lnk\"\n";
    nsi << "    Delete \"$SMPROGRAMS\\" << app_name_esc << "\\"
        << app_name_esc << ".lnk\"\n";
    nsi << "    RMDir  \"$SMPROGRAMS\\" << app_name_esc << "\"\n";
    nsi << "    RMDir  \"$INSTDIR\"\n";
    nsi << "    DeleteRegKey HKLM "
           "\"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\"
        << app_name_esc << "\"\n";
    nsi << "    DeleteRegKey HKLM \"Software\\" << app_name_esc << "\"\n";
    nsi << "SectionEnd\n";

    // Redist 安装器捆绑尚未实现完整的静默安装段；只要没有复制 Redist DLL，
    // 或选择了“不捆绑安装器”，就必须明确提示用户，避免静默生成不可运行安装包。
    if (!report.redist_needed.empty() &&
        (!opts.copy_redist_dlls || !opts.bundle_redist_installer))
    {
        std::string msg = app_name +
            " has been installed.\n\n"
            "The following runtime packages may be required on the target machine:\n";
        if (opts.bundle_redist_installer && !opts.copy_redist_dlls) {
            msg += "\nRuntime installer bundling is not configured in this build.\n";
        }
        for (const auto& r : report.redist_needed) {
            msg += "  * " + r.name;
            if (!r.url.empty()) msg += "\n    " + r.url;
            msg += "\n";
        }
        msg += "\nPlease install any missing runtimes before running " + app_name + ".";
        nsi << "\nFunction .onInstSuccess\n";
        nsi << "    MessageBox MB_OK|MB_ICONINFORMATION \"" << nsis_escape(msg) << "\"\n";
        nsi << "FunctionEnd\n";
    }

    nsi.close();

    // 运行 makensis
    // NSISDIR 必须通过 Win32 SetEnvironmentVariableA 设置（_putenv 只更新 CRT 环境，
    // 不保证子进程继承；SetEnvironmentVariableA 直接修改进程环境块，子进程可见）
    if (on_progress) on_progress("Running makensis...", 50);
    std::string nsis_root = fs::path(makensis_path).parent_path().parent_path().string();
    std::string nsis_root_bs = nsis_root;
    for (auto& c : nsis_root_bs) if (c == '/') c = '\\';

#ifdef _WIN32
    SetEnvironmentVariableA("NSISDIR", nsis_root_bs.c_str());
#else
    setenv("NSISDIR", nsis_root_bs.c_str(), 1);
#endif

    // 用 CreateProcessW 直接启动 makensis（避免 cmd.exe 中转的引号解析问题），
    // 同时捕获 stdout + stderr（_popen 只捕获 stdout，makensis 写 stderr 会丢失）
    std::string nsis_output;
    int ret = -1;
#ifdef _WIN32
    ret = RunProcessCapture(
        fs::path(makensis_path).wstring(),
        fs::path(nsi_path).wstring(),
        nsis_output, on_progress);
#else
    {
        std::string cmd = "\"" + makensis_path + "\" \"" + nsi_path + "\" 2>&1";
        FILE* pipe = popen(cmd.c_str(), "r");
        char buf[512];
        if (pipe) {
            while (fgets(buf, sizeof(buf), pipe)) {
                nsis_output += buf;
                if (on_progress) {
                    std::string line(buf);
                    while (!line.empty() &&
                           (line.back() == '\n' || line.back() == '\r')) line.pop_back();
                    if (!line.empty()) on_progress(line, 75);
                }
            }
            ret = pclose(pipe);
        }
    }
#endif
    if (ret != 0) {
        errors.push_back("makensis exited with code " + std::to_string(ret) +
                         "\nOutput:\n" + nsis_output +
                         "\nNSI script: " + nsi_path);
        return false;
    }

    if (on_progress) on_progress("Done", 100);
    return true;
}
