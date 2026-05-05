#include "app/main_frame.h"
#include "engine/dep_resolver.h"
#include "engine/file_deployer.h"
#include "config/config_manager.h"
#include <wx/filedlg.h>
#include <wx/dirdlg.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/imaglist.h>
#include <wx/artprov.h>
#include <wx/aui/aui.h>
#include <wx/stdpaths.h>
#include <wx/filename.h>
#include <wx/textdlg.h>
#include <wx/clipbrd.h>
#include <wx/tokenzr.h>
#include <wx/progdlg.h>
#include <unordered_set>
#include <unordered_map>
#include <algorithm>
#include <filesystem>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <thread>
#include <mutex>
#include <fstream>
#include <wx/dir.h>

namespace fs = std::filesystem;

// ── IDs ──────────────────────────────────────────────────────────────────────
enum {
    ID_OPEN_EXE     = wxID_HIGHEST + 1,
    ID_ANALYZE,
    ID_COPY_ALL,
    ID_PACK_ZIP,
    ID_GEN_INSTALLER,
    ID_ADD_PATH,
    ID_REMOVE_PATH,
    ID_MOVE_UP,
    ID_MOVE_DOWN,
    ID_ADD_EXCLUDE_DIR,
    ID_REMOVE_EXCLUDE_DIR,
    ID_CLEAR_EXCLUDE_DIR,
    ID_LANG_EN,
    ID_LANG_ZH,
    ID_THEME_SYSTEM,
    ID_THEME_LIGHT,
    ID_THEME_DARK,
    ID_DEPLOY,
    ID_SHOW_REPORT,
    ID_QT_DEPLOY,
    ID_EXPORT_LOG,
    ID_HISTORY_LOGS,
    ID_RECENT_CLEAR,
    ID_RECENT_FIRST = wxID_HIGHEST + 100, // 最多 10 个最近路径
};

// ── Tree helpers (defined early so all member functions can use them) ─────────

// NodeData holds a raw pointer into DepReport nodes (kept alive by node_pool).
struct NodeData : wxTreeItemData {
    const DepNode* node;
    explicit NodeData(const DepNode* n) : node(n) {}
};

// ExcludedDirsDropTarget implementation
ExcludedDirsDropTarget::ExcludedDirsDropTarget(MainFrame* frame)
    : m_frame(frame) {}

bool ExcludedDirsDropTarget::OnDropText(wxCoord x, wxCoord y, const wxString& text) {
    if (!m_frame) return false;
    
    wxString input(text);  // Create a copy
    input.Trim(true).Trim(false);
    if (input.IsEmpty()) return false;
    
    m_frame->OnExcludedDirDropped(input);
    return true;
}

static int ImageForNode(const DepNode& n,
                        int found, int missing, int redist, int system, int apiset)
{
    if (n.category == DllCategory::OsCore)    return system;
    if (n.category == DllCategory::ApiSet)    return apiset;
    if (n.status   == DepStatus::Missing)     return missing;
    if (n.status   == DepStatus::MaybeAbsent) return redist;
    return found;
}

static wxString NormalizeExcludedDirToken(wxString token)
{
    token.Trim(true).Trim(false);
    if (token.IsEmpty()) return token;

    // Strip display wrappers such as [name/] or [name]
    if (token.StartsWith("[")) token = token.Mid(1);
    if (token.EndsWith("]")) token = token.Left(token.Length() - 1);

    // Strip repeated-node suffix from tree labels
    if (token.EndsWith(" (...)")) {
        token = token.Left(token.Length() - 6);
    }

    // Remove trailing slashes from directory-style display text
    while (token.EndsWith("/") || token.EndsWith("\\")) {
        token = token.Left(token.Length() - 1);
    }

    // If a path-like token is dropped, keep only the last segment
    int slash = token.Find('/', true);
    int backslash = token.Find('\\', true);
    int cut = std::max(slash, backslash);
    if (cut != wxNOT_FOUND) token = token.Mid(cut + 1);

    token.Trim(true).Trim(false);
    token.MakeLower();

    // If a DLL name is dropped, keep its base name
    if (token.EndsWith(".dll")) {
        token = token.Left(token.Length() - 4);
    }

    token.Trim(true).Trim(false);
    return token;
}

static void ApplyThemeRecursive(wxWindow* window,
                                const wxColour& bg,
                                const wxColour& fg,
                                const wxColour& control_bg)
{
    if (!window) return;
    window->SetForegroundColour(fg);

    if (dynamic_cast<wxTextCtrl*>(window) ||
        dynamic_cast<wxListCtrl*>(window) ||
        dynamic_cast<wxTreeCtrl*>(window)) {
        window->SetBackgroundColour(control_bg);
    } else {
        window->SetBackgroundColour(bg);
    }

    for (wxWindow* child : window->GetChildren()) {
        ApplyThemeRecursive(child, bg, fg, control_bg);
    }
}

wxBEGIN_EVENT_TABLE(MainFrame, wxFrame)
    EVT_BUTTON(ID_OPEN_EXE,       MainFrame::OnOpenExe)
    EVT_BUTTON(ID_ANALYZE,        MainFrame::OnAnalyze)
    EVT_BUTTON(ID_DEPLOY,         MainFrame::OnDeploy)
    EVT_BUTTON(ID_SHOW_REPORT,    MainFrame::OnShowReport)
    EVT_BUTTON(ID_COPY_ALL,       MainFrame::OnCopyAll)
    EVT_BUTTON(ID_PACK_ZIP,       MainFrame::OnPackZip)
    EVT_BUTTON(ID_GEN_INSTALLER,  MainFrame::OnGenInstaller)
    EVT_BUTTON(ID_ADD_PATH,       MainFrame::OnAddSearchPath)
    EVT_BUTTON(ID_REMOVE_PATH,    MainFrame::OnRemoveSearchPath)
    EVT_BUTTON(ID_MOVE_UP,        MainFrame::OnMoveUp)
    EVT_BUTTON(ID_MOVE_DOWN,      MainFrame::OnMoveDown)
    EVT_BUTTON(ID_ADD_EXCLUDE_DIR,    MainFrame::OnAddExcludedDir)
    EVT_BUTTON(ID_REMOVE_EXCLUDE_DIR, MainFrame::OnRemoveExcludedDir)
    EVT_BUTTON(ID_CLEAR_EXCLUDE_DIR,  MainFrame::OnClearExcludedDirs)
    EVT_TREE_SEL_CHANGED(wxID_ANY, MainFrame::OnTreeSelChanged)
    EVT_TREE_BEGIN_DRAG(wxID_ANY, MainFrame::OnTreeBeginDrag)
    EVT_CLOSE(MainFrame::OnClose)
    EVT_MENU(ID_EXPORT_LOG,    MainFrame::OnExportLog)
    EVT_MENU(ID_HISTORY_LOGS,  MainFrame::OnHistoryLogs)
wxEND_EVENT_TABLE()

// ── Constructor ───────────────────────────────────────────────────────────────
MainFrame::MainFrame(const wxString& title)
    : wxFrame(nullptr, wxID_ANY, title, wxDefaultPosition, wxSize(1100, 700))
{
    // 初始化日志目录
    m_log_dir = fs::path(
        wxStandardPaths::Get().GetExecutablePath().ToStdString())
        .parent_path().string() + "/logs";
    std::error_code ec;
    fs::create_directories(m_log_dir, ec);

    LoadConfig();
    BuildMenuBar();
    BuildUI();
    BuildTreeImageList();
    ApplyTheme();
#ifdef __WXMSW__
    SetIcon(wxICON(IDI_APP_ICON));
#endif

    SetMinSize(wxSize(800, 500));
    Centre();
}

