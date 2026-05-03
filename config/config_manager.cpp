#include "config/config_manager.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <filesystem>

namespace fs = std::filesystem;
using json = nlohmann::json;

const std::string ConfigManager::kDefaultPath = "libdeploy_config.json";

std::string ConfigManager::OsToString(TargetOs os) {
    switch (os) {
        case TargetOs::Win7:   return "windows7";
        case TargetOs::Win81:  return "windows81";
        case TargetOs::Win10:  return "windows10";
        case TargetOs::Win11:  return "windows11";
    }
    return "windows10";
}

TargetOs ConfigManager::OsFromString(const std::string& s) {
    if (s == "windows7")  return TargetOs::Win7;
    if (s == "windows81") return TargetOs::Win81;
    if (s == "windows11") return TargetOs::Win11;
    return TargetOs::Win10;
}

AppConfig ConfigManager::Load(const std::string& path) {
    AppConfig cfg;
    if (!fs::exists(path)) return cfg;

    std::ifstream f(path);
    if (!f) return cfg;

    try {
        json j;
        f >> j;

        if (j.contains("ui_language"))
            cfg.ui_language = j["ui_language"].get<std::string>();
        if (j.contains("ui_theme"))
            cfg.ui_theme = j["ui_theme"].get<std::string>();
        if (j.contains("search_paths"))
            cfg.search_paths = j["search_paths"].get<std::vector<std::string>>();
        if (j.contains("target_os"))
            cfg.target_os = OsFromString(j["target_os"].get<std::string>());
        if (j.contains("follow_system_path"))
            cfg.follow_system_path = j["follow_system_path"].get<bool>();
        if (j.contains("last_target"))
            cfg.last_target = j["last_target"].get<std::string>();

        if (j.contains("deploy")) {
            auto& d = j["deploy"];
            if (d.contains("copy_redist_dlls"))
                cfg.copy_redist_dlls = d["copy_redist_dlls"].get<bool>();
            if (d.contains("bundle_redist_installer"))
                cfg.bundle_redist_installer = d["bundle_redist_installer"].get<bool>();
        }

        if (j.contains("nsis")) {
            auto& n = j["nsis"];
            if (n.contains("makensis_path"))
                cfg.makensis_path = n["makensis_path"].get<std::string>();
            if (n.contains("redist_cache_dir"))
                cfg.redist_cache_dir = n["redist_cache_dir"].get<std::string>();
        }

        if (j.contains("classifier")) {
            auto& c = j["classifier"];
            if (c.contains("extra_os_core"))
                cfg.extra_os_core = c["extra_os_core"].get<std::vector<std::string>>();
            if (c.contains("extra_redist")) {
                for (auto& r : c["extra_redist"]) {
                    AppConfig::RedistRule rule;
                    rule.prefix        = r.value("prefix", "");
                    rule.package       = r.value("package", "");
                    rule.always_deploy = r.value("always_deploy", true);
                    cfg.extra_redist.push_back(rule);
                }
            }
            if (c.contains("user_excluded"))
                cfg.user_excluded = c["user_excluded"].get<std::vector<std::string>>();
        }

        if (j.contains("qt_deploy_tool"))
            cfg.qt_deploy_tool = j["qt_deploy_tool"].get<std::string>();
        if (j.contains("recent_targets")) {
            for (auto& item : j["recent_targets"]) {
                AppConfig::RecentTarget rt;
                if (item.is_string()) {
                    rt.path = item.get<std::string>();
                } else if (item.is_object()) {
                    rt.path = item.value("path", "");
                    if (item.contains("search_paths"))
                        rt.search_paths = item["search_paths"].get<std::vector<std::string>>();
                }
                if (!rt.path.empty())
                    cfg.recent_targets.push_back(std::move(rt));
            }
        }
    } catch (...) {}

    return cfg;
}

bool ConfigManager::Save(const AppConfig& cfg, const std::string& path) {
    try {
        json j;
        j["ui_language"]       = cfg.ui_language;
        j["ui_theme"]          = cfg.ui_theme;
        j["search_paths"]      = cfg.search_paths;
        j["target_os"]         = OsToString(cfg.target_os);
        j["follow_system_path"]= cfg.follow_system_path;
        j["last_target"]       = cfg.last_target;

        j["deploy"]["copy_redist_dlls"]        = cfg.copy_redist_dlls;
        j["deploy"]["bundle_redist_installer"] = cfg.bundle_redist_installer;

        j["nsis"]["makensis_path"]   = cfg.makensis_path;
        j["nsis"]["redist_cache_dir"]= cfg.redist_cache_dir;

        j["qt_deploy_tool"]   = cfg.qt_deploy_tool;
        {
            json arr = json::array();
            for (const auto& rt : cfg.recent_targets)
                arr.push_back({{"path", rt.path}, {"search_paths", rt.search_paths}});
            j["recent_targets"] = arr;
        }

        j["classifier"]["extra_os_core"]  = cfg.extra_os_core;
        j["classifier"]["user_excluded"]  = cfg.user_excluded;
        json redist_arr = json::array();
        for (const auto& r : cfg.extra_redist) {
            redist_arr.push_back({
                {"prefix",        r.prefix},
                {"package",       r.package},
                {"always_deploy", r.always_deploy}
            });
        }
        j["classifier"]["extra_redist"] = redist_arr;

        std::ofstream f(path);
        if (!f) return false;
        f << j.dump(2);
        return true;
    } catch (...) {
        return false;
    }
}
