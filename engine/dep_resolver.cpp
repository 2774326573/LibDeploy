#include "engine/dep_resolver.h"
#include "engine/pe_parser.h"
#include <filesystem>
#include <algorithm>
#include <unordered_map>
#include <queue>
#include <unordered_set>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <winver.h>
#endif

namespace fs = std::filesystem;

// 前向声明（定义在下方）
static bool IsWindowsSystemPath(const std::string& path);

static void AddWarning(DepReport& report, const std::string& warning)
{
    if (std::find(report.warnings.begin(), report.warnings.end(), warning) != report.warnings.end())
        return;
    report.warnings.push_back(warning);
}

#ifdef _WIN32
static std::wstring Utf8ToWide(const std::string& s)
{
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (len <= 0) {
        len = MultiByteToWideChar(CP_ACP, 0, s.c_str(), -1, nullptr, 0);
        if (len <= 0) return {};
        std::wstring out(static_cast<size_t>(len - 1), L'\0');
        MultiByteToWideChar(CP_ACP, 0, s.c_str(), -1, out.data(), len);
        return out;
    }
    std::wstring out(static_cast<size_t>(len - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), len);
    return out;
}

static std::string GetFileVersionString(const std::string& path)
{
    std::wstring wide = Utf8ToWide(path);
    if (wide.empty()) return {};

    DWORD handle = 0;
    DWORD size = GetFileVersionInfoSizeW(wide.c_str(), &handle);
    if (size == 0) return {};

    std::vector<unsigned char> data(size);
    if (!GetFileVersionInfoW(wide.c_str(), 0, size, data.data())) return {};

    VS_FIXEDFILEINFO* info = nullptr;
    UINT info_len = 0;
    if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &info_len) ||
        !info || info_len == 0)
    {
        return {};
    }

    char buf[64];
    std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u",
        HIWORD(info->dwFileVersionMS),
        LOWORD(info->dwFileVersionMS),
        HIWORD(info->dwFileVersionLS),
        LOWORD(info->dwFileVersionLS));
    return buf;
}
#endif

static int ParseMajorVersion(const std::string& version)
{
    if (version.empty()) return -1;
    size_t i = 0;
    while (i < version.size() && std::isdigit(static_cast<unsigned char>(version[i]))) ++i;
    if (i == 0) return -1;
    try {
        return std::stoi(version.substr(0, i));
    } catch (...) {
        return -1;
    }
}

static std::string ToLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    return s;
}

static std::string PathKey(const fs::path& path) {
    std::error_code ec;
    fs::path p = fs::weakly_canonical(path, ec);
    if (ec) p = path.lexically_normal();
    return ToLower(p.string());
}

DepResolver::DepResolver(TargetOs target_os,
                         const std::vector<std::string>& search_paths,
                         bool follow_system_path,
                         ProgressCallback on_progress,
                         const std::vector<std::string>& resource_scan_paths)
    : m_classifier(target_os)
    , m_search_paths(search_paths)
    , m_resource_scan_paths(resource_scan_paths)
    , m_follow_system(follow_system_path)
    , m_progress(std::move(on_progress))
{}

// ── 全盘 DLL 索引（懒建立）────────────────────────────────────────────────────
// 扫描系统常用 DLL 安装目录，将所有 .dll 文件名（小写）→ 完整路径 建立索引。
// 不依赖任何第三方工具；recursive_directory_iterator 跳过无权访问的目录。
// 仅在首次需要全盘搜索时执行，结果缓存供后续查找复用。
void DepResolver::EnsureDllIndex() const
{
    if (m_dll_index_built) return;
    m_dll_index_built = true;

    // 收集要扫描的根目录（去重）
    std::vector<fs::path> scan_roots;
    auto add_env = [&](const char* var) {
        const char* v = std::getenv(var);
        if (v && *v) {
            fs::path p(v);
            std::error_code ec;
            if (fs::is_directory(p, ec)) scan_roots.push_back(p);
        }
    };
    add_env("ProgramFiles");
    add_env("ProgramFiles(x86)");
    add_env("ProgramW6432");   // 64 位 Program Files（在 32 位进程中有效）

    // Windows 系统目录：System32（64位）和 SysWOW64（32位），
    // 运行时库（msvcp140、vcruntime140 等）安装后也在这里
    {
        const char* sysroot = std::getenv("SystemRoot");
        fs::path win(sysroot ? sysroot : "C:\\Windows");
        std::error_code ec;
        for (const char* sub : {"System32", "SysWOW64"}) {
            fs::path p = win / sub;
            if (fs::is_directory(p, ec)) scan_roots.push_back(p);
        }
    }

    // 去重（不同环境变量可能指向同一路径）
    std::sort(scan_roots.begin(), scan_roots.end());
    scan_roots.erase(std::unique(scan_roots.begin(), scan_roots.end()), scan_roots.end());

    // PATH 里的目录：SDK 的 bin 目录通常在 PATH 中（OpenCV、CUDA 等），
    // 做平铺扫描（不递归），避免把整个 SDK 根目录都递归一遍
    std::vector<fs::path> path_dirs;
    {
        const char* path_env = std::getenv("PATH");
        if (path_env) {
            std::string env(path_env);
            size_t s = 0, e;
            auto sep = env.find_first_of(";:");
            char delim = (sep != std::string::npos && env[sep] == ':') ? ':' : ';';
            while ((e = env.find(delim, s)) != std::string::npos) {
                if (e > s) {
                    fs::path p(env.substr(s, e - s));
                    std::error_code ec;
                    if (fs::is_directory(p, ec)) path_dirs.push_back(p);
                }
                s = e + 1;
            }
            if (s < env.size()) {
                fs::path p(env.substr(s));
                std::error_code ec;
                if (fs::is_directory(p, ec)) path_dirs.push_back(p);
            }
        }
    }

    if (scan_roots.empty() && path_dirs.empty()) return;

    Log("[DirScan] Building DLL index (" +
        std::to_string(scan_roots.size()) + " recursive root(s), " +
        std::to_string(path_dirs.size()) + " PATH dir(s))...");

    size_t count = 0;

    // 辅助：索引单个 .dll 文件
    auto index_file = [&](const fs::path& p) {
        auto ext = p.extension().string();
        std::string lower_ext = ext;
        for (auto& c : lower_ext) c = (char)::tolower((unsigned char)c);
        if (lower_ext != ".dll") return;
        std::string lower_name = p.filename().string();
        for (auto& c : lower_name) c = (char)::tolower((unsigned char)c);
        m_dll_index[lower_name].push_back(p.string());
        ++count;
    };

    // 递归扫描 Program Files / System32 / SysWOW64
    // 用 it.increment(ec) 手动推进，遇到损坏 symlink/junction 时：
    //   1. 先对当前条目调用 disable_recursion_pending()，跳过递归进入
    //   2. increment 出错则清除 ec 继续（而非 throw）
    for (const auto& root : scan_roots) {
        std::error_code ec;
        auto it = fs::recursive_directory_iterator(
            root,
            fs::directory_options::skip_permission_denied,
            ec);
        while (!ec && it != fs::recursive_directory_iterator()) {
            // 检查当前条目的 symlink_status（Windows junction 也会报为 symlink）
            std::error_code ec_sym;
            auto sym_st = it->symlink_status(ec_sym);
            if (!ec_sym && fs::is_symlink(sym_st)) {
                // 不递归进入符号链接目录，避免循环或无效参数错误
                it.disable_recursion_pending();
            }

            std::error_code ec2;
            if (it->is_regular_file(ec2)) index_file(it->path());

            it.increment(ec);
            if (ec) ec.clear();  // 跳过无法进入的单个目录，继续迭代
        }
    }

    // 平铺扫描 PATH 目录（只看目录本身，不递归）
    for (const auto& dir : path_dirs) {
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(
                dir,
                fs::directory_options::skip_permission_denied,
                ec))
        {
            if (ec) { ec.clear(); continue; }
            std::error_code ec2;
            if (!entry.is_regular_file(ec2)) continue;
            index_file(entry.path());
        }
    }

    Log("[DirScan] Index built: " + std::to_string(count) + " DLLs indexed.");
}

