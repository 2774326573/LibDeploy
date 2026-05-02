#include "engine/dll_classifier.h"
#include <algorithm>

DllClassifier::DllClassifier(TargetOs target) : m_target(target) {
    InitBuiltinRules();
}

void DllClassifier::InitBuiltinRules()
{
    // ── OsCore白名单：Win7 SP1及以上所有版本必然存在 ──────────────────────
    static const char* os_core[] = {
        "kernel32.dll", "ntdll.dll", "user32.dll", "gdi32.dll",
        "advapi32.dll", "shell32.dll", "ole32.dll", "oleaut32.dll",
        "ws2_32.dll", "rpcrt4.dll", "comctl32.dll", "comdlg32.dll",
        "imm32.dll", "winmm.dll", "winspool.drv", "shlwapi.dll",
        "setupapi.dll", "cfgmgr32.dll", "msimg32.dll", "opengl32.dll",
        "glu32.dll", "version.dll", "iphlpapi.dll", "psapi.dll",
        "dbghelp.dll", "imagehlp.dll", "netapi32.dll", "userenv.dll",
        "uxtheme.dll", "dwmapi.dll", "wtsapi32.dll", "secur32.dll",
        // GDI+
        "gdiplus.dll",
        // COM/automation
        "urlmon.dll", "wininet.dll", "shlwapi.dll",
        // msvcrt.dll是系统文件但已废弃，不建议依赖，仍列入OsCore
        "msvcrt.dll",
        // 无障碍 / 输入法 / UI框架
        "oleacc.dll", "msctf.dll", "usp10.dll",
        // DirectX 核心（Win7+ 均内置；d3dcompiler_47 不在此列，由 Redist 规则覆盖）
        "d2d1.dll", "dwrite.dll", "dxgi.dll",
        "d3d9.dll", "d3d10.dll", "d3d10_1.dll", "d3d11.dll", "d3d12.dll",
        // 加密 / 安全
        "bcrypt.dll", "bcryptprimitives.dll", "ncrypt.dll",
        "cryptbase.dll", "cryptsp.dll", "crypt32.dll",
        // 现代 Windows 内部 DLL
        "win32u.dll", "gdi32full.dll", "msvcp_win.dll",
        "combase.dll", "coml2.dll", "clbcatq.dll",
        "wldp.dll", "ntmarta.dll",
        // 多媒体（mfplat/mf 不在此列，N 版系统无 Media Foundation，由 Redist 规则覆盖）
        "evr.dll", "avrt.dll",
    };
    for (const char* n : os_core) m_os_core.insert(n);

    // Win10+内置的额外DLL
    if (m_target >= TargetOs::Win10) {
        for (const char* n : {
            "ucrtbase.dll", "kernelbase.dll",
        }) m_os_core.insert(n);
    }

    // ── Redist规则（前缀匹配，按优先级排列）────────────────────────────────
    // MSVC debug runtime – cannot be redistributed by installers; target should
    // be rebuilt in Release or run on a machine with matching Visual Studio.
    m_redist_rules.push_back({"vcruntime140d",   "MSVC Debug Runtime (not redistributable)", true});
    m_redist_rules.push_back({"vcruntime140_1d", "MSVC Debug Runtime (not redistributable)", true});
    m_redist_rules.push_back({"msvcp140d",       "MSVC Debug Runtime (not redistributable)", true});
    m_redist_rules.push_back({"ucrtbased",       "MSVC Debug Runtime (not redistributable)", true});
    // VC++ 2015-2022 (v14x)
    m_redist_rules.push_back({"vcruntime140",  "VC++ 2022 x64 Redistributable", true});
    m_redist_rules.push_back({"msvcp140",      "VC++ 2022 x64 Redistributable", true});
    m_redist_rules.push_back({"concrt140",     "VC++ 2022 x64 Redistributable", true});
    m_redist_rules.push_back({"vcomp140",      "VC++ 2022 x64 Redistributable", true});
    // VC++ 2013 (v12x)
    m_redist_rules.push_back({"vcruntime120",  "VC++ 2013 Redistributable", true});
    m_redist_rules.push_back({"msvcp120",      "VC++ 2013 Redistributable", true});
    // VC++ 2012 (v11x)
    m_redist_rules.push_back({"msvcp110",      "VC++ 2012 Redistributable", true});
    // UCRT – Win10+内置；Win7/8.1需KB2999226
    bool ucrt_deploy = (m_target < TargetOs::Win10);
    m_redist_rules.push_back({"ucrtbase",          "Universal CRT", ucrt_deploy});
    m_redist_rules.push_back({"api-ms-win-crt-",   "Universal CRT", ucrt_deploy});
    // MinGW/GCC runtime – 总是随包（不是系统自带）
    m_redist_rules.push_back({"libgcc_s_",     "MinGW-w64 Runtime", true});
    m_redist_rules.push_back({"libstdc++",     "MinGW-w64 Runtime", true});
    m_redist_rules.push_back({"libwinpthread", "MinGW-w64 Runtime", true});
    m_redist_rules.push_back({"libgomp",       "MinGW-w64 Runtime", true});
    // DirectX shader compiler – 总是随包
    m_redist_rules.push_back({"d3dcompiler_",  "DirectX Shader Compiler", true});
    // Media Foundation – N版系统可能缺失
    m_redist_rules.push_back({"mfplat",  "Media Foundation", true});
    m_redist_rules.push_back({"mf.dll",  "Media Foundation", true});
}

void DllClassifier::AddOsCore(const std::string& dll_name_lower) {
    m_os_core.insert(dll_name_lower);
}

void DllClassifier::AddRedistRule(const std::string& prefix,
                                   const std::string& package,
                                   bool always_deploy) {
    m_redist_rules.insert(m_redist_rules.begin(), {prefix, package, always_deploy});
}

DllClassifier::ClassifyResult
DllClassifier::Classify(const std::string& name) const
{
    // api-ms-win-* → ApiSet 或 Redist（取决于子类型和目标系统版本）
    if (name.size() > 10 && name.substr(0, 10) == "api-ms-win") {
        // api-ms-win-crt-* 属于 UCRT Redist
        if (name.size() > 14 && name.substr(0, 14) == "api-ms-win-crt") {
            if (m_target < TargetOs::Win10) {
                return {DllCategory::Redist, "Universal CRT", true};
            }
            return {DllCategory::ApiSet, "Universal CRT", false};
        }
        // api-ms-win-downlevel-* 是 Win8.1以下的 AppCompat Shim
        if (name.size() > 20 && name.substr(0, 20) == "api-ms-win-downlevel") {
            if (m_target < TargetOs::Win81) {
                return {DllCategory::Redist, "Windows AppCompat Shim", true};
            }
            return {DllCategory::ApiSet, "", false};
        }
        return {DllCategory::ApiSet, "", false};
    }

    // ext-ms-win-* → ApiSet (AppCompat shim)
    if (name.size() > 10 && name.substr(0, 10) == "ext-ms-win") {
        return {DllCategory::ApiSet, "", false};
    }

    // OsCore白名单
    if (m_os_core.count(name))
        return {DllCategory::OsCore, "", false};

    // Redist规则（前缀匹配）
    for (const auto& rule : m_redist_rules) {
        if (name.size() >= rule.prefix.size() &&
            name.substr(0, rule.prefix.size()) == rule.prefix)
        {
            return {DllCategory::Redist, rule.package, rule.always_deploy};
        }
    }

    // 默认第三方
    return {DllCategory::ThirdParty, "", true};
}