// ── Build UI ──────────────────────────────────────────────────────────────────
void MainFrame::BuildMenuBar() {
    auto* mb = new wxMenuBar;

    auto* mFile = new wxMenu;
    mFile->Append(ID_OPEN_EXE, _("&Open EXE...\tCtrl+O"));
    mFile->Append(ID_ANALYZE,  _("&Analyze\tF5"));
    mFile->AppendSeparator();
    m_recent_menu = new wxMenu;
    mFile->AppendSubMenu(m_recent_menu, _("Recent Files"));
    UpdateRecentMenu();
    mFile->AppendSeparator();
    mFile->Append(ID_EXPORT_LOG,   _("&Export Log..."));
    mFile->Append(ID_HISTORY_LOGS, _("&History Logs..."));
    mFile->AppendSeparator();
    mFile->Append(wxID_EXIT,   _("E&xit"));

    auto* mTools = new wxMenu;
    mTools->Append(ID_COPY_ALL,      _("&Copy All Dependencies"));
    mTools->Append(ID_PACK_ZIP,      _("Pack &ZIP..."));
    mTools->Append(ID_GEN_INSTALLER, _("Generate &Installer..."));
    mTools->AppendSeparator();
    mTools->Append(ID_QT_DEPLOY,     _("Run &Qt Deploy Tool..."));

    auto* mLang = new wxMenu;
    mLang->AppendRadioItem(ID_LANG_EN, _("English"));
    mLang->AppendRadioItem(ID_LANG_ZH, _("Chinese (Simplified)"));
    if (m_cfg.ui_language == "zh_CN") mLang->Check(ID_LANG_ZH, true);
    else mLang->Check(ID_LANG_EN, true);

    auto* mTheme = new wxMenu;
    mTheme->AppendRadioItem(ID_THEME_SYSTEM, _("System"));
    mTheme->AppendRadioItem(ID_THEME_LIGHT, _("Light"));
    mTheme->AppendRadioItem(ID_THEME_DARK, _("Dark"));
    if (m_cfg.ui_theme == "dark") mTheme->Check(ID_THEME_DARK, true);
    else if (m_cfg.ui_theme == "light") mTheme->Check(ID_THEME_LIGHT, true);
    else mTheme->Check(ID_THEME_SYSTEM, true);

    auto* mHelp = new wxMenu;
    mHelp->Append(wxID_ABOUT, _("&About"));

    mb->Append(mFile,  _("&File"));
    mb->Append(mTools, _("&Tools"));
    mb->Append(mLang,  _("&Language"));
    mb->Append(mTheme, _("&Theme"));
    mb->Append(mHelp,  _("&Help"));
    SetMenuBar(mb);

    Bind(wxEVT_MENU, [this](wxCommandEvent&){ Close(); }, wxID_EXIT);
    Bind(wxEVT_MENU, [this](wxCommandEvent&){
        wxMessageBox(_("LibDeploy v0.1\nDependency analyzer & deployer\n\nAuthor: onewu\nEmail:  onewu867@gmail.com"),
                     _("About"), wxOK | wxICON_INFORMATION, this);
    }, wxID_ABOUT);
    Bind(wxEVT_MENU, [this](auto& e){ OnOpenExe(e); },        ID_OPEN_EXE);
    Bind(wxEVT_MENU, [this](auto& e){ OnAnalyze(e); },        ID_ANALYZE);
    Bind(wxEVT_MENU, [this](auto& e){ OnCopyAll(e); },        ID_COPY_ALL);
    Bind(wxEVT_MENU, [this](auto& e){ OnPackZip(e); },        ID_PACK_ZIP);
    Bind(wxEVT_MENU, [this](auto& e){ OnGenInstaller(e); },   ID_GEN_INSTALLER);
    Bind(wxEVT_MENU, [this](auto& e){ OnQtDeploy(e); },      ID_QT_DEPLOY);
    Bind(wxEVT_MENU, [this](wxCommandEvent&){ SwitchLanguage("en_US"); }, ID_LANG_EN);
    Bind(wxEVT_MENU, [this](wxCommandEvent&){ SwitchLanguage("zh_CN"); }, ID_LANG_ZH);
    Bind(wxEVT_MENU, [this](wxCommandEvent&){ SwitchTheme("system"); }, ID_THEME_SYSTEM);
    Bind(wxEVT_MENU, [this](wxCommandEvent&){ SwitchTheme("light"); }, ID_THEME_LIGHT);
    Bind(wxEVT_MENU, [this](wxCommandEvent&){ SwitchTheme("dark"); }, ID_THEME_DARK);
}