// ── 在索引中查找，按目标架构过滤 ─────────────────────────────────────────────
std::string DepResolver::FindDllInIndex(const std::string& lower_name,
                                        uint16_t target_machine) const
{
    EnsureDllIndex();

    // 若名称无扩展名则追加 ".dll"（部分导入表省略扩展名）
    std::string key = lower_name;
    if (fs::path(key).extension().empty()) key += ".dll";

    auto it = m_dll_index.find(key);
    if (it == m_dll_index.end()) return {};

    for (const auto& path : it->second) {
        if (target_machine != 0) {
            uint16_t m = PEParser::GetMachineType(path);
            if (m != 0 && m != target_machine) continue;
        }
        return path;
    }
    return {};
}

// Search a single directory for dll_name (case-insensitive on Windows).
// Also tries appending ".dll" when the name has no extension (some import
// tables omit the extension, e.g. "libcrypto-3-x64" instead of
// "libcrypto-3-x64.dll").
static std::string SearchDir(const std::string& dir, const std::string& raw_name,
                             const std::string& lower_name)
{
    auto try_path = [](const fs::path& p) -> std::string {
        if (fs::exists(p)) return p.string();
        return {};
    };

    if (auto r = try_path(fs::path(dir) / raw_name);   !r.empty()) return r;
    if (auto r = try_path(fs::path(dir) / lower_name); !r.empty()) return r;

    // If the name has no extension, also try appending ".dll"
    if (fs::path(lower_name).extension().empty()) {
        if (auto r = try_path(fs::path(dir) / (raw_name   + ".dll")); !r.empty()) return r;
        if (auto r = try_path(fs::path(dir) / (lower_name + ".dll")); !r.empty()) return r;
    }
    return {};
}

std::string DepResolver::FindDll(const std::string& dll_name) const {
    std::string lower = dll_name;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    for (const auto& dir : m_search_paths) {
        auto r = SearchDir(dir, dll_name, lower);
        if (!r.empty()) return r;
    }
    if (m_follow_system) {
        const char* path_env = std::getenv("PATH");
        if (path_env) {
            std::string env(path_env);
            size_t start = 0, end;
            while ((end = env.find(';', start)) != std::string::npos) {
                auto r = SearchDir(env.substr(start, end - start), dll_name, lower);
                if (!r.empty()) return r;
                start = end + 1;
            }
            // last segment (no trailing ';')
            if (start < env.size()) {
                auto r = SearchDir(env.substr(start), dll_name, lower);
                if (!r.empty()) return r;
            }
        }
        // Also search Windows system directories (for UCRT and system redist DLLs)
        const char* sysroot = std::getenv("SystemRoot");
        if (sysroot) {
            for (const char* sub : {"System32", "SysWOW64"}) {
                auto r = SearchDir(std::string(sysroot) + "\\" + sub, dll_name, lower);
                if (!r.empty()) return r;
            }
        }
    }
    return {};
}

// 判断路径是否在 Windows 系统目录下（System32 / SysWOW64 / WinSxS 等）
static bool IsWindowsSystemPath(const std::string& path)
{
    const char* sysroot = std::getenv("SystemRoot");
    std::string win_dir = sysroot ? sysroot : "C:\\Windows";
    // 转小写比较
    std::string p = path, w = win_dir;
    std::transform(p.begin(), p.end(), p.begin(), ::tolower);
    std::transform(w.begin(), w.end(), w.begin(), ::tolower);
    // 替换正斜杠
    for (auto& c : p) if (c == '/') c = '\\';
    for (auto& c : w) if (c == '/') c = '\\';
    return p.size() >= w.size() && p.substr(0, w.size()) == w;
}

