#pragma once
#include <wx/wx.h>
#include <wx/treectrl.h>
#include <wx/listctrl.h>
#include <wx/splitter.h>
#include "engine/dep_result.h"
#include "engine/dep_resolver.h"
#include "config/config_manager.h"

class MainFrame : public wxFrame {
public:
    explicit MainFrame(const wxString& title);

private:
    // ── Widgets ───────────────────────────────────────────────────────────────
    wxTextCtrl*   m_exe_path    = nullptr;
    wxChoice*     m_target_os   = nullptr;
    wxTreeCtrl*   m_dep_tree    = nullptr;
    wxListCtrl*   m_search_list = nullptr;
    wxListCtrl*   m_excluded_dirs_list = nullptr;
    wxTextCtrl*   m_log         = nullptr;
    wxPanel*      m_redist_warn = nullptr;
    wxPanel*      m_root_panel  = nullptr;
    wxTextCtrl*   m_redist_text = nullptr;
    wxStaticText* m_detail_name = nullptr;
    wxStaticText* m_detail_path = nullptr;
    wxStaticText* m_detail_cat  = nullptr;

    // ── State ─────────────────────────────────────────────────────────────────
    AppConfig  m_cfg;
    DepReport  m_report;
    bool       m_has_report = false;
    std::string m_log_dir;          // logs/ directory
    std::string m_current_log_file; // current session log path

    // Tree image list indices
    int m_img_found   = -1;
    int m_img_missing = -1;
    int m_img_redist  = -1;
    int m_img_system  = -1;
    int m_img_apiset  = -1;

    // ── Setup ─────────────────────────────────────────────────────────────────
    void BuildMenuBar();
    void BuildToolBar();
    void BuildUI();
    void BuildTreeImageList();
    void LoadConfig();

    // ── Actions ───────────────────────────────────────────────────────────────
    void OnOpenExe(wxCommandEvent&);
    void OnAnalyze(wxCommandEvent&);
    void OnDeploy(wxCommandEvent&);
    void OnCopyAll(wxCommandEvent&);
    void OnPackZip(wxCommandEvent&);
    void OnGenInstaller(wxCommandEvent&);
    void OnShowReport(wxCommandEvent&);
    void OnQtDeploy(wxCommandEvent&);
    void OnAddSearchPath(wxCommandEvent&);
    void OnRemoveSearchPath(wxCommandEvent&);
    void OnMoveUp(wxCommandEvent&);
    void OnMoveDown(wxCommandEvent&);
    void OnAddExcludedDir(wxCommandEvent&);
    void OnRemoveExcludedDir(wxCommandEvent&);
    void OnMoveExcludeUp(wxCommandEvent&);
    void OnMoveExcludeDown(wxCommandEvent&);
    void OnTreeSelChanged(wxTreeEvent&);
    void OnClose(wxCloseEvent&);
    void OnExportLog(wxCommandEvent&);
    void OnHistoryLogs(wxCommandEvent&);

    // ── Helpers ───────────────────────────────────────────────────────────────
    void RefreshExcludedDirsList();
    void PopulateTree(const DepReport& report);
    void PopulateTreeNode(wxTreeItemId parent_id,
                          const DepNode& node,
                          std::unordered_set<std::string>& visited);
    void UpdateRedistWarning(const DepReport& report);
    // 生成完整依赖闭合报告文本（所有文件分类，含缺失文件的引用链）
    wxString BuildFullReport(const DepReport& report);
    // 收集 name -> 直接引用者列表（用于报告中显示"被谁需要"）
    void CollectParentMap(const DepNode& node,
                          std::unordered_map<std::string, std::vector<std::string>>& parent_map,
                          std::unordered_set<std::string>& visited) const;
    void Log(const wxString& msg);
    TargetOs SelectedTargetOs() const;
    void RefreshSearchList();
    void SwitchLanguage(const std::string& lang_code);
    void SwitchTheme(const std::string& theme);
    void ApplyTheme();
    void PushRecentTarget(const std::string& path);
    void UpdateRecentMenu();
    void SaveSessionLog();
    wxMenu* m_recent_menu = nullptr;
    // 异步分析：后台线程执行 Resolve + DirScan，主线程更新进度条
    void RunAnalyzeAsync(const wxString& path);
    // 异步部署：后台线程执行复制，主线程更新进度条
    void RunDeployAsync(const std::string& dest_dir);
    // 异步打包 ZIP
    void RunPackZipAsync(const std::string& zip_path);
    // 异步生成安装程序
    void RunGenInstallerAsync(const std::string& out_path,
                              const std::string& app_name,
                              const std::string& makensis_path);

    wxDECLARE_EVENT_TABLE();
};