void MainFrame::BuildUI() {
    auto* root_panel = new wxPanel(this);
    m_root_panel = root_panel;
    auto* root_vbox  = new wxBoxSizer(wxVERTICAL);

    // ── Top bar: exe path + target OS + Analyze button ───────────────────────
    auto* top_bar = new wxBoxSizer(wxHORIZONTAL);
    top_bar->Add(new wxStaticText(root_panel, wxID_ANY, _("EXE:")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
    m_exe_path = new wxTextCtrl(root_panel, wxID_ANY, m_cfg.last_target,
                                wxDefaultPosition, wxDefaultSize, wxTE_READONLY);
    top_bar->Add(m_exe_path, 1, wxEXPAND | wxRIGHT, 4);
    top_bar->Add(new wxButton(root_panel, ID_OPEN_EXE, _("Browse...")), 0, wxRIGHT, 8);

    top_bar->Add(new wxStaticText(root_panel, wxID_ANY, _("Target OS:")),
                 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
    m_target_os = new wxChoice(root_panel, wxID_ANY);
    m_target_os->Append(_("Windows 7"));
    m_target_os->Append(_("Windows 8.1"));
    m_target_os->Append(_("Windows 10"));
    m_target_os->Append(_("Windows 11"));
    m_target_os->SetSelection(static_cast<int>(m_cfg.target_os));
    top_bar->Add(m_target_os, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);

    top_bar->Add(new wxButton(root_panel, ID_ANALYZE,       _("Analyze")),   0, wxRIGHT, 2);

    auto* btn_deploy = new wxButton(root_panel, ID_DEPLOY, _("Deploy"));
    btn_deploy->SetBackgroundColour(wxColour(0, 120, 215));
    btn_deploy->SetForegroundColour(*wxWHITE);
    top_bar->Add(btn_deploy, 0, wxRIGHT, 2);

    auto* btn_report = new wxButton(root_panel, ID_SHOW_REPORT, _("Report"));
    top_bar->Add(btn_report, 0, wxRIGHT, 8);

    top_bar->Add(new wxButton(root_panel, ID_COPY_ALL,      _("Copy All")),  0, wxRIGHT, 2);
    top_bar->Add(new wxButton(root_panel, ID_PACK_ZIP,      _("Pack ZIP")),  0, wxRIGHT, 2);
    top_bar->Add(new wxButton(root_panel, ID_GEN_INSTALLER, _("Installer")), 0);

    root_vbox->Add(top_bar, 0, wxEXPAND | wxALL, 6);

    // ── Main splitter: left tree | right panel ────────────────────────────────
    auto* h_splitter = new wxSplitterWindow(root_panel, wxID_ANY,
                                            wxDefaultPosition, wxDefaultSize,
                                            wxSP_LIVE_UPDATE | wxSP_3D);

    // Left: dependency tree
    m_dep_tree = new wxTreeCtrl(h_splitter, wxID_ANY, wxDefaultPosition,
                                wxDefaultSize,
                                wxTR_HAS_BUTTONS | wxTR_LINES_AT_ROOT |
                                wxTR_SINGLE | wxBORDER_SUNKEN);

    // Right: detail + search paths + redist warning
    auto* right_panel = new wxPanel(h_splitter);
    auto* right_vbox  = new wxBoxSizer(wxVERTICAL);

    // Detail box
    auto* detail_box = new wxStaticBoxSizer(wxVERTICAL, right_panel, _("Detail"));
    m_detail_name = new wxStaticText(right_panel, wxID_ANY, _("(select a node)"));
    m_detail_path = new wxStaticText(right_panel, wxID_ANY, "");
    m_detail_cat  = new wxStaticText(right_panel, wxID_ANY, "");
    detail_box->Add(m_detail_name, 0, wxEXPAND | wxALL, 2);
    detail_box->Add(m_detail_path, 0, wxEXPAND | wxALL, 2);
    detail_box->Add(m_detail_cat,  0, wxEXPAND | wxALL, 2);
    right_vbox->Add(detail_box, 0, wxEXPAND | wxALL, 4);

    // Search paths
    auto* path_box = new wxStaticBoxSizer(wxVERTICAL, right_panel, _("Search Paths"));
    m_search_list = new wxListCtrl(right_panel, wxID_ANY, wxDefaultPosition,
                                   wxSize(-1, 120), wxLC_LIST | wxBORDER_SUNKEN);
    RefreshSearchList();
    path_box->Add(m_search_list, 1, wxEXPAND | wxBOTTOM, 2);

    auto* btn_row = new wxBoxSizer(wxHORIZONTAL);
    btn_row->Add(new wxButton(right_panel, ID_ADD_PATH,    _("Add"),  wxDefaultPosition, wxSize(60,-1)), 0, wxRIGHT, 2);
    btn_row->Add(new wxButton(right_panel, ID_REMOVE_PATH, _("Del"),  wxDefaultPosition, wxSize(60,-1)), 0, wxRIGHT, 2);
    btn_row->Add(new wxButton(right_panel, ID_MOVE_UP,     _("Up"),   wxDefaultPosition, wxSize(60,-1)), 0, wxRIGHT, 2);
    btn_row->Add(new wxButton(right_panel, ID_MOVE_DOWN,   _("Down"), wxDefaultPosition, wxSize(60,-1)));
    path_box->Add(btn_row, 0, wxEXPAND | wxTOP, 2);

    path_box->Add(new wxStaticText(right_panel, wxID_ANY,
        _("Tip: this app's folder is searched first; system PATH is controlled by config.")),
        0, wxTOP, 4);
    right_vbox->Add(path_box, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 4);

    // Excluded directories (resource scan exclusions)
    auto* exclude_box = new wxStaticBoxSizer(wxVERTICAL, right_panel, _("Excluded Directories"));
    m_excluded_dirs_list = new wxListCtrl(right_panel, wxID_ANY, wxDefaultPosition,
                                          wxSize(-1, 100), wxLC_LIST | wxBORDER_SUNKEN);
    RefreshExcludedDirsList();
    m_excluded_dirs_list->SetDropTarget(new ExcludedDirsDropTarget(this));
    exclude_box->Add(m_excluded_dirs_list, 1, wxEXPAND | wxBOTTOM, 2);

    auto* exclude_btn_row = new wxBoxSizer(wxHORIZONTAL);
    exclude_btn_row->Add(new wxButton(right_panel, ID_ADD_EXCLUDE_DIR,    _("Add"),  wxDefaultPosition, wxSize(60,-1)), 0, wxRIGHT, 2);
    exclude_btn_row->Add(new wxButton(right_panel, ID_REMOVE_EXCLUDE_DIR, _("Del"),  wxDefaultPosition, wxSize(60,-1)), 0, wxRIGHT, 2);
    exclude_btn_row->Add(new wxButton(right_panel, ID_CLEAR_EXCLUDE_DIR,  _("Clear"), wxDefaultPosition, wxSize(60,-1)));
    exclude_box->Add(exclude_btn_row, 0, wxEXPAND | wxTOP, 2);

    exclude_box->Add(new wxStaticText(right_panel, wxID_ANY,
        _("Tip: use commas to add multiple directory names at once; matching is case-insensitive.")),
        0, wxTOP, 4);
    right_vbox->Add(exclude_box, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 4);

    // Redist warning panel (hidden until analysis)
    m_redist_warn = new wxPanel(right_panel);
    m_redist_warn->SetBackgroundColour(wxColour(255, 240, 180));
    auto* redist_vbox = new wxBoxSizer(wxVERTICAL);
    auto* redist_title = new wxStaticText(m_redist_warn, wxID_ANY, _("Redistributable Requirements:"));
    wxFont bold = redist_title->GetFont();
    bold.SetWeight(wxFONTWEIGHT_BOLD);
    redist_title->SetFont(bold);
    m_redist_text = new wxTextCtrl(m_redist_warn, wxID_ANY, "",
                                   wxDefaultPosition, wxSize(-1, 80),
                                   wxTE_MULTILINE | wxTE_READONLY | wxBORDER_NONE);
    m_redist_text->SetBackgroundColour(wxColour(255, 240, 180));
    redist_vbox->Add(redist_title, 0, wxALL, 4);
    redist_vbox->Add(m_redist_text, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 4);
    m_redist_warn->SetSizer(redist_vbox);
    m_redist_warn->Hide();
    right_vbox->Add(m_redist_warn, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 4);

    right_panel->SetSizer(right_vbox);

    h_splitter->SplitVertically(m_dep_tree, right_panel, 380);

    root_vbox->Add(h_splitter, 1, wxEXPAND | wxLEFT | wxRIGHT, 4);

    // ── Log panel ─────────────────────────────────────────────────────────────
    m_log = new wxTextCtrl(root_panel, wxID_ANY, "",
                           wxDefaultPosition, wxSize(-1, 120),
                           wxTE_MULTILINE | wxTE_READONLY | wxHSCROLL);
    root_vbox->Add(new wxStaticText(root_panel, wxID_ANY, _("Log:")),
                   0, wxLEFT | wxTOP, 4);
    root_vbox->Add(m_log, 0, wxEXPAND | wxALL, 4);

    root_panel->SetSizer(root_vbox);

    CreateStatusBar(2);
    SetStatusText(_("Ready"));
}

void MainFrame::BuildTreeImageList() {
    auto* il = new wxImageList(16, 16, true, 5);
    // Use stock art icons
    il->Add(wxArtProvider::GetIcon(wxART_TICK_MARK,    wxART_OTHER, wxSize(16,16))); // found
    il->Add(wxArtProvider::GetIcon(wxART_ERROR,        wxART_OTHER, wxSize(16,16))); // missing
    il->Add(wxArtProvider::GetIcon(wxART_WARNING,      wxART_OTHER, wxSize(16,16))); // redist
    il->Add(wxArtProvider::GetIcon(wxART_INFORMATION,  wxART_OTHER, wxSize(16,16))); // system
    il->Add(wxArtProvider::GetIcon(wxART_HELP,         wxART_OTHER, wxSize(16,16))); // apiset
    m_img_found   = 0;
    m_img_missing = 1;
    m_img_redist  = 2;
    m_img_system  = 3;
    m_img_apiset  = 4;
    m_dep_tree->AssignImageList(il);
}

// ── Config ────────────────────────────────────────────────────────────────────
void MainFrame::LoadConfig() {
    m_cfg = ConfigManager::Load();
}

void MainFrame::RefreshSearchList() {
    if (!m_search_list) return;
    m_search_list->DeleteAllItems();
    for (int i = 0; i < (int)m_cfg.search_paths.size(); ++i)
        m_search_list->InsertItem(i, m_cfg.search_paths[i]);
}

void MainFrame::RefreshExcludedDirsList() {
    if (!m_excluded_dirs_list) return;
    m_excluded_dirs_list->DeleteAllItems();
    for (int i = 0; i < (int)m_cfg.extra_excluded_dirs.size(); ++i)
        m_excluded_dirs_list->InsertItem(i, m_cfg.extra_excluded_dirs[i]);
}

void MainFrame::SwitchTheme(const std::string& theme) {
    std::string next = (theme == "dark" || theme == "light") ? theme : "system";
    if (next == m_cfg.ui_theme) return;
    m_cfg.ui_theme = next;
    ConfigManager::Save(m_cfg);
    ApplyTheme();
    SetStatusText(_("Theme changed."));
}

void MainFrame::ApplyTheme() {
    wxColour bg;
    wxColour fg;
    wxColour control_bg;

    if (m_cfg.ui_theme == "dark") {
        bg = wxColour(32, 34, 37);
        fg = wxColour(232, 235, 239);
        control_bg = wxColour(24, 26, 29);
    } else if (m_cfg.ui_theme == "light") {
        bg = wxColour(245, 247, 250);
        fg = wxColour(20, 24, 28);
        control_bg = *wxWHITE;
    } else {
        bg = wxSystemSettings::GetColour(wxSYS_COLOUR_BTNFACE);
        fg = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT);
        control_bg = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW);
    }

    SetBackgroundColour(bg);
    SetForegroundColour(fg);
    ApplyThemeRecursive(m_root_panel, bg, fg, control_bg);

    if (m_redist_warn && m_redist_text) {
        const bool dark = m_cfg.ui_theme == "dark";
        wxColour warn_bg = dark ? wxColour(78, 67, 33) : wxColour(255, 240, 180);
        wxColour warn_fg = dark ? wxColour(255, 235, 170) : wxColour(40, 35, 20);
        m_redist_warn->SetBackgroundColour(warn_bg);
        m_redist_warn->SetForegroundColour(warn_fg);
        m_redist_text->SetBackgroundColour(warn_bg);
        m_redist_text->SetForegroundColour(warn_fg);
        for (wxWindow* child : m_redist_warn->GetChildren()) {
            child->SetBackgroundColour(warn_bg);
            child->SetForegroundColour(warn_fg);
        }
    }

    Refresh();
    Update();
}

TargetOs MainFrame::SelectedTargetOs() const {
    if (!m_target_os) return TargetOs::Win10;
    switch (m_target_os->GetSelection()) {
        case 0: return TargetOs::Win7;
        case 1: return TargetOs::Win81;
        case 3: return TargetOs::Win11;
        default: return TargetOs::Win10;
    }
}

// ── Log ───────────────────────────────────────────────────────────────────────
void MainFrame::Log(const wxString& msg) {
    auto now = std::chrono::system_clock::now();
    auto t   = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf{};
#ifdef _WIN32
    localtime_s(&tm_buf, &t);
#else
    localtime_r(&t, &tm_buf);
#endif
    std::ostringstream ts;
    ts << std::put_time(&tm_buf, "%H:%M:%S");
    m_log->AppendText("[" + ts.str() + "] " + msg + "\n");
}

// ── Actions ───────────────────────────────────────────────────────────────────
void MainFrame::OnOpenExe(wxCommandEvent&) {
    wxFileDialog dlg(this, _("Select executable"), "", "",
                     _("Executables (*.exe;*.dll)|*.exe;*.dll|All files|*.*"),
                     wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dlg.ShowModal() == wxID_CANCEL) return;
    m_exe_path->SetValue(dlg.GetPath());
    m_cfg.last_target = dlg.GetPath().ToStdString();
    PushRecentTarget(m_cfg.last_target);
    ConfigManager::Save(m_cfg);
}

void MainFrame::OnAnalyze(wxCommandEvent&) {
    wxString path = m_exe_path->GetValue();
    if (path.IsEmpty()) {
        wxMessageBox(_("Please select an executable first."), _("LibDeploy"),
                     wxOK | wxICON_WARNING, this);
        return;
    }
    m_dep_tree->DeleteAllItems();
    m_redist_warn->Hide();
    Log(_("Analyzing: ") + path);
    m_cfg.target_os = SelectedTargetOs();
    PushRecentTarget(path.ToStdString());
    // 新建当次会话日志文件名
    wxString exeName = wxFileName(path).GetName();
    m_current_log_file = m_log_dir + "/" +
        wxDateTime::Now().Format("%Y-%m-%d_%H-%M-%S").ToStdString()
        + "_" + exeName.ToStdString() + ".log";
    RunAnalyzeAsync(path);
}

void MainFrame::RunAnalyzeAsync(const wxString& path) {
    struct SharedState {
        std::mutex               mtx;
        std::string              msg = "Initializing...";
        bool                     done = false;
        std::vector<std::string> log_lines;
        DepReport                report;
    };
    auto state = std::make_shared<SharedState>();

    // 在主线程收集所有参数，避免线程内访问 wxWidgets 对象
    std::string path_str = path.ToStdString();

    std::vector<std::string> effective_paths;
    for (const auto& p : m_cfg.search_paths) effective_paths.push_back(p);

    std::vector<std::string> resource_scan_paths;
    for (const auto& p : m_cfg.search_paths) resource_scan_paths.push_back(p);

    TargetOs target_os           = m_cfg.target_os;
    bool     follow_system_path  = m_cfg.follow_system_path;
    std::vector<std::string>           extra_os_core   = m_cfg.extra_os_core;
    std::vector<AppConfig::RedistRule> extra_redist    = m_cfg.extra_redist;
    std::vector<std::string>           user_excluded   = m_cfg.user_excluded;
    std::vector<std::string>           extra_excluded_dirs = m_cfg.extra_excluded_dirs;

    // 进度对话框：Pulse 模式（分析阶段无法精确知道百分比）
    auto* dlg = new wxProgressDialog(
        _("Analyzing"), _("Initializing..."), 100, this,
        wxPD_APP_MODAL | wxPD_ELAPSED_TIME | wxPD_AUTO_HIDE);
    dlg->Show();

    // 后台线程：构造 resolver 并执行 Resolve（含 DirScan 建索引）
    std::thread worker([state, path_str, effective_paths,
                        resource_scan_paths, target_os,
                        follow_system_path, extra_os_core, extra_redist, user_excluded, extra_excluded_dirs]() {
        DepResolver resolver(
            target_os,
            effective_paths,
            follow_system_path,
            [&state](const std::string& msg) {
                std::lock_guard<std::mutex> lk(state->mtx);
                state->msg = msg;
                state->log_lines.push_back(msg);
            },
            resource_scan_paths
        );
        for (const auto& name : extra_os_core)
            resolver.AddOsCore(name);
        for (const auto& r : extra_redist)
            resolver.AddRedistRule(r.prefix, r.package, r.always_deploy);
        if (!user_excluded.empty())
            resolver.SetUserExcluded(user_excluded);
        if (!extra_excluded_dirs.empty())
            resolver.SetExtraExcludedDirs(extra_excluded_dirs);
        state->report = resolver.Resolve(path_str);
        std::lock_guard<std::mutex> lk(state->mtx);
        state->done = true;
    });

    // 主线程轮询：Pulse() 驱动不确定进度动画，同时将日志刷到 UI
    while (true) {
        wxMilliSleep(50);

        bool is_done; std::string cur_msg;
        std::vector<std::string> lines;
        {
            std::lock_guard<std::mutex> lk(state->mtx);
            is_done = state->done;
            cur_msg = state->msg;
            lines.swap(state->log_lines);
        }

        for (const auto& l : lines)
            Log(wxString::FromUTF8(l));

        dlg->Pulse(wxString::FromUTF8(cur_msg));
        if (is_done) break;
    }

    worker.join();
    dlg->Destroy();

    m_report     = std::move(state->report);
    m_has_report = true;

    PopulateTree(m_report);
    SaveSessionLog();
    UpdateRedistWarning(m_report);
    for (const auto& warning : m_report.warnings)
        Log("[WARN] " + wxString::FromUTF8(warning));

    wxString status = wxString::Format(
        _("Found: %zu  Missing: %zu  Redist: %zu"),
        m_report.to_copy.size(),
        m_report.missing.size(),
        m_report.maybe_absent.size()
    );
    SetStatusText(status);
    Log(_("Done. ") + status);
}

void MainFrame::RunDeployAsync(const std::string& dest_dir) {
    // 共享状态（worker 写，主线程读）
    struct SharedState {
        std::mutex  mtx;
        std::string msg     = "Starting...";
        int         pct     = 0;
        bool        done    = false;
        int         copied  = 0;
        std::vector<std::string> errors;
        std::vector<std::string> log_lines;  // 收集日志行，UI 侧刷新
    };
    auto state = std::make_shared<SharedState>();

    FileDeployer::Options opts;
    opts.copy_redist_dlls = m_cfg.copy_redist_dlls;
    opts.overwrite        = true;

    // 进度对话框（显示文件名 + 百分比，带经过时间）
    auto* dlg = new wxProgressDialog(
        _("Deploying"),
        _("Starting..."),
        100, this,
        wxPD_APP_MODAL | wxPD_ELAPSED_TIME | wxPD_AUTO_HIDE);
    dlg->Show();

    // 后台线程：执行文件复制
    std::thread worker([state, dest_dir, opts, report = m_report]() mutable {
        state->copied = FileDeployer::Deploy(
            report, dest_dir, opts, state->errors,
            [&state](const std::string& m, int p) {
                std::lock_guard<std::mutex> lk(state->mtx);
                state->msg = m;
                state->pct = p;
                state->log_lines.push_back(m);
            });
        std::lock_guard<std::mutex> lk(state->mtx);
        state->done = true;
    });

    // 主线程轮询：仅通过 dlg->Update() 驱动事件循环
    // 不使用 wxSafeYield(this)——那会重新激活被 modal 禁用的主窗口，
    // 导致用户误触 Analyze 等按钮，产生树被清空的重入问题。
    // wxProgressDialog::Update() 内部已调用 wxYield，足以保持 UI 响应。
    while (true) {
        wxMilliSleep(50);

        bool is_done; int cur_pct; std::string cur_msg;
        std::vector<std::string> lines;
        {
            std::lock_guard<std::mutex> lk(state->mtx);
            is_done  = state->done;
            cur_pct  = state->pct;
            cur_msg  = state->msg;
            lines.swap(state->log_lines);
        }

        for (const auto& l : lines)
            Log(wxString::FromUTF8(l));

        dlg->Update(cur_pct, wxString::FromUTF8(cur_msg));

        if (is_done) break;
    }

    worker.join();
    dlg->Destroy();

    // 打印错误
    for (const auto& e : state->errors)
        Log(_("ERROR: ") + wxString::FromUTF8(e));

    // 汇总弹窗
    wxString summary = wxString::Format(
        _("Deployed to:\n%s\n\nFiles copied: %d\nResource dirs: %zu\nData files: %zu"),
        wxString::FromUTF8(dest_dir),
        state->copied,
        m_report.resource_dirs.size(),
        m_report.data_files.size());

    if (!m_report.missing.empty()) {
        summary += wxString::Format(_("\n\n[!] %d DLL(s) missing:"),
                                    (int)m_report.missing.size());
        int show = std::min((int)m_report.missing.size(), 6);
        for (int i = 0; i < show; ++i)
            summary += wxString::Format(wxT("\n  - %s"),
                wxString::FromUTF8(m_report.missing[i]->name.c_str()));
        if ((int)m_report.missing.size() > show)
            summary += wxString::Format(_("\n  ... and %d more"),
                                        (int)m_report.missing.size() - show);
    }
    if (!state->errors.empty())
        summary += wxString::Format(_("\n[x] %d error(s) - check the log."),
                                    (int)state->errors.size());

    wxMessageBox(summary, _("Deploy Complete"),
                 state->errors.empty() ? wxOK | wxICON_INFORMATION : wxOK | wxICON_WARNING,
                 this);
}

void MainFrame::OnCopyAll(wxCommandEvent&) {
    if (!m_has_report) {
        wxMessageBox(_("Run Analyze first."), _("LibDeploy"), wxOK | wxICON_WARNING, this);
        return;
    }

    // 选择目标目录（默认为 exe 旁边的 _deploy 子目录）
    wxString default_dest = wxString::FromUTF8(
        (fs::path(m_report.target_exe).parent_path() /
         (fs::path(m_report.target_exe).stem().string() + "_deploy")).string().c_str());

    wxDirDialog dlg(this,
                    _("Select deployment destination directory"),
                    default_dest,
                    wxDD_DEFAULT_STYLE | wxDD_NEW_DIR_BUTTON);
    if (dlg.ShowModal() == wxID_CANCEL) return;

    RunDeployAsync(dlg.GetPath().ToStdString());
}

void MainFrame::OnDeploy(wxCommandEvent& e) {
    // 一键部署：分析 + 直接部署到 {exe名}_deploy/，不弹对话框
    OnAnalyze(e);
    if (!m_has_report) return;

    fs::path exe_path(m_report.target_exe);
    fs::path dest = exe_path.parent_path() /
                    (exe_path.stem().string() + "_deploy");

    Log(_("Deploying to: ") + wxString::FromUTF8(dest.string()));
    RunDeployAsync(dest.string());
}

void MainFrame::OnPackZip(wxCommandEvent&) {
    if (!m_has_report) {
        wxCommandEvent dummy;
        OnAnalyze(dummy);
        if (!m_has_report) return;
    }
    wxFileName exe_fn(m_report.target_exe);
    wxString stamp = wxDateTime::Now().Format("%Y%m%d_%H%M%S");
    wxString zip_path = exe_fn.GetPath() + wxFileName::GetPathSeparator()
                        + exe_fn.GetName() + "_" + stamp + ".zip";
    Log(_("Creating ZIP: ") + zip_path);
    RunPackZipAsync(zip_path.ToStdString());
}

void MainFrame::OnGenInstaller(wxCommandEvent&) {
    if (!m_has_report) {
        wxCommandEvent dummy;
        OnAnalyze(dummy);
        if (!m_has_report) return;
    }

    wxString app_dir = wxFileName(wxStandardPaths::Get().GetExecutablePath()).GetPath();
    wxString bundled_nsis = app_dir + wxFileName::GetPathSeparator()
                            + "nsis" + wxFileName::GetPathSeparator()
                            + "Bin" + wxFileName::GetPathSeparator()
                            + "makensis.exe";
    wxString makensis_path;
    if (wxFileName::FileExists(bundled_nsis)) {
        makensis_path = bundled_nsis;
        Log(_("Using bundled NSIS: ") + makensis_path);
    } else if (wxFileName::FileExists(m_cfg.makensis_path)) {
        makensis_path = m_cfg.makensis_path;
        Log(_("Using system NSIS: ") + makensis_path);
    } else {
        wxMessageBox(
            _("makensis.exe not found.\n"
              "Bundled NSIS should be in nsis/ next to LibDeploy.exe."),
            _("NSIS Not Found"), wxOK | wxICON_WARNING, this);
        return;
    }

    wxFileName exe_fn(m_report.target_exe);
    wxString app_name = exe_fn.GetName();
    wxString stamp = wxDateTime::Now().Format("%Y%m%d_%H%M%S");
    wxString out_path = exe_fn.GetPath() + wxFileName::GetPathSeparator()
                        + app_name + "_Setup_" + stamp + ".exe";
    Log(_("Generating installer: ") + out_path);
    RunGenInstallerAsync(out_path.ToStdString(),
                         app_name.ToStdString(),
                         makensis_path.ToStdString());
}

void MainFrame::OnQtDeploy(wxCommandEvent&) {
    if (!m_has_report) {
        wxMessageBox(_("Run Analyze first."), _("LibDeploy"), wxOK | wxICON_WARNING, this);
        return;
    }

    // Optionally let the user specify the windeployqt path
    wxString tool_hint = wxString::FromUTF8(m_cfg.qt_deploy_tool);
    wxTextEntryDialog dlg(this,
        _("Path to windeployqt / linuxdeployqt (leave empty to auto-detect):"),
        _("Qt Deploy Tool"), tool_hint);
    if (dlg.ShowModal() == wxID_CANCEL) return;

    wxString tool_path = dlg.GetValue().Trim();

    // Save the tool path for the session
    m_cfg.qt_deploy_tool = tool_path.ToStdString();

    Log(_("Running Qt deploy tool..."));

    // Rebuild a minimal resolver just to call RunQtDeployTool on the existing report
    std::string path_str = m_report.target_exe;

    std::vector<std::string> effective_paths;
    for (const auto& p : m_cfg.search_paths) effective_paths.push_back(p);

    TargetOs target_os = m_cfg.target_os;
    bool     follow    = m_cfg.follow_system_path;

    struct SharedState {
        std::mutex               mtx;
        bool                     done = false;
        std::vector<std::string> log_lines;
        DepReport                updated_report;
        bool                     success = false;
    };
    auto state = std::make_shared<SharedState>();
    state->updated_report = m_report; // start from current report

    std::string tool_str = tool_path.ToStdString();
    std::vector<std::string>           extra_os_core = m_cfg.extra_os_core;
    std::vector<AppConfig::RedistRule> extra_redist  = m_cfg.extra_redist;
    std::vector<std::string>           user_excluded = m_cfg.user_excluded;

    auto* pdlg = new wxProgressDialog(
        _("Qt Deploy"), _("Running Qt deploy tool..."), 100, this,
        wxPD_APP_MODAL | wxPD_ELAPSED_TIME | wxPD_AUTO_HIDE);
    pdlg->Show();

    std::thread worker([state, path_str, effective_paths, target_os, follow,
                        tool_str, extra_os_core, extra_redist, user_excluded]() {
        try {
            DepResolver resolver(
                target_os, effective_paths, follow,
                [&state](const std::string& msg) {
                    std::lock_guard<std::mutex> lk(state->mtx);
                    state->log_lines.push_back(msg);
                },
                {}
            );
            for (const auto& name : extra_os_core)
                resolver.AddOsCore(name);
            for (const auto& r : extra_redist)
                resolver.AddRedistRule(r.prefix, r.package, r.always_deploy);
            if (!tool_str.empty())
                resolver.SetQtDeployTool(tool_str);
            if (!user_excluded.empty())
                resolver.SetUserExcluded(user_excluded);
            resolver.SetRunQtDeployTool(true);

            // Run only the Qt deploy tool step on the existing report
            // We pass the target_exe; resolver will invoke the tool and merge output.
            // Re-run Resolve so Qt-discovery paths are populated.
            state->updated_report = resolver.Resolve(path_str);
            state->success = true;
        } catch (...) {
            std::lock_guard<std::mutex> lk(state->mtx);
            state->log_lines.push_back("[Qt Deploy] exception during run");
        }
        std::lock_guard<std::mutex> lk(state->mtx);
        state->done = true;
    });

    while (true) {
        wxMilliSleep(50);
        bool is_done;
        std::vector<std::string> lines;
        {
            std::lock_guard<std::mutex> lk(state->mtx);
            is_done = state->done;
            lines.swap(state->log_lines);
        }
        for (const auto& l : lines)
            Log(wxString::FromUTF8(l));
        pdlg->Pulse();
        if (is_done) break;
    }
    worker.join();
    pdlg->Destroy();

    if (state->success) {
        m_report = std::move(state->updated_report);
        PopulateTree(m_report);
        UpdateRedistWarning(m_report);
        wxString status = wxString::Format(
            _("Found: %zu  Missing: %zu  Redist: %zu"),
            m_report.to_copy.size(),
            m_report.missing.size(),
            m_report.maybe_absent.size()
        );
        SetStatusText(status);
        Log(_("Qt Deploy complete. ") + status);
        wxMessageBox(_("Qt deploy tool finished. Tree refreshed."),
                     _("Qt Deploy"), wxOK | wxICON_INFORMATION, this);
    } else {
        wxMessageBox(_("Qt deploy tool failed or was not found.\nCheck the log for details."),
                     _("Qt Deploy"), wxOK | wxICON_WARNING, this);
    }
}

void MainFrame::OnAddSearchPath(wxCommandEvent&) {
    wxString startDir;
    if (!m_cfg.search_paths.empty())
        startDir = wxString::FromUTF8(m_cfg.search_paths.back());
    else if (!m_cfg.last_target.empty())
        startDir = wxFileName(wxString::FromUTF8(m_cfg.last_target)).GetPath();
    wxDirDialog dlg(this, _("Select search path"), startDir);
    if (dlg.ShowModal() == wxID_CANCEL) return;
    m_cfg.search_paths.push_back(dlg.GetPath().ToStdString());
    RefreshSearchList();
}

void MainFrame::OnRemoveSearchPath(wxCommandEvent&) {
    long sel = m_search_list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
    if (sel < 0) return;
    m_cfg.search_paths.erase(m_cfg.search_paths.begin() + sel);
    RefreshSearchList();
}

void MainFrame::OnMoveUp(wxCommandEvent&) {
    long sel = m_search_list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
    if (sel <= 0) return;
    std::swap(m_cfg.search_paths[sel], m_cfg.search_paths[sel - 1]);
    RefreshSearchList();
    m_search_list->SetItemState(sel - 1, wxLIST_STATE_SELECTED, wxLIST_STATE_SELECTED);
}

void MainFrame::OnMoveDown(wxCommandEvent&) {
    long sel = m_search_list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
    if (sel < 0 || sel >= (long)m_cfg.search_paths.size() - 1) return;
    std::swap(m_cfg.search_paths[sel], m_cfg.search_paths[sel + 1]);
    RefreshSearchList();
    m_search_list->SetItemState(sel + 1, wxLIST_STATE_SELECTED, wxLIST_STATE_SELECTED);
}

void MainFrame::OnAddExcludedDir(wxCommandEvent&) {
    wxTextEntryDialog dlg(this,
                          _("Enter directory names to exclude, separated by commas:"),
                          _("Add Excluded Directory"));
    if (dlg.ShowModal() == wxID_CANCEL) return;

    wxString input = dlg.GetValue();
    if (input.Trim(true).Trim(false).IsEmpty()) return;

    wxStringTokenizer tokenizer(input, ",，、;；\n\r\t");
    int added = 0;
    int skipped = 0;
    while (tokenizer.HasMoreTokens()) {
        wxString token = NormalizeExcludedDirToken(tokenizer.GetNextToken());
        if (token.IsEmpty()) continue;

        std::string dir_name = token.ToStdString();
        if (std::find(m_cfg.extra_excluded_dirs.begin(), m_cfg.extra_excluded_dirs.end(), dir_name)
            != m_cfg.extra_excluded_dirs.end()) {
            ++skipped;
            continue;
        }
        m_cfg.extra_excluded_dirs.push_back(dir_name);
        ++added;
    }

    if (added == 0) {
        wxMessageBox(_("No new directories were added (all already exist or invalid)."),
                     _("LibDeploy"), wxOK | wxICON_WARNING);
        return;
    }

    RefreshExcludedDirsList();
    ConfigManager::Save(m_cfg);

    if (skipped > 0) {
        wxMessageBox(wxString::Format(_("Added %d directories, skipped %d duplicates."), added, skipped),
                     _("LibDeploy"), wxOK | wxICON_INFORMATION);
    }
}

void MainFrame::OnRemoveExcludedDir(wxCommandEvent&) {
    long sel = m_excluded_dirs_list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
    if (sel < 0) return;
    m_cfg.extra_excluded_dirs.erase(m_cfg.extra_excluded_dirs.begin() + sel);
    RefreshExcludedDirsList();
    ConfigManager::Save(m_cfg);
}

void MainFrame::OnClearExcludedDirs(wxCommandEvent&) {
    if (m_cfg.extra_excluded_dirs.empty()) return;

    int choice = wxMessageBox(
        _("Clear all excluded directories? This action cannot be undone."),
        _("Confirm Clear"),
        wxYES_NO | wxNO_DEFAULT | wxICON_WARNING,
        this
    );
    if (choice != wxYES) return;

    m_cfg.extra_excluded_dirs.clear();
    RefreshExcludedDirsList();
    ConfigManager::Save(m_cfg);
}

void MainFrame::OnTreeSelChanged(wxTreeEvent& evt) {
    auto id = evt.GetItem();
    if (!id.IsOk()) return;

    wxTreeItemData* data = m_dep_tree->GetItemData(id);
    if (!data) return;

    auto* nd = dynamic_cast<NodeData*>(data);
    if (!nd) return;

    m_detail_name->SetLabel(_("Name: ") + nd->node->name);

    wxString path_label = nd->node->resolved_path.empty()
        ? _("(not found)") : wxString::FromUTF8(nd->node->resolved_path);
    if (!nd->node->resolved_path.empty()) {
        std::error_code ec;
        auto sz = fs::file_size(fs::path(nd->node->resolved_path), ec);
        if (!ec) {
            if (sz >= 1024 * 1024)
                path_label += wxString::Format(" (%.1f MB)", sz / 1048576.0);
            else if (sz >= 1024)
                path_label += wxString::Format(" (%.1f KB)", sz / 1024.0);
            else
                path_label += wxString::Format(" (%zu B)", (size_t)sz);
        }
    }
    m_detail_path->SetLabel(_("Path: ") + path_label);
    wxString cat_str;
    switch (nd->node->category) {
        case DllCategory::OsCore:     cat_str = _("OsCore (system)");    break;
        case DllCategory::Redist:     cat_str = _("Redistributable");    break;
        case DllCategory::ThirdParty: cat_str = _("Third-party");        break;
        case DllCategory::ApiSet:     cat_str = _("ApiSet (virtual)");   break;
        default:                      cat_str = "Unknown";                break;
    }
    wxString st_str;
    switch (nd->node->status) {
        case DepStatus::Found:         st_str = _("Found");                    break;
        case DepStatus::SystemPresent: st_str = _("System (always present)");  break;
        case DepStatus::MaybeAbsent:   st_str = _("Maybe absent on target");   break;
        case DepStatus::Missing:       st_str = _("MISSING");                  break;
        case DepStatus::UserExcluded:  st_str = _("Excluded");                 break;
    }
    m_detail_cat->SetLabel(_("Category: ") + cat_str + _("  Status: ") + st_str);
}

// ── Collect parent map: child_name -> [parent_name, ...] ─────────────────────
void MainFrame::CollectParentMap(
    const DepNode& node,
    std::unordered_map<std::string, std::vector<std::string>>& parent_map,
    std::unordered_set<std::string>& visited) const
{
    if (visited.count(node.name)) return;
    visited.insert(node.name);
    for (const auto& child : node.children) {
        parent_map[child->name].push_back(node.name);
        CollectParentMap(*child, parent_map, visited);
    }
}

// ── Build comprehensive flat dependency report ────────────────────────────────
wxString MainFrame::BuildFullReport(const DepReport& report)
{
    if (!report.root) return _("No analysis result.");

    // 构建 child->parents 映射（用于显示"被谁需要"）
    std::unordered_map<std::string, std::vector<std::string>> parent_map;
    std::unordered_set<std::string> vis;
    CollectParentMap(*report.root, parent_map, vis);

    wxString out;
    wxString line(wxT("================================================================\n"));

    // ── Header ──────────────────────────────────────────────────────────────
    out += _("Dependency Full Report\n");
    out += line;
    out += _("Target: ") + wxString::FromUTF8(report.target_exe) + "\n";
    out += wxString::Format(
        _("Deploy: %zu  |  Maybe absent: %zu  |  Missing: %zu\n"),
        report.to_copy.size(),
        report.maybe_absent.size(),
        report.missing.size()
    );
    out += line;

    if (!report.warnings.empty()) {
        out += wxString::Format(_("\n[!] Warnings (%zu):\n"), report.warnings.size());
        for (const auto& w : report.warnings)
            out += "  [WARN] " + wxString::FromUTF8(w) + "\n";
    }

    // ── Section 1: MISSING (最重要，列在最前) ──────────────────────────────
    if (!report.missing.empty()) {
        out += wxString::Format(_("\n[!] MISSING (%zu) — not found in any search path:\n"),
                                report.missing.size());
        for (const auto* n : report.missing) {
            out += wxString::Format(wxT("  [MISSING]  %s\n"),
                                    wxString::FromUTF8(n->name));
            auto it = parent_map.find(n->name);
            if (it != parent_map.end()) {
                // 去重父节点列表
                std::vector<std::string> uniq = it->second;
                std::sort(uniq.begin(), uniq.end());
                uniq.erase(std::unique(uniq.begin(), uniq.end()), uniq.end());
                for (const auto& p : uniq)
                    out += wxString::Format(wxT("             <- required by: %s\n"),
                                            wxString::FromUTF8(p));
            }
        }
    }

    // ── Section 2: TO COPY (ThirdParty Found) ────────────────────────────
    out += wxString::Format(_("\n[+] To Copy (%zu) — will be deployed:\n"),
                            report.to_copy.size());
    for (const auto* n : report.to_copy) {
        out += wxString::Format(wxT("  [COPY]     %s\n"),
                                wxString::FromUTF8(n->name));
        if (!n->resolved_path.empty())
            out += wxString::Format(wxT("             <- %s\n"),
                                    wxString::FromUTF8(n->resolved_path));
    }

    // ── Section 3: MAYBE ABSENT (Redist) ──────────────────────────────────
    if (!report.maybe_absent.empty()) {
        out += wxString::Format(_("\n[~] Maybe Absent (%zu) — Redist, may need installer:\n"),
                                report.maybe_absent.size());
        for (const auto* n : report.maybe_absent) {
            out += wxString::Format(wxT("  [REDIST]   %s"),
                                    wxString::FromUTF8(n->name));
            if (!n->redist_package.empty())
                out += wxString::Format(wxT("  (%s)"),
                                        wxString::FromUTF8(n->redist_package));
            out += "\n";
            if (!n->resolved_path.empty())
                out += wxString::Format(wxT("             <- %s\n"),
                                        wxString::FromUTF8(n->resolved_path));
        }
    }

    // ── Section 4: REDIST PACKAGES NEEDED ─────────────────────────────────
    if (!report.redist_needed.empty()) {
        out += _("\n[*] Redistributable packages required on target:\n");
        for (const auto& r : report.redist_needed) {
            out += wxString::Format(wxT("  [PKG]      %s\n"),
                                    wxString::FromUTF8(r.name));
            if (!r.url.empty())
                out += wxString::Format(wxT("             %s\n"),
                                        wxString::FromUTF8(r.url));
        }
    }

    // ── Section 5: DATA FILES（exe目录中非DLL附加文件）────────────────────────
    if (!report.data_files.empty()) {
        out += wxString::Format(_("\n[d] Data Files (%zu) — non-DLL files from exe dir:\n"),
                                report.data_files.size());
        for (const auto& df : report.data_files)
            out += wxString::Format(wxT("  [DATA]     %s\n"),
                                    wxString::FromUTF8(fs::path(df).filename().string().c_str()));
    }

    // ── Section 6: RESOURCE DIRS ───────────────────────────────────────────────
    if (!report.resource_dirs.empty()) {
        out += wxString::Format(_("\n[>] Resource Dirs (%zu) — will be deployed recursively:\n"),
                                report.resource_dirs.size());
        for (const auto& rd : report.resource_dirs)
            out += wxString::Format(wxT("  [DIR]      %s\n             <- %s\n"),
                                    wxString::FromUTF8(rd.dir_name),
                                    wxString::FromUTF8(rd.src_path));
    }

    out += "\n" + line;
    return out;
}

// ── Show Full Report Dialog ───────────────────────────────────────────────────
void MainFrame::OnShowReport(wxCommandEvent&) {
    if (!m_has_report) {
        wxCommandEvent dummy;
        OnAnalyze(dummy);
        if (!m_has_report) return;
    }

    wxString report_text = BuildFullReport(m_report);

    // 显示在可滚动文本对话框中
    wxDialog dlg(this, wxID_ANY, _("Full Dependency Report"),
                 wxDefaultPosition, wxSize(800, 600),
                 wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
    auto* sizer = new wxBoxSizer(wxVERTICAL);

    auto* txt = new wxTextCtrl(&dlg, wxID_ANY, report_text,
                               wxDefaultPosition, wxDefaultSize,
                               wxTE_MULTILINE | wxTE_READONLY | wxHSCROLL |
                               wxTE_RICH2);
    txt->SetFont(wxFont(9, wxFONTFAMILY_TELETYPE,
                        wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL));

    // 将缺失行标红
    wxTextAttr red_attr;
    red_attr.SetTextColour(wxColour(200, 0, 0));
    wxTextAttr normal_attr;
    normal_attr.SetTextColour(*wxBLACK);

    // 按行扫描，对含 [MISSING] 或 MISSING 的行着色
    long line_start = 0;
    wxStringTokenizer tok(report_text, "\n", wxTOKEN_RET_EMPTY);
    while (tok.HasMoreTokens()) {
        wxString line = tok.GetNextToken();
        long line_end = line_start + (long)line.Len();
        if (line.Contains("[MISSING]") || line.StartsWith("  [!]")) {
            txt->SetStyle(line_start, line_end, red_attr);
        }
        line_start = line_end + 1; // +1 for '\n'
    }

    // 复制到剪贴板按钮
    auto* btn_copy = new wxButton(&dlg, wxID_ANY, _("Copy to Clipboard"));
    btn_copy->Bind(wxEVT_BUTTON, [&](wxCommandEvent&){
        if (wxTheClipboard->Open()) {
            wxTheClipboard->SetData(new wxTextDataObject(report_text));
            wxTheClipboard->Close();
        }
    });
    auto* btn_close = new wxButton(&dlg, wxID_OK, _("Close"));

    auto* btn_row = new wxBoxSizer(wxHORIZONTAL);
    btn_row->Add(btn_copy,  0, wxRIGHT, 8);
    btn_row->AddStretchSpacer();
    btn_row->Add(btn_close, 0);

    sizer->Add(txt,     1, wxEXPAND | wxALL, 6);
    sizer->Add(btn_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 6);
    dlg.SetSizer(sizer);
    dlg.ShowModal();
}

void MainFrame::OnExcludedDirDropped(const wxString& text) {
    wxString input(text);  // Create a copy
    input.Trim(true).Trim(false);
    if (input.IsEmpty()) return;
    
    wxStringTokenizer tokenizer(input, ",，、;；\n\r\t");
    int added = 0;
    
    while (tokenizer.HasMoreTokens()) {
        wxString token = NormalizeExcludedDirToken(tokenizer.GetNextToken());
        if (token.IsEmpty()) continue;
        
        std::string dir_name = token.ToStdString();
        if (std::find(m_cfg.extra_excluded_dirs.begin(),
                      m_cfg.extra_excluded_dirs.end(),
                      dir_name) != m_cfg.extra_excluded_dirs.end()) {
            continue;
        }
        
        m_cfg.extra_excluded_dirs.push_back(dir_name);
        ++added;
    }
    
    if (added > 0) {
        RefreshExcludedDirsList();
        ConfigManager::Save(m_cfg);
    }
}

void MainFrame::OnTreeBeginDrag(wxTreeEvent& evt) {
    // We provide our own wxDropSource flow; veto the tree's internal drag flow
    // to avoid MSW drag-image re-entry assertions.
    evt.Veto();

    if (m_tree_drag_in_progress) {
        return;
    }

    auto id = evt.GetItem();
    if (!id.IsOk()) {
        return;
    }

    wxTreeItemData* data = m_dep_tree->GetItemData(id);
    if (!data) {
        return;
    }

    auto* nd = dynamic_cast<NodeData*>(data);
    if (!nd || !nd->node) {
        return;
    }

    // Create a text data object with the DLL name (directory basename)
    std::string dll_name = nd->node->name;
    
    // Extract directory name from DLL name (e.g., "msvcr120.dll" -> "msvcr120")
    size_t dot_pos = dll_name.find_last_of('.');
    if (dot_pos != std::string::npos) {
        dll_name = dll_name.substr(0, dot_pos);
    }
    
    wxString drag_text = wxString::FromUTF8(dll_name);
    wxTextDataObject textData(drag_text);
    wxDropSource source(textData, m_dep_tree);

    m_tree_drag_in_progress = true;
    source.DoDragDrop(wxDrag_CopyOnly);
    m_tree_drag_in_progress = false;
}


void MainFrame::OnClose(wxCloseEvent& evt) {
    ConfigManager::Save(m_cfg);
    evt.Skip();
}

// ── Tree population ───────────────────────────────────────────────────────────

void MainFrame::PopulateTree(const DepReport& report) {
    m_dep_tree->DeleteAllItems();
    if (!report.root) return;

    std::unordered_set<std::string> visited;
    auto root_id = m_dep_tree->AddRoot(
        report.root->name,
        m_img_found, m_img_found,
        new NodeData(report.root.get())
    );

    PopulateTreeNode(root_id, *report.root, visited);
    m_dep_tree->Expand(root_id);
}

void MainFrame::PopulateTreeNode(wxTreeItemId parent_id,
                                  const DepNode& node,
                                  std::unordered_set<std::string>& visited)
{
    for (const auto& child : node.children) {
        bool already = visited.count(child->name) > 0;
        int img = ImageForNode(*child,
                               m_img_found, m_img_missing,
                               m_img_redist, m_img_system, m_img_apiset);
        wxString label = child->name;
        if (already) label += " (...)";

        auto item_id = m_dep_tree->AppendItem(
            parent_id, label, img, img,
            new NodeData(child.get())
        );

        if (!already) {
            visited.insert(child->name);
            PopulateTreeNode(item_id, *child, visited);
        }
    }
}

void MainFrame::SwitchLanguage(const std::string& lang_code) {
    if (lang_code == m_cfg.ui_language) return; // already active

    // Delegate to the App: it will destroy this frame and create a new one
    // with the new locale already loaded.
    // Cast through wxApp base – LibDeployApp is defined in main.cpp.
    // We use a posted event so the current event handler finishes cleanly
    // before the frame is destroyed.
    m_cfg.ui_language = lang_code;

    // Schedule the switch after the current event returns
    CallAfter([lang_code, this]() {
        // wxGetApp() returns wxApp&; we reach the concrete type via a
        // helper method we expose as a free function in main.cpp.
        extern void LibDeployApp_SwitchLanguage(const std::string&, MainFrame*);
        LibDeployApp_SwitchLanguage(lang_code, this);
    });
}

void MainFrame::RunPackZipAsync(const std::string& zip_path) {
    struct SharedState {
        std::mutex               mtx;
        std::string              msg  = "Collecting files...";
        int                      pct  = 0;
        bool                     done = false;
        bool                     ok   = false;
        std::vector<std::string> errors;
        std::vector<std::string> log_lines;
    };
    auto state = std::make_shared<SharedState>();

    FileDeployer::Options opts;
    opts.copy_redist_dlls = true;

    auto* dlg = new wxProgressDialog(
        _("Packing ZIP"), _("Collecting files..."), 100, this,
        wxPD_APP_MODAL | wxPD_ELAPSED_TIME | wxPD_AUTO_HIDE);
    dlg->Show();

    std::thread worker([state, zip_path, opts, report = m_report]() mutable {
        state->ok = FileDeployer::PackZip(
            report, zip_path, opts, state->errors,
            [&state](const std::string& m, int p) {
                std::lock_guard<std::mutex> lk(state->mtx);
                state->msg = m;
                state->pct = p;
                state->log_lines.push_back(m);
            });
        std::lock_guard<std::mutex> lk(state->mtx);
        state->done = true;
    });

    while (true) {
        wxMilliSleep(50);
        bool is_done; int cur_pct; std::string cur_msg;
        std::vector<std::string> lines;
        {
            std::lock_guard<std::mutex> lk(state->mtx);
            is_done = state->done;
            cur_pct = state->pct;
            cur_msg = state->msg;
            lines.swap(state->log_lines);
        }
        for (const auto& l : lines) Log(wxString::FromUTF8(l));
        dlg->Update(cur_pct, wxString::FromUTF8(cur_msg));
        if (is_done) break;
    }
    worker.join();
    dlg->Destroy();

    for (const auto& e : state->errors) Log(_("ERROR: ") + wxString::FromUTF8(e));
    if (state->ok) {
        wxMessageBox(wxString::Format(_("ZIP created:\n%s"),
                                      wxString::FromUTF8(zip_path)),
                     _("Pack ZIP"), wxOK | wxICON_INFORMATION, this);
    } else {
        wxString err_msg = _("ZIP creation failed.");
        for (const auto& e : state->errors)
            err_msg += "\n" + wxString::FromUTF8(e);
        wxMessageBox(err_msg, _("Pack ZIP"), wxOK | wxICON_ERROR, this);
    }
}

void MainFrame::RunGenInstallerAsync(const std::string& out_path,
                                     const std::string& app_name,
                                     const std::string& makensis_path) {
    struct SharedState {
        std::mutex               mtx;
        std::string              msg  = "Running NSIS...";
        int                      pct  = 0;
        bool                     done = false;
        bool                     ok   = false;
        std::vector<std::string> errors;
        std::vector<std::string> log_lines;
    };
    auto state = std::make_shared<SharedState>();

    FileDeployer::Options opts;
    opts.copy_redist_dlls        = m_cfg.copy_redist_dlls;
    opts.bundle_redist_installer = m_cfg.bundle_redist_installer;

    auto* dlg = new wxProgressDialog(
        _("Generating Installer"), _("Running NSIS..."), 100, this,
        wxPD_APP_MODAL | wxPD_ELAPSED_TIME | wxPD_AUTO_HIDE);
    dlg->Show();

    std::thread worker([state, out_path, app_name, makensis_path, opts,
                        report = m_report]() mutable {
        state->ok = FileDeployer::GenInstaller(
            report, out_path, app_name, makensis_path, opts, state->errors,
            [&state](const std::string& m, int p) {
                std::lock_guard<std::mutex> lk(state->mtx);
                state->msg = m;
                state->pct = p;
                state->log_lines.push_back(m);
            });
        std::lock_guard<std::mutex> lk(state->mtx);
        state->done = true;
    });

    while (true) {
        wxMilliSleep(50);
        bool is_done; int cur_pct; std::string cur_msg;
        std::vector<std::string> lines;
        {
            std::lock_guard<std::mutex> lk(state->mtx);
            is_done = state->done;
            cur_pct = state->pct;
            cur_msg = state->msg;
            lines.swap(state->log_lines);
        }
        for (const auto& l : lines) Log(wxString::FromUTF8(l));
        dlg->Update(cur_pct, wxString::FromUTF8(cur_msg));
        if (is_done) break;
    }
    worker.join();
    dlg->Destroy();

    for (const auto& e : state->errors) Log(_("ERROR: ") + wxString::FromUTF8(e));
    if (state->ok) {
        wxMessageBox(wxString::Format(_("Installer created:\n%s"),
                                      wxString::FromUTF8(out_path)),
                     _("Generate Installer"), wxOK | wxICON_INFORMATION, this);
    } else {
        wxString err_msg = _("Installer generation failed.");
        for (const auto& e : state->errors)
            err_msg += "\n" + wxString::FromUTF8(e);
        wxMessageBox(err_msg, _("Generate Installer"), wxOK | wxICON_ERROR, this);
    }
}

// ── Recent targets ────────────────────────────────────────────────────────────

void MainFrame::PushRecentTarget(const std::string& path) {
    auto& v = m_cfg.recent_targets;
    v.erase(std::remove_if(v.begin(), v.end(), [&](const AppConfig::RecentTarget& rt) {
        return rt.path == path;
    }), v.end());

    AppConfig::RecentTarget rt;
    rt.path = path;
    rt.search_paths = m_cfg.search_paths;
    v.insert(v.begin(), std::move(rt));

    if ((int)v.size() > AppConfig::kMaxRecentTargets)
        v.resize(AppConfig::kMaxRecentTargets);
    UpdateRecentMenu();
}

void MainFrame::UpdateRecentMenu() {
    if (!m_recent_menu) return;
    for (int i = 0; i < AppConfig::kMaxRecentTargets; ++i)
        Disconnect(ID_RECENT_FIRST + i, wxEVT_MENU);
    Disconnect(ID_RECENT_CLEAR, wxEVT_MENU);

    // 清空已有条目（从末尾删，避免索引混乱）
    while (m_recent_menu->GetMenuItemCount() > 0)
        m_recent_menu->Destroy(m_recent_menu->FindItemByPosition(0));

    if (m_cfg.recent_targets.empty()) {
        m_recent_menu->Append(wxID_ANY, _("(empty)"))->Enable(false);
        return;
    }
    for (int i = 0; i < (int)m_cfg.recent_targets.size(); ++i) {
        int id = ID_RECENT_FIRST + i;
        wxString label = wxString::FromUTF8(m_cfg.recent_targets[i].path);
        m_recent_menu->Append(id, label);
        Bind(wxEVT_MENU, [this, i](wxCommandEvent&) {
            if (i < (int)m_cfg.recent_targets.size()) {
                const auto& rt = m_cfg.recent_targets[i];
                wxString p = wxString::FromUTF8(rt.path);
                m_exe_path->SetValue(p);
                m_cfg.last_target = rt.path;
                if (!rt.search_paths.empty()) {
                    m_cfg.search_paths = rt.search_paths;
                    RefreshSearchList();
                }
            }
        }, id);
    }
    m_recent_menu->AppendSeparator();
    m_recent_menu->Append(ID_RECENT_CLEAR, _("Clear Recent"));
    Bind(wxEVT_MENU, [this](wxCommandEvent&) {
        m_cfg.recent_targets.clear();
        ConfigManager::Save(m_cfg);
        UpdateRecentMenu();
    }, ID_RECENT_CLEAR);
}

// ── Log helpers ───────────────────────────────────────────────────────────────

void MainFrame::SaveSessionLog() {
    if (m_current_log_file.empty() || !m_log) return;
    std::ofstream f(m_current_log_file);
    if (f) f << m_log->GetValue().ToStdString();
}

void MainFrame::OnExportLog(wxCommandEvent&) {
    wxString defaultPath = wxString::FromUTF8(
        m_current_log_file.empty()
            ? m_log_dir + "/" +
              wxDateTime::Now().Format("%Y-%m-%d_%H-%M-%S").ToStdString() + "_export.log"
            : m_current_log_file);
    wxFileDialog dlg(this, _("Export Log"), "", defaultPath,
                     _("Log files (*.log)|*.log|Text files (*.txt)|*.txt|All files|*.*"),
                     wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
    if (dlg.ShowModal() == wxID_CANCEL) return;
    std::ofstream f(dlg.GetPath().ToStdString());
    if (f) {
        f << m_log->GetValue().ToStdString();
        wxMessageBox(wxString::Format(_("Log saved to:\n%s"), dlg.GetPath()),
                     _("Export Log"), wxOK | wxICON_INFORMATION, this);
    } else {
        wxMessageBox(_("Cannot write file."), _("Export Log"), wxOK | wxICON_ERROR, this);
    }
}

void MainFrame::OnHistoryLogs(wxCommandEvent&) {
    wxDialog dlg(this, wxID_ANY, _("History Logs"),
                 wxDefaultPosition, wxSize(860, 560),
                 wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    auto* hsplit = new wxSplitterWindow(&dlg, wxID_ANY,
                                        wxDefaultPosition, wxDefaultSize,
                                        wxSP_LIVE_UPDATE | wxSP_3D);

    // 左：文件列表
    auto* fileList = new wxListBox(hsplit, wxID_ANY);
    wxDir dir(wxString::FromUTF8(m_log_dir));
    wxString filename;
    std::vector<wxString> logFiles;
    if (dir.IsOpened() && dir.GetFirst(&filename, "*.log", wxDIR_FILES)) {
        do { logFiles.push_back(filename); } while (dir.GetNext(&filename));
    }
    std::sort(logFiles.rbegin(), logFiles.rend()); // 最新在前
    for (const auto& f : logFiles) fileList->Append(f);

    // 右：内容查看
    auto* viewer = new wxTextCtrl(hsplit, wxID_ANY, "",
                                  wxDefaultPosition, wxDefaultSize,
                                  wxTE_MULTILINE | wxTE_READONLY | wxHSCROLL | wxTE_RICH2);
    viewer->SetFont(wxFont(9, wxFONTFAMILY_TELETYPE,
                           wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL));
    hsplit->SplitVertically(fileList, viewer, 240);
    sizer->Add(hsplit, 1, wxEXPAND | wxALL, 4);

    auto loadLog = [this, viewer](const wxString& name) {
        wxString path = wxString::FromUTF8(m_log_dir) + wxFileName::GetPathSeparator()
                        + name;
        wxFile f(path);
        if (!f.IsOpened()) return;
        wxString content;
        f.ReadAll(&content);
        viewer->SetValue(content);
    };

    fileList->Bind(wxEVT_LISTBOX, [fileList, loadLog](wxCommandEvent&) {
        int sel = fileList->GetSelection();
        if (sel == wxNOT_FOUND) return;
        loadLog(fileList->GetString(sel));
    });

    // 按钮行
    auto* btnRow = new wxBoxSizer(wxHORIZONTAL);
    auto* btnOpen = new wxButton(&dlg, wxID_ANY, _("Open Folder"));
    auto* btnDel  = new wxButton(&dlg, wxID_ANY, _("Delete"));
    auto* btnClose = new wxButton(&dlg, wxID_OK, _("Close"));
    btnOpen->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        wxLaunchDefaultApplication(wxString::FromUTF8(m_log_dir));
    });
    btnDel->Bind(wxEVT_BUTTON, [this, fileList, viewer](wxCommandEvent&) {
        int sel = fileList->GetSelection();
        if (sel == wxNOT_FOUND) return;
        wxString name = fileList->GetString(sel);
        if (wxMessageBox(wxString::Format(_("Delete %s?"), name),
                         _("Delete Log"), wxYES_NO | wxICON_QUESTION, this) != wxYES) return;
        wxRemoveFile(wxString::FromUTF8(m_log_dir) + wxFileName::GetPathSeparator() + name);
        fileList->Delete(sel);
        viewer->Clear();
    });
    btnRow->Add(btnOpen, 0, wxRIGHT, 6);
    btnRow->Add(btnDel,  0, wxRIGHT, 6);
    btnRow->AddStretchSpacer();
    btnRow->Add(btnClose, 0);
    sizer->Add(btnRow, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 6);
    dlg.SetSizer(sizer);

    if (fileList->GetCount() > 0) {
        fileList->SetSelection(0);
        loadLog(fileList->GetString(0));
    }
    dlg.ShowModal();
}

void MainFrame::UpdateRedistWarning(const DepReport& report) {
    if (report.redist_needed.empty()) {
        m_redist_warn->Hide();
        return;
    }

    wxString text;
    for (const auto& r : report.redist_needed) {
        text += "  [!] " + wxString::FromUTF8(r.name);
        if (!r.url.empty())
            text += "\n       " + wxString::FromUTF8(r.url);
        text += "\n";
    }
    m_redist_text->SetValue(text);
    m_redist_warn->Show();
    m_redist_warn->GetParent()->Layout();
}