// 已知 Redist 包的官方下载链接
static std::string RedistUrl(const std::string& package) {
    if (package.find("VC++ 2022") != std::string::npos ||
        package.find("VC++ 2019") != std::string::npos ||
        package.find("VC++ 2015") != std::string::npos)
        return "https://aka.ms/vs/17/release/vc_redist.x64.exe";
    if (package.find("VC++ 2013") != std::string::npos)
        return "https://www.microsoft.com/en-US/download/details.aspx?id=40784";
    if (package.find("VC++ 2012") != std::string::npos)
        return "https://www.microsoft.com/en-US/download/details.aspx?id=30679";
    if (package.find("Universal CRT") != std::string::npos)
        return "https://support.microsoft.com/kb/2999226";
    if (package.find("DirectX Shader Compiler") != std::string::npos)
        return "https://www.microsoft.com/en-US/download/details.aspx?id=35";
    if (package.find("Media Foundation") != std::string::npos)
        return "https://support.microsoft.com/topic/media-feature-pack-for-windows-10-n-may-2020-ebbdf559-b84c-0fc2-bd51-e23c9f6a4439";
    return {};
}

DepReport DepResolver::Resolve(const std::string& target_exe) {
    DepReport report;
    report.target_exe = target_exe;

    // root node (the exe itself)
    auto root = std::make_shared<DepNode>();
    root->name          = fs::path(target_exe).filename().string();
    root->resolved_path = target_exe;
    root->category      = DllCategory::ThirdParty;
    root->status        = DepStatus::Found;
    root->need_deploy   = false;
    report.root = root;

    // 读取目标 exe 的架构（用于过滤位数不匹配的 DLL）
    uint16_t target_machine = PEParser::GetMachineType(target_exe);

    // Prepend the exe's directory to search paths
    auto exe_dir = fs::path(target_exe).parent_path().string();
    std::vector<std::string> effective_paths;
    effective_paths.push_back(exe_dir);
    for (const auto& p : m_search_paths) effective_paths.push_back(p);

    // BFS
    std::unordered_map<std::string, std::shared_ptr<DepNode>> visited; // lower-name → node
    std::unordered_set<std::string> analyzed_files; // normalized path → already queued/scanned
    std::unordered_set<std::string> redist_packages_seen;

    struct WorkItem {
        std::string path;
        DepNode*    parent;
    };
    std::queue<WorkItem> queue;
    queue.push({target_exe, root.get()});
    analyzed_files.insert(PathKey(target_exe));

    // 跳过明确不需要部署的扩展名
    static const std::unordered_set<std::string> kSkipExt = {
        ".pdb", ".ilk", ".exp", ".lib", ".a",
        ".def",               // 导入定义文件，仅供链接器使用
        ".log", ".tmp", ".bak",
    };

    // ── Phase 0：扫描 exe 目录，补充 PE 导入表扫描不到的文件 ──────────────────
    // 动态加载的 PE（.pyd Python 扩展、.dll 插件）→ 纳入 BFS 分析依赖
    // 数据文件（.zip .pth .db .lic .ini .json 等）      → 记入 data_files
    {
        std::error_code ec0;
        for (const auto& entry : fs::directory_iterator(exe_dir, ec0)) {
            if (!entry.is_regular_file()) continue;
            // 跳过 exe 自身
            if (fs::equivalent(entry.path(), fs::path(target_exe), ec0)) continue;

            std::string fname = entry.path().filename().string();
            std::string lower = ToLower(fname);
            std::string ext   = ToLower(entry.path().extension().string());

            if (kSkipExt.count(ext)) continue;

            if (ext == ".dll" || ext == ".pyd") {
                std::string file_key = PathKey(entry.path());
                if (!analyzed_files.insert(file_key).second) continue;
                // 架构检查（跳过位数不匹配的）
                if (target_machine != 0) {
                    uint16_t m = PEParser::GetMachineType(entry.path().string());
                    if (m != 0 && m != target_machine) continue;
                }
                auto node = std::make_shared<DepNode>();
                node->name          = lower;
                node->resolved_path = entry.path().string();
                node->category      = DllCategory::ThirdParty;
                node->status        = DepStatus::Found;
                node->need_deploy   = true;
                if (!visited.count(lower)) visited[lower] = node;
                report.node_pool.push_back(node);   // 保证生命周期
                root->children.push_back(node);
                report.to_copy.push_back(node.get());
                // 进入 BFS 队列，分析其自身的依赖
                queue.push({entry.path().string(), node.get()});
                Log("[ExeDir/PE] " + lower);
            } else {
                // 数据文件：直接记录，不做 PE 分析
                report.data_files.push_back(entry.path().string());
                Log("[ExeDir/Data] " + fname);
            }
        }
    }

    // ── Phase 0.5：扫描资源子目录（在 BFS 之前），同时提取其中的 PE 文件 ──────
    // 资源子目录（packages/、plugins/、scripts/ 等）随 exe 整体部署，
    // 但其中的 DLL/pyd 同样需要分析依赖，以检测缺少的外部库。
    // 注意：这些节点 need_deploy=false，因整个目录已由资源部署覆盖，无需单独复制。
    static const std::unordered_set<std::string> kSkipDirs = {
            ".git", ".svn", ".hg", ".vs", ".idea",
            "__pycache__", "node_modules",
            "cmakefiles", "cmakecache", "obj", "build", "bin",
            "logs", "log", "tmp", "temp", "cache",
            // CMake/MSVC build output directories
            "release", "debug", "relwithdebinfo", "minsizerel",
            // Platform subdirectories
            "x64", "x86", "win32", "win64",
        };

        static const std::unordered_set<std::string> kBinaryContainerDirNames = {
            "bin", "lib", "lib64", "debug", "release",
            "relwithdebinfo", "minsizerel", "x64", "x86",
            "win32", "win64",
        };

        static const std::unordered_set<std::string> kKnownResourceDirNames = {
            "assets", "asset", "images", "image", "img",
            "scripts", "script", "docs", "doc", "help",
            "resources", "resource", "static", "templates",
            "locale", "locales", "data", "config", "configs",
            "packages", "plugins", "runtime", "runtimes",
            "redist", "redistributable", "webview2_runtime",
            // Qt deployment folders produced by windeployqt.
            "platforms", "imageformats", "iconengines", "styles",
            "translations", "sqldrivers", "tls", "bearer", "printsupport",
            "networkinformation",
        };

        static const std::unordered_set<std::string> kResourceFileExt = {
            ".png", ".jpg", ".jpeg", ".bmp", ".gif", ".ico", ".svg", ".webp",
            ".py", ".pyw", ".pyi", ".js", ".mjs", ".css", ".html", ".htm",
            ".json", ".xml", ".yaml", ".yml", ".ini", ".cfg", ".conf", ".toml",
            ".txt", ".md", ".rst", ".csv", ".db", ".sqlite", ".lic", ".dat",
            ".zip", ".pth", ".ttf", ".otf", ".woff", ".woff2", ".qm",
        };

    std::unordered_set<std::string> seen_resource_dirs;
    std::unordered_set<std::string> scanned_related_resource_roots;

    auto is_skipped_dir_name = [&](const std::string& name) {
            if (name.empty()) return true;
            if (name[0] == '.') return true;
            std::string lower_name = ToLower(name);
            if (kSkipDirs.count(lower_name)) return true;
            if (m_extra_excluded_dirs.count(lower_name)) return true;
            return lower_name.size() > 7 && lower_name.substr(lower_name.size() - 7) == "_deploy";
        };

    auto is_binary_container_dir_name = [&](const std::string& name) {
            return kBinaryContainerDirNames.count(ToLower(name)) > 0;
        };

    auto has_direct_resource_file = [&](const fs::path& dir) {
            std::error_code ec;
            if (!fs::exists(dir, ec)) return false;
            for (const auto& entry : fs::directory_iterator(dir, ec)) {
                if (!entry.is_regular_file()) continue;
                std::string ext = ToLower(entry.path().extension().string());
                if (ext == ".dll" || ext == ".pyd" || ext == ".exe") continue;
                if (kSkipExt.count(ext)) continue;
                if (kResourceFileExt.count(ext)) return true;
            }
            return false;
        };

    auto has_resource_file_recursive = [&](const fs::path& dir, int max_depth) {
            std::error_code ec;
            if (!fs::exists(dir, ec)) return false;

            fs::recursive_directory_iterator it(dir, ec), end;
            while (!ec && it != end) {
                if (it.depth() >= max_depth) it.disable_recursion_pending();

                if (it->is_regular_file()) {
                    std::string ext = ToLower(it->path().extension().string());
                    if (ext != ".dll" && ext != ".pyd" && ext != ".exe" &&
                        !kSkipExt.count(ext) && kResourceFileExt.count(ext))
                    {
                        return true;
                    }
                }

                it.increment(ec);
            }
            return false;
        };

    auto add_resource_root = [&](const fs::path& resource_dir,
                                 const std::string& display_name,
                                 const std::string& log_tag) {
            std::error_code ec;
            if (!fs::is_directory(resource_dir, ec)) return;

            std::string name = display_name.empty()
                ? resource_dir.filename().string()
                : display_name;
            if (is_skipped_dir_name(name)) return;

            std::string key = resource_dir.string();
            std::string key_lower = PathKey(resource_dir);
            if (!seen_resource_dirs.insert(key_lower).second) return;

            report.resource_dirs.push_back({key, name});
            Log(log_tag + " " + name + " <- " + key);

            // 为该资源目录创建一个虚拟父节点，挂到 root 下（在树里可见）
            auto dir_node = std::make_shared<DepNode>();
            dir_node->name          = "[" + name + "/]";
            dir_node->resolved_path = key;
            dir_node->category      = DllCategory::ThirdParty;
            dir_node->status        = DepStatus::Found;
            dir_node->need_deploy   = false;
            report.node_pool.push_back(dir_node);
            root->children.push_back(dir_node);

            // 递归找出资源目录中所有 DLL/pyd → 加入 BFS 分析其依赖
            // （挂在 dir_node 下，不加入 to_copy，整个目录已作为资源复制）
            std::error_code ec_r;
            for (const auto& fe :
                 fs::recursive_directory_iterator(resource_dir, ec_r))
            {
                if (!fe.is_regular_file()) continue;
                std::string ext = ToLower(fe.path().extension().string());
                if (ext != ".dll" && ext != ".pyd") continue;
                std::string lower = ToLower(fe.path().filename().string());
                std::string file_key = PathKey(fe.path());
                if (!analyzed_files.insert(file_key).second) continue;
                if (target_machine != 0) {
                    uint16_t m = PEParser::GetMachineType(fe.path().string());
                    if (m != 0 && m != target_machine) continue;
                }
                auto node = std::make_shared<DepNode>();
                node->name          = lower;
                node->resolved_path = fe.path().string();
                node->category      = DllCategory::ThirdParty;
                node->status        = DepStatus::Found;
                node->need_deploy   = false; // 随资源目录部署，无需单独复制
                if (!visited.count(lower)) visited[lower] = node;
                report.node_pool.push_back(node);   // 保证生命周期
                dir_node->children.push_back(node); // 挂在目录节点下
                queue.push({fe.path().string(), node.get()});
                Log("[ResDir/PE] " + lower);
            }
        };

        // 扫描一个目录下的顶层子目录：记录为资源目录，并递归提取其中的 PE 文件
    std::function<void(const std::string&)> scan_resource_dir;
    scan_resource_dir = [&](const std::string& dir_path) {
            fs::path dir(dir_path);
            std::error_code ec;
            if (!fs::exists(dir, ec)) return;
            for (const auto& entry : fs::directory_iterator(dir, ec)) {
                if (!entry.is_directory()) continue;
                std::string name = entry.path().filename().string();
                if (is_skipped_dir_name(name)) {
                    if (is_binary_container_dir_name(name)) {
                        std::error_code ec_nested;
                        for (const auto& nested : fs::directory_iterator(entry.path(), ec_nested)) {
                            if (!nested.is_directory()) continue;
                            std::string nested_name = nested.path().filename().string();
                            std::string nested_lower = ToLower(nested_name);
                            if (is_skipped_dir_name(nested_name)) {
                                if (is_binary_container_dir_name(nested_name))
                                    scan_resource_dir(nested.path().string());
                                continue;
                            }
                            if (kKnownResourceDirNames.count(nested_lower) ||
                                has_direct_resource_file(nested.path()) ||
                                has_resource_file_recursive(nested.path(), 2))
                            {
                                add_resource_root(nested.path(), nested_name, "[ResDir] ");
                            }
                        }
                    }
                    continue;
                }
                add_resource_root(entry.path(), name, "[ResDir] ");
            }
        };

    auto scan_related_resource_dir = [&](const fs::path& dir,
                                         const std::string& log_tag) {
            std::error_code ec;
            if (!fs::is_directory(dir, ec)) return;

            std::string root_key = PathKey(dir);
            if (!scanned_related_resource_roots.insert(root_key).second) return;

            for (const auto& entry : fs::directory_iterator(dir, ec)) {
                if (!entry.is_directory()) continue;
                std::string name = entry.path().filename().string();
                if (is_skipped_dir_name(name)) continue;

                std::string lower_name = ToLower(name);
                if (!kKnownResourceDirNames.count(lower_name) &&
                    !has_direct_resource_file(entry.path()) &&
                    !has_resource_file_recursive(entry.path(), 2))
                {
                    continue;
                }

                add_resource_root(entry.path(), name, log_tag);
            }
        };

    auto scan_related_resource_roots = [&](const fs::path& dll_path) {
            fs::path dll_dir = dll_path.parent_path();
            scan_related_resource_dir(dll_dir, "[SiblingRes]");

            std::string dir_name = ToLower(dll_dir.filename().string());
            if (kBinaryContainerDirNames.count(dir_name)) {
                fs::path package_root = dll_dir.parent_path();
                if (!package_root.empty())
                    scan_related_resource_dir(package_root, "[SiblingRes]");
            }
        };

    auto add_support_dll = [&](const fs::path& dll_path,
                               const std::string& log_tag) {
            std::error_code ec;
            if (!fs::is_regular_file(dll_path, ec)) return;

            std::string file_key = PathKey(dll_path);
            if (!analyzed_files.insert(file_key).second) return;

            if (target_machine != 0) {
                uint16_t m = PEParser::GetMachineType(dll_path.string());
                if (m != 0 && m != target_machine) return;
            }

            std::string lower = ToLower(dll_path.filename().string());
            auto node = std::make_shared<DepNode>();
            node->name          = lower;
            node->resolved_path = dll_path.string();
            node->category      = DllCategory::ThirdParty;
            node->status        = DepStatus::Found;
            node->need_deploy   = true;
            if (!visited.count(lower)) visited[lower] = node;
            report.node_pool.push_back(node);
            root->children.push_back(node);
            report.to_copy.push_back(node.get());
            queue.push({dll_path.string(), node.get()});
            Log(log_tag + " " + lower + " -> " + dll_path.string());
        };

    auto add_qt_runtime_siblings = [&](const fs::path& qt_dll_path) {
            fs::path dir = qt_dll_path.parent_path();
            std::error_code ec;
            if (!fs::is_directory(dir, ec)) return;

            for (const auto& entry : fs::directory_iterator(dir, ec)) {
                if (!entry.is_regular_file()) continue;
                std::string name = ToLower(entry.path().filename().string());
                bool should_copy =
                    name == "libegl.dll" ||
                    name == "libglesv2.dll" ||
                    name == "d3dcompiler_47.dll" ||
                    name == "opengl32sw.dll" ||
                    name == "ucrtbase.dll" ||
                    name == "concrt140.dll" ||
                    name.rfind("api-ms-win-", 0) == 0 ||
                    name.rfind("vcruntime", 0) == 0 ||
                    name.rfind("msvcp", 0) == 0;
                if (should_copy)
                    add_support_dll(entry.path(), "[QtSupport]");
            }
        };

    scan_resource_dir(exe_dir);
    for (const auto& p : m_resource_scan_paths) {
            fs::path dir(p);
            std::error_code ec;
            if (!fs::is_directory(dir, ec)) continue;
            bool same_as_exe_dir = false;
            std::error_code ec_eq;
            if (fs::exists(dir, ec_eq) && fs::exists(exe_dir, ec_eq))
                same_as_exe_dir = fs::equivalent(dir, fs::path(exe_dir), ec_eq);
            if (same_as_exe_dir) {
                scan_resource_dir(p);
                continue;
            }

            std::string root_name = dir.filename().string();
            std::string lower_root_name = ToLower(root_name);
            if (kKnownResourceDirNames.count(lower_root_name) ||
                has_direct_resource_file(dir) ||
                has_resource_file_recursive(dir, 2))
            {
                // 用户显式添加的路径本身就是依赖资源根，例如 assets/scripts/images。
                add_resource_root(dir, root_name, "[UserRes]");
            } else {
                // 容器目录：沿用旧行为，部署其中的顶层资源子目录。
                scan_resource_dir(p);
            }
        }

    while (!queue.empty()) {
        auto [cur_path, parent] = queue.front();
        queue.pop();

        std::vector<std::string> imports;
        std::string parse_err;
        if (!PEParser::GetImports(cur_path, imports, parse_err)) {
            Log("[WARN] Cannot parse " + cur_path + ": " + parse_err);
            continue;
        }

        for (auto raw_name : imports) {
            std::string lower_name = ToLower(raw_name);

            // Normalize: import tables sometimes omit ".dll" (e.g. "libcrypto-3-x64")
            if (fs::path(lower_name).extension().empty()) {
                raw_name   += ".dll";
                lower_name += ".dll";
            }

            std::shared_ptr<DepNode> node;
            if (auto it = visited.find(lower_name); it != visited.end()) {
                // Already processed – just add as child (no re-enqueue)
                parent->children.push_back(it->second);
                continue;
            }

            node = std::make_shared<DepNode>();
            node->name = lower_name;
            visited[lower_name] = node;
            report.node_pool.push_back(node);
            parent->children.push_back(node);

            // 用户手动排除：跳过分析与部署
            if (m_user_excluded.count(lower_name)) {
                node->status      = DepStatus::UserExcluded;
                node->category    = DllCategory::Unknown;
                node->need_deploy = false;
                Log("[Excluded] " + lower_name);
                continue;
            }

            auto cr = m_classifier.Classify(lower_name);
            node->category      = cr.category;
            node->redist_package = cr.redist_package;
            node->need_deploy   = cr.need_deploy;

            if (cr.category == DllCategory::OsCore) {
                node->status = DepStatus::SystemPresent;
                Log("[OsCore]  " + lower_name + " -> skip");
                continue;
            }

            if (cr.category == DllCategory::ApiSet) {
                node->status = cr.need_deploy ? DepStatus::MaybeAbsent
                                              : DepStatus::SystemPresent;
                if (cr.need_deploy && !cr.redist_package.empty() &&
                    !redist_packages_seen.count(cr.redist_package))
                {
                    redist_packages_seen.insert(cr.redist_package);
                    report.redist_needed.push_back({cr.redist_package, RedistUrl(cr.redist_package)});
                }
                Log("[ApiSet]  " + lower_name);
                continue;
            }

            // ThirdParty or Redist
            // Priority: user/runtime search paths → system PATH → exe dir (fallback only)
            // This ensures we deploy known-good versions rather than stale copies
            // that may already exist in the target exe directory.
            std::string found_path;
            // 1. user-supplied search paths + system PATH (known-good versions first)
            found_path = FindDll(raw_name);
            // 2. exe directory as last resort (already-present file)
            if (found_path.empty())
                found_path = SearchDir(exe_dir, raw_name, lower_name);

            // 3. 全盘 DirScan 兜底（仅当前两步都找不到时）
            bool found_via_dirscan = false;
            if (found_path.empty() && m_follow_system) {
                found_path = FindDllInIndex(lower_name, target_machine);
                if (!found_path.empty()) found_via_dirscan = true;
            }

            if (!found_path.empty()) {
                // ── 过滤1：系统目录命中 ──────────────────────────────────────
                // OsCore/ThirdParty → SystemPresent（不部署）
                // Redist            → MaybeAbsent（本机有，目标机可能没有）
                if (IsWindowsSystemPath(found_path)) {
                    node->resolved_path = found_path;
                    if (cr.category == DllCategory::Redist) {
                        node->status = DepStatus::MaybeAbsent;
                        report.maybe_absent.push_back(node.get());
                        if (!cr.redist_package.empty() &&
                            !redist_packages_seen.count(cr.redist_package))
                        {
                            redist_packages_seen.insert(cr.redist_package);
                            report.redist_needed.push_back({cr.redist_package, RedistUrl(cr.redist_package)});
                        }
                        Log("[Redist/SysDLL] " + lower_name + " -> " + found_path);
                    } else {
                        node->status = DepStatus::SystemPresent;
                        Log("[SysDLL]  " + lower_name + " -> " + found_path);
                    }
                    continue;
                }

                // ── 过滤2：架构不匹配（32/64位）→ 尝试 DirScan 找正确架构版本 ──
                if (target_machine != 0) {
                    uint16_t dll_machine = PEParser::GetMachineType(found_path);
                    if (dll_machine != 0 && dll_machine != target_machine) {
                        Log("[ArchMismatch] " + lower_name + " (skip " +
                            std::to_string(dll_machine) + " vs target " +
                            std::to_string(target_machine) + ") — trying DirScan...");
                        std::string alt = m_follow_system
                            ? FindDllInIndex(lower_name, target_machine)
                            : std::string{};
                        if (alt.empty()) {
                            node->status = DepStatus::Missing;
                            Log("[ArchMismatch] " + lower_name + " no matching arch found");
                            continue;
                        }
                        found_path = alt;
                        found_via_dirscan = true;
                    }
                }

                node->resolved_path = found_path;
                node->status        = DepStatus::Found;

                if (cr.category == DllCategory::Redist) {
                    // Found on this machine, but target may not have it
                    node->status = DepStatus::MaybeAbsent;
                    report.maybe_absent.push_back(node.get());
                    if (!cr.redist_package.empty() &&
                        !redist_packages_seen.count(cr.redist_package))
                    {
                        redist_packages_seen.insert(cr.redist_package);
                        report.redist_needed.push_back({cr.redist_package, RedistUrl(cr.redist_package)});
                    }
                    Log("[Redist/!] " + lower_name + " -> " + found_path);
                } else {
                    report.to_copy.push_back(node.get());
                    scan_related_resource_roots(fs::path(found_path));
                    if (lower_name.rfind("qt5", 0) == 0 ||
                        lower_name.rfind("qt6", 0) == 0)
                    {
                        add_qt_runtime_siblings(fs::path(found_path));
                    }
                    if (found_via_dirscan)
                        Log("[DirScan] " + lower_name + " -> " + found_path);
                    else
                        Log("[Found]   " + lower_name + " -> " + found_path);
                }

                if (node->need_deploy)
                    queue.push({found_path, node.get()});
            } else {
                node->status = DepStatus::Missing;
                report.missing.push_back(node.get());
                Log("[Missing] " + lower_name);
            }
        }
    }

    // ── 后处理：Qt 插件发现 & 可选的 Qt 部署工具 ──────────────────────────────
    DiscoverQtPlugins(report);
    if (m_run_qt_deploy)
        RunQtDeployTool(target_exe, report);
    CheckWebView2Compatibility(report);

    return report;
}

