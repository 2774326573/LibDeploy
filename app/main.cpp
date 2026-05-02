#include <wx/wx.h>
#include <wx/stdpaths.h>
#include <wx/filename.h>
#include <wx/intl.h>
#include "app/main_frame.h"
#include "config/config_manager.h"

class LibDeployApp : public wxApp {
public:
    bool OnInit() override {
        if (!wxApp::OnInit()) return false;
        SetAppName("LibDeploy");
        ApplyLocale();
        SpawnMainFrame();
        return true;
    }

    // Called by MainFrame when user picks a language from the menu.
    // Saves config, destroys the current locale + frame, rebuilds both.
    void SwitchLanguage(const std::string& lang_code, MainFrame* sender) {
        // 1. Persist choice
        AppConfig cfg = ConfigManager::Load();
        cfg.ui_language = lang_code;
        ConfigManager::Save(cfg);

        // 2. Rebuild locale (delete old first – wxLocale sets a global ptr)
        delete m_locale;
        m_locale = nullptr;
        m_lang_code = lang_code;
        ApplyLocale();

        // 3. Close current frame (Destroy schedules deletion after event loop)
        sender->Destroy();

        // 4. Open a fresh frame whose strings are now fetched from new catalogue
        SpawnMainFrame();
    }

    int OnExit() override {
        delete m_locale;
        return wxApp::OnExit();
    }

private:
    wxLocale*   m_locale    = nullptr;
    std::string m_lang_code;           // "" = auto

    void ApplyLocale() {
        // Determine which language to use
        if (m_lang_code.empty()) {
            AppConfig cfg = ConfigManager::Load();
            m_lang_code = cfg.ui_language;
        }

        wxLanguage lang_id = wxLANGUAGE_DEFAULT;
        if (m_lang_code == "zh_CN")
            lang_id = wxLANGUAGE_CHINESE_SIMPLIFIED;
        else if (m_lang_code == "en_US")
            lang_id = wxLANGUAGE_ENGLISH_US;

        m_locale = new wxLocale;
        m_locale->Init(lang_id, wxLOCALE_DONT_LOAD_DEFAULT);

        // <exe dir>/locale/<lang>/LibDeploy.mo
        wxFileName exe(wxStandardPaths::Get().GetExecutablePath());
        wxString locale_dir = exe.GetPath() + wxFILE_SEP_PATH + "locale";
        m_locale->AddCatalogLookupPathPrefix(locale_dir);
        m_locale->AddCatalog("LibDeploy");
    }

    void SpawnMainFrame() {
        auto* frame = new MainFrame(_("LibDeploy - Dependency Analyzer"));
        frame->Show(true);
        SetTopWindow(frame);
    }
};

wxIMPLEMENT_APP(LibDeployApp);

// Free-function bridge so main_frame.cpp can trigger a language switch
// without a circular header dependency on LibDeployApp.
void LibDeployApp_SwitchLanguage(const std::string& lang_code, MainFrame* sender) {
    static_cast<LibDeployApp&>(wxGetApp()).SwitchLanguage(lang_code, sender);
}
