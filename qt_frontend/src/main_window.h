#pragma once

#include <QFutureWatcher>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProgressDialog>
#include <QPushButton>
#include <QTextEdit>
#include <QTimer>
#include <QTreeWidget>
#include <QComboBox>
#include <QGroupBox>
#include <QTranslator>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>

#include "config/config_manager.h"
#include "engine/dep_result.h"

#include <memory>
#include <unordered_map>
#include <unordered_set>

// Forward declarations
class MainWindow;

// Custom QListWidget to handle drag-drop for excluded directories
class ExcludedDirsListWidget : public QListWidget {
public:
    explicit ExcludedDirsListWidget(MainWindow* parent = nullptr);

protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    MainWindow* m_mainWindow = nullptr;
};

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override = default;
    void addExcludedDirFromDrag(const QString& text);

private:
    struct AnalyzeResult {
        DepReport report;
        QStringList logs;
    };

    struct DeployResult {
        int copied = 0;
        QStringList errors;
        QStringList logs;
    };

    struct ZipResult {
        bool ok = false;
        QString zipPath;
        QStringList errors;
        QStringList logs;
    };

    struct InstallerResult {
        bool ok = false;
        QString outPath;
        QStringList errors;
        QStringList logs;
    };

    QLineEdit*      m_exePath = nullptr;
    QComboBox*      m_targetOs = nullptr;
    QTreeWidget*    m_tree = nullptr;
    QListWidget*    m_searchPaths = nullptr;
    QListWidget*    m_excludedDirs = nullptr;
    QPlainTextEdit* m_log = nullptr;
    QGroupBox*      m_redistPanel = nullptr;
    QTextEdit*      m_redistText = nullptr;
    QLabel*         m_detailName = nullptr;
    QLabel*         m_detailPath = nullptr;
    QLabel*         m_detailCategory = nullptr;
    QLabel*         m_exeLabel = nullptr;
    QLabel*         m_targetOsLabel = nullptr;
    QLabel*         m_tipLabel = nullptr;
    QLabel*         m_logLabel = nullptr;
    QLabel*         m_noticeLabel = nullptr;
    QGroupBox*      m_detailGroup = nullptr;
    QGroupBox*      m_pathsGroup = nullptr;
    QGroupBox*      m_excludedGroup = nullptr;
    QLabel*         m_excludedTipLabel = nullptr;

    QMenu*          m_recentMenu = nullptr;

    QPushButton*    m_browseButton = nullptr;
    QPushButton*    m_analyzeButton = nullptr;
    QPushButton*    m_deployButton = nullptr;
    QPushButton*    m_reportButton = nullptr;
    QPushButton*    m_copyButton = nullptr;
    QPushButton*    m_zipButton = nullptr;
    QPushButton*    m_installerButton = nullptr;
    QPushButton*    m_addPathButton = nullptr;
    QPushButton*    m_removePathButton = nullptr;
    QPushButton*    m_upPathButton = nullptr;
    QPushButton*    m_downPathButton = nullptr;
    QPushButton*    m_addExcludedButton = nullptr;
    QPushButton*    m_removeExcludedButton = nullptr;
    QPushButton*    m_clearExcludedButton = nullptr;

    AppConfig m_config;
    DepReport m_report;
    bool      m_hasReport = false;
    QString   m_currentLanguage;
    QString   m_currentTheme;
    QString   m_logDir;          // logs/ directory next to exe
    QString   m_currentLogFile;  // log file path for current session
    std::unique_ptr<QTranslator> m_translator;

    QPointer<QProgressDialog>        m_progress;
    std::uint64_t                    m_progressCookie = 0;
    std::uint64_t                    m_noticeCookie = 0;
    QFutureWatcher<AnalyzeResult>   m_analyzeWatcher;
    QFutureWatcher<DeployResult>    m_deployWatcher;
    QFutureWatcher<ZipResult>       m_zipWatcher;
    QFutureWatcher<InstallerResult> m_installerWatcher;

    void buildMenus();
    void buildUi();
    void loadConfig();
    void saveConfig();
    void initializeLanguage();
    void setLanguage(const QString& language);
    void initializeTheme();
    void setTheme(const QString& theme);
    void applyTheme();
    void retranslateUi();

    void openExecutable();
    void analyze();
    void deployToDefault();
    void copyAll();
    void packZip();
    void generateInstaller();
    void runQtDeploy();
    void showReportDialog();
    void exportLog();
    void showHistoryLogs();
    void addSearchPath();
    void removeSearchPath();
    void moveSearchPath(int direction);
    void addExcludedDir();
    void removeExcludedDir();
    void clearExcludedDirs();

    void startAnalyze(bool runQtDeployTool = false);
    void startDeploy(const QString& destDir);

    void populateTree();
    void addTreeNode(QTreeWidgetItem* parent,
                     const std::shared_ptr<DepNode>& node,
                     std::unordered_set<std::string>& expanded);
    void updateDetails(QTreeWidgetItem* item);
    void updateRedistPanel();
    void refreshSearchPaths();
    void refreshExcludedDirs();
    void updateRecentMenu();
    void pushRecentTarget(const QString& path);
    void saveSessionLog();
    QString logDir() const;
    void setBusy(bool busy, const QString& title = {}, const QString& text = {}, int maxValue = 0);
    void appendLog(const QString& text);
    void showNotice(const QString& text, bool isError = false, int timeoutMs = 8000);

    TargetOs selectedTargetOs() const;
    QString categoryText(DllCategory category) const;
    QString statusText(DepStatus status) const;
    QIcon iconForNode(const DepNode& node) const;
    QString buildFullReport() const;
    void collectParentMap(const DepNode& node,
                          std::unordered_map<std::string, std::vector<std::string>>& parents,
                          std::unordered_set<std::string>& visited) const;

    static QString qstr(const std::string& value);
    static std::string str(const QString& value);
};