void DepResolver::CheckWebView2Compatibility(DepReport& report) const
{
    const bool old_windows =
        m_classifier.Target() == TargetOs::Win7 ||
        m_classifier.Target() == TargetOs::Win81;
    if (!old_windows) return;

    for (const auto& rd : report.resource_dirs) {
        std::string name = ToLower(rd.dir_name);
        std::string path = ToLower(fs::path(rd.src_path).filename().string());
        if (name != "webview2_runtime" && path != "webview2_runtime") continue;

        fs::path runtime = fs::path(rd.src_path) / "msedgewebview2.exe";
        std::error_code ec;
        if (!fs::exists(runtime, ec)) {
            AddWarning(report,
                "WebView2 runtime directory found, but msedgewebview2.exe is missing. "
                "Win7/8.1 web documentation panels may be blank.");
            continue;
        }

#ifdef _WIN32
        std::string version = GetFileVersionString(runtime.string());
#else
        std::string version;
#endif
        int major = ParseMajorVersion(version);
        if (major > 109) {
            AddWarning(report,
                "WebView2 Runtime " + version + " is not compatible with Windows 7/8.1. "
                "The runtime will still be kept in the deployment for Windows 10/11. "
                "For Windows 7/8.1, Microsoft Edge/WebView2 109 is the last supported runtime; "
                "web documentation panels may be blank unless the app provides a fallback view "
                "or a compatible WebView2 109 fixed runtime.");
        } else if (major < 0) {
            AddWarning(report,
                "WebView2 Runtime version could not be read. The runtime will still be kept in "
                "the deployment. For Windows 7/8.1, verify it is version 109 or older; newer "
                "runtimes can make web documentation panels blank.");
        }

        fs::path loader = fs::path(report.target_exe).parent_path() / "WebView2Loader.dll";
        if (!fs::exists(loader, ec)) {
            AddWarning(report,
                "WebView2Loader.dll was not found next to the target executable. "
                "Applications using wxWebViewEdge/WebView2 may fail to initialize the web panel.");
        }
    }
}

