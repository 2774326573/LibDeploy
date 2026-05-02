#pragma once
#include "engine/dep_result.h"
#include <string>
#include <vector>
#include <unordered_set>

enum class TargetOs { Win7, Win81, Win10, Win11 };

class DllClassifier {
public:
    struct ClassifyResult {
        DllCategory category;
        std::string redist_package; // 非空时表示来源
        bool        need_deploy;    // 是否需要复制到发布目录
    };

    explicit DllClassifier(TargetOs target = TargetOs::Win10);

    // 追加用户自定义OsCore白名单
    void AddOsCore(const std::string& dll_name_lower);
    // 追加用户自定义Redist规则（前缀匹配）
    void AddRedistRule(const std::string& prefix,
                       const std::string& package,
                       bool always_deploy);

    ClassifyResult Classify(const std::string& dll_name_lower) const;
    TargetOs Target() const { return m_target; }

private:
    TargetOs m_target;
    std::unordered_set<std::string> m_os_core;

    struct RedistRule {
        std::string prefix;
        std::string package;
        bool        always_deploy;
    };
    std::vector<RedistRule> m_redist_rules;

    void InitBuiltinRules();
};