// ── Qt 插件目录自动发现 ───────────────────────────────────────────────────────
// 在 to_copy 中找到 Qt5Core/Qt6Core，推算 plugins/ 目录，
// 将含有 PE/ELF/dylib 的子目录追加到 report.resource_dirs。
void DepResolver::DiscoverQtPlugins(DepReport& report) const
{
    const DepNode* qt_core = nullptr;
    for (auto* n : report.to_copy) {
        const std::string& name = n->name;
        if (name == "qt5core.dll" || name == "qt6core.dll" ||
            name.rfind("libqt5core", 0) == 0 ||
            name.rfind("libqt6core", 0) == 0 ||
            name == "qtcore" || name == "qtcore.framework")
        {
            qt_core = n;
            break;
        }
    }
    if (!qt_core || qt_core->resolved_path.empty()) return;

    // Windows: Qt5Core.dll 在 bin/，plugins 在 ../plugins/
    // Linux installed: libQt5Core.so 在 lib/，plugins 在 ../plugins/
    // Linux system: libQt5Core.so 在 /usr/lib/x86_64-linux-gnu/，
    //               plugins 在 /usr/lib/x86_64-linux-gnu/qt5/plugins/
    fs::path qt_bin    = fs::path(qt_core->resolved_path).parent_path();
    fs::path qt_root   = qt_bin.parent_path();
    fs::path plugins   = qt_root / "plugins";

    std::error_code ec;
    if (!fs::is_directory(plugins, ec)) {
        // 尝试 Linux 系统 Qt 布局
        plugins = qt_bin / "qt5" / "plugins";
        if (!fs::is_directory(plugins, ec)) {
            plugins = qt_bin / "qt6" / "plugins";
            if (!fs::is_directory(plugins, ec)) return;
        }
    }

    // 已知插件子目录（按需裁剪；实际存在的才会添加）
    static const char* kQtPluginDirs[] = {
        "platforms", "imageformats", "sqldrivers", "iconengines",
        "styles", "multimedia", "networkinformation", "tls",
        "bearer", "printsupport", "audio", "position", "sensors",
        "webview", "scenegraph", "wayland-shell-integration",
        "wayland-graphics-integration-server",
    };

    // 已加入的资源目录集合（避免重复）
    std::unordered_set<std::string> existing;
    for (const auto& rd : report.resource_dirs) {
        std::string k = rd.src_path;
        for (auto& c : k) c = (char)::tolower((unsigned char)c);
        existing.insert(k);
    }

    for (const char* sub : kQtPluginDirs) {
        fs::path sub_dir = plugins / sub;
        if (!fs::is_directory(sub_dir, ec)) continue;

        // 确认子目录中有可执行二进制文件
        bool has_bin = false;
        for (const auto& e : fs::directory_iterator(sub_dir, ec)) {
            if (!e.is_regular_file()) continue;
            std::string ext = e.path().extension().string();
            for (auto& c : ext) c = (char)::tolower((unsigned char)c);
            if (ext == ".dll" || ext == ".so" || ext == ".dylib" ||
                ext.rfind(".so.", 0) == 0) {
                has_bin = true;
                break;
            }
        }
        if (!has_bin) continue;

        std::string key = sub_dir.string();
        for (auto& c : key) c = (char)::tolower((unsigned char)c);
        if (!existing.insert(key).second) continue;

        report.resource_dirs.push_back({sub_dir.string(), sub});
        Log("[QtPlugin] plugins/" + std::string(sub) + " <- " + sub_dir.string());
    }
}

// ── windeployqt / linuxdeployqt 集成 ─────────────────────────────────────────
// 在 Qt bin 目录查找工具，运行后将输出目录中的新文件合并入 report。
void DepResolver::RunQtDeployTool(const std::string& target_exe,
                                   DepReport& report) const
{
    // 定位 windeployqt / linuxdeployqt
    fs::path tool;
    if (!m_qt_deploy_tool.empty()) {
        tool = m_qt_deploy_tool;
    } else {
        // 从 Qt core DLL 路径推断工具位置
        const DepNode* qt_core = nullptr;
        for (auto* n : report.to_copy) {
            const std::string& name = n->name;
            if (name == "qt5core.dll" || name == "qt6core.dll" ||
                name.rfind("libqt5core", 0) == 0 ||
                name.rfind("libqt6core", 0) == 0)
            {
                qt_core = n;
                break;
            }
        }
        if (!qt_core || qt_core->resolved_path.empty()) return;
        fs::path qt_bin = fs::path(qt_core->resolved_path).parent_path();

#ifdef _WIN32
        for (const char* candidate : {"windeployqt.exe", "windeployqt6.exe"}) {
            fs::path p = qt_bin / candidate;
            std::error_code ec;
            if (fs::exists(p, ec)) { tool = p; break; }
        }
#else
        for (const char* candidate : {"linuxdeployqt", "macdeployqt"}) {
            fs::path p = qt_bin / candidate;
            std::error_code ec;
            if (fs::exists(p, ec)) { tool = p; break; }
        }
#endif
    }

    std::error_code ec;
    if (tool.empty() || !fs::exists(tool, ec)) {
        Log("[QtDeploy] Tool not found, skipping");
        return;
    }

    // 临时输出目录
    fs::path temp_out = fs::temp_directory_path() / "libdeploy_qtdeploy_out";
    fs::remove_all(temp_out, ec);
    fs::create_directories(temp_out, ec);
    if (ec) { Log("[QtDeploy] Cannot create temp dir: " + ec.message()); return; }

    Log("[QtDeploy] Running: " + tool.string());

    // 构建命令行并执行
#ifdef _WIN32
    {
        std::wstring cmdline = L"\"" + tool.wstring() +
                               L"\" --dir \"" + temp_out.wstring() +
                               L"\" \"" + fs::path(target_exe).wstring() + L"\"";
        STARTUPINFOW si{};
        PROCESS_INFORMATION pi{};
        si.cb = sizeof(si);
        si.dwFlags     = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        std::vector<wchar_t> buf(cmdline.begin(), cmdline.end());
        buf.push_back(0);
        if (CreateProcessW(nullptr, buf.data(), nullptr, nullptr,
                           FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            WaitForSingleObject(pi.hProcess, 120000);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        } else {
            Log("[QtDeploy] Failed to launch tool");
            return;
        }
    }
#else
    {
        std::string cmd = "\"" + tool.string() + "\" \"" + target_exe +
                          "\" --dir \"" + temp_out.string() + "\"";
        std::system(cmd.c_str());
    }
#endif

    if (!fs::is_directory(temp_out, ec)) {
        Log("[QtDeploy] No output produced");
        return;
    }

    // 已知的 DLL 名集合（避免重复）
    std::unordered_set<std::string> known_dlls;
    known_dlls.insert(ToLower(fs::path(target_exe).filename().string()));
    for (auto* n : report.to_copy)      known_dlls.insert(n->name);
    for (auto* n : report.maybe_absent) known_dlls.insert(n->name);

    std::unordered_set<std::string> known_dirs;
    for (const auto& rd : report.resource_dirs)
        known_dirs.insert(ToLower(rd.src_path));

    // 合并输出：顶层 DLL → to_copy，子目录 → resource_dirs
    for (const auto& entry : fs::directory_iterator(temp_out, ec)) {
        if (entry.is_regular_file()) {
            std::string ext = entry.path().extension().string();
            for (auto& c : ext) c = (char)::tolower((unsigned char)c);
            if (ext != ".dll" && ext != ".so" && ext != ".dylib") continue;

            std::string lower_name = ToLower(entry.path().filename().string());
            if (!known_dlls.insert(lower_name).second) continue;

            auto node = std::make_shared<DepNode>();
            node->name          = lower_name;
            node->resolved_path = entry.path().string();
            node->category      = DllCategory::ThirdParty;
            node->status        = DepStatus::Found;
            node->need_deploy   = true;
            report.node_pool.push_back(node);
            report.to_copy.push_back(node.get());
            Log("[QtDeploy] + " + lower_name);
        } else if (entry.is_directory()) {
            std::string dir_name = entry.path().filename().string();
            std::string key = ToLower(entry.path().string());
            if (!known_dirs.insert(key).second) continue;
            report.resource_dirs.push_back({entry.path().string(), dir_name});
            Log("[QtDeploy] + dir: " + dir_name);
        }
    }

    Log("[QtDeploy] Done");
}
