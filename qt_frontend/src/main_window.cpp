#include "main_window.h"

#include "engine/dep_resolver.h"
#include "engine/file_deployer.h"

#include <QtConcurrent/QtConcurrent>
#include <QApplication>
#include <QBoxLayout>
#include <QClipboard>
#include <QColor>
#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QGroupBox>
#include <QHeaderView>
#include <QInputDialog>
#include <QLocale>
#include <QMenuBar>
#include <QMessageBox>
#include <QActionGroup>
#include <QPalette>
#include <QSplitter>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStyle>
#include <QStyleFactory>
#include <QTextStream>
#include <QTime>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace {
constexpr int NodePtrRole = Qt::UserRole + 1;

QString appDir()
{
    return QFileInfo(QCoreApplication::applicationFilePath()).absolutePath();
}

QString runtimeConfigPath()
{
    const QString configRoot = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    if (configRoot.isEmpty())
        return appDir() + "/libdeploy_config.json";
    return QDir(configRoot).absoluteFilePath("libdeploy_config.json");
}

QString bundledConfigPath()
{
    return appDir() + "/libdeploy_config.json";
}

QString defaultDeployDir(const QString& exePath)
{
    QFileInfo info(exePath);
    return info.absoluteDir().absoluteFilePath(info.completeBaseName() + "_deploy");
}
}

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    m_logDir = appDir() + "/logs";
    QDir().mkpath(m_logDir);

    loadConfig();
    initializeLanguage();
    initializeTheme();
    buildMenus();
    buildUi();
    resize(1180, 760);
    setMinimumSize(900, 560);
    retranslateUi();

    connect(&m_analyzeWatcher, &QFutureWatcher<AnalyzeResult>::finished, this, [this]() {
        AnalyzeResult result = m_analyzeWatcher.result();
        for (const QString& line : result.logs) appendLog(line);
        m_report = std::move(result.report);
        m_hasReport = true;
        populateTree();
        updateRedistPanel();
        for (const auto& warning : m_report.warnings)
            appendLog(tr("WARN: ") + qstr(warning));
        const QString status = tr("Found: %1  Missing: %2  Redist: %3")
            .arg(m_report.to_copy.size())
            .arg(m_report.missing.size())
            .arg(m_report.maybe_absent.size());
        statusBar()->showMessage(status);
        appendLog(tr("Done. ") + status);
        saveSessionLog();
        setBusy(false);
    });

    connect(&m_deployWatcher, &QFutureWatcher<DeployResult>::finished, this, [this]() {
        DeployResult result = m_deployWatcher.result();
        for (const QString& line : result.logs) appendLog(line);
        for (const QString& err : result.errors) appendLog(tr("ERROR: ") + err);
        setBusy(false);

        QString summary = tr("Files copied: %1\nResource dirs: %2\nData files: %3")
            .arg(result.copied)
            .arg(m_report.resource_dirs.size())
            .arg(m_report.data_files.size());
        if (!m_report.missing.empty())
            summary += tr("\n\nMissing DLLs: %1").arg(m_report.missing.size());
        if (!result.errors.empty())
            summary += tr("\nErrors: %1 - check the log.").arg(result.errors.size());
        statusBar()->showMessage(tr("Deploy complete."), 10000);
        showNotice(tr("Deploy complete. Files copied: %1").arg(result.copied),
                   !result.errors.empty());
        appendLog(summary);
    });

    connect(&m_zipWatcher, &QFutureWatcher<ZipResult>::finished, this, [this]() {
        ZipResult result = m_zipWatcher.result();
        for (const QString& line : result.logs) appendLog(line);
        for (const QString& err : result.errors) appendLog(tr("ERROR: ") + err);
        setBusy(false);
        if (result.ok) {
            const QString message = tr("ZIP created: %1").arg(result.zipPath);
            statusBar()->showMessage(message, 15000);
            showNotice(message);
            appendLog(message);
        } else {
            const QString message = tr("ZIP creation failed.");
            statusBar()->showMessage(message, 15000);
            showNotice(message, true);
            appendLog(message);
        }
    });

    connect(&m_installerWatcher, &QFutureWatcher<InstallerResult>::finished, this, [this]() {
        InstallerResult result = m_installerWatcher.result();
        for (const QString& line : result.logs) appendLog(line);
        for (const QString& err : result.errors) appendLog(tr("ERROR: ") + err);
        setBusy(false);
        if (result.ok) {
            const QString message = tr("Installer created: %1").arg(result.outPath);
            statusBar()->showMessage(message, 15000);
            showNotice(message);
            appendLog(message);
        } else {
            const QString message = tr("Installer generation failed.");
            statusBar()->showMessage(message, 15000);
            showNotice(message, true);
            appendLog(message);
        }
    });
}

void MainWindow::buildMenus()
{
    menuBar()->clear();
    QMenu* file = menuBar()->addMenu(tr("&File"));
    file->addAction(tr("&Open EXE...\tCtrl+O"), this, &MainWindow::openExecutable, QKeySequence::Open);
    file->addAction(tr("&Analyze\tF5"), this, &MainWindow::analyze, Qt::Key_F5);
    file->addSeparator();
    m_recentMenu = file->addMenu(tr("Recent Files"));
    updateRecentMenu();
    file->addSeparator();
    file->addAction(tr("&Export Log..."), this, &MainWindow::exportLog);
    file->addAction(tr("&History Logs..."), this, &MainWindow::showHistoryLogs);
    file->addSeparator();
    file->addAction(tr("E&xit"), this, &QWidget::close);

    QMenu* tools = menuBar()->addMenu(tr("&Tools"));
    tools->addAction(tr("&Copy All Dependencies"), this, &MainWindow::copyAll);
    tools->addAction(tr("Pack &ZIP..."), this, &MainWindow::packZip);
    tools->addAction(tr("Generate &Installer..."), this, &MainWindow::generateInstaller);
    tools->addSeparator();
    tools->addAction(tr("Run &Qt Deploy Tool..."), this, &MainWindow::runQtDeploy);

    QMenu* language = menuBar()->addMenu(tr("&Language"));
    QActionGroup* languageGroup = new QActionGroup(this);
    QAction* english = language->addAction(tr("English"));
    QAction* chinese = language->addAction(tr("Chinese (Simplified)"));
    english->setCheckable(true);
    chinese->setCheckable(true);
    languageGroup->addAction(english);
    languageGroup->addAction(chinese);
    const QString currentLanguage = qstr(m_config.ui_language);
    if (currentLanguage == "zh_CN" || m_currentLanguage == "zh_CN") chinese->setChecked(true);
    else english->setChecked(true);
    connect(english, &QAction::triggered, this, [this]() {
        setLanguage("en_US");
    });
    connect(chinese, &QAction::triggered, this, [this]() {
        setLanguage("zh_CN");
    });

    QMenu* theme = menuBar()->addMenu(tr("&Theme"));
    QActionGroup* themeGroup = new QActionGroup(this);
    QAction* systemTheme = theme->addAction(tr("System"));
    QAction* lightTheme = theme->addAction(tr("Light"));
    QAction* darkTheme = theme->addAction(tr("Dark"));
    for (QAction* action : {systemTheme, lightTheme, darkTheme}) {
        action->setCheckable(true);
        themeGroup->addAction(action);
    }
    if (m_currentTheme == "dark") darkTheme->setChecked(true);
    else if (m_currentTheme == "light") lightTheme->setChecked(true);
    else systemTheme->setChecked(true);
    connect(systemTheme, &QAction::triggered, this, [this]() { setTheme("system"); });
    connect(lightTheme, &QAction::triggered, this, [this]() { setTheme("light"); });
    connect(darkTheme, &QAction::triggered, this, [this]() { setTheme("dark"); });

    QMenu* help = menuBar()->addMenu(tr("&Help"));
    help->addAction(tr("&About"), this, [this]() {
        QMessageBox::information(this, tr("About"),
            tr("LibDeployQt v0.1\nQt frontend for dependency analysis and deployment."));
    });
}

void MainWindow::buildUi()
{
    QWidget* central = new QWidget(this);
    QVBoxLayout* root = new QVBoxLayout(central);
    root->setContentsMargins(8, 8, 8, 8);

    QHBoxLayout* top = new QHBoxLayout;
    m_exeLabel = new QLabel;
    top->addWidget(m_exeLabel);
    m_exePath = new QLineEdit(qstr(m_config.last_target));
    m_exePath->setReadOnly(true);
    top->addWidget(m_exePath, 1);

    m_browseButton = new QPushButton;
    connect(m_browseButton, &QPushButton::clicked, this, &MainWindow::openExecutable);
    top->addWidget(m_browseButton);

    m_targetOsLabel = new QLabel;
    top->addWidget(m_targetOsLabel);
    m_targetOs = new QComboBox;
    m_targetOs->setCurrentIndex(static_cast<int>(m_config.target_os));
    top->addWidget(m_targetOs);

    m_analyzeButton = new QPushButton;
    m_deployButton = new QPushButton;
    m_reportButton = new QPushButton;
    m_copyButton = new QPushButton;
    m_zipButton = new QPushButton;
    m_installerButton = new QPushButton;

    connect(m_analyzeButton, &QPushButton::clicked, this, &MainWindow::analyze);
    connect(m_deployButton, &QPushButton::clicked, this, &MainWindow::deployToDefault);
    connect(m_reportButton, &QPushButton::clicked, this, &MainWindow::showReportDialog);
    connect(m_copyButton, &QPushButton::clicked, this, &MainWindow::copyAll);
    connect(m_zipButton, &QPushButton::clicked, this, &MainWindow::packZip);
    connect(m_installerButton, &QPushButton::clicked, this, &MainWindow::generateInstaller);

    top->addWidget(m_analyzeButton);
    top->addWidget(m_deployButton);
    top->addWidget(m_reportButton);
    top->addWidget(m_copyButton);
    top->addWidget(m_zipButton);
    top->addWidget(m_installerButton);
    root->addLayout(top);

    QSplitter* splitter = new QSplitter(Qt::Horizontal, this);
    m_tree = new QTreeWidget;
    m_tree->setColumnCount(3);
    m_tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_tree->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    connect(m_tree, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* item) {
        updateDetails(item);
    });
    splitter->addWidget(m_tree);

    QWidget* right = new QWidget;
    QVBoxLayout* side = new QVBoxLayout(right);

    m_detailGroup = new QGroupBox;
    QVBoxLayout* detailLayout = new QVBoxLayout(m_detailGroup);
    m_detailName = new QLabel;
    m_detailPath = new QLabel;
    m_detailPath->setWordWrap(true);
    m_detailCategory = new QLabel;
    detailLayout->addWidget(m_detailName);
    detailLayout->addWidget(m_detailPath);
    detailLayout->addWidget(m_detailCategory);
    side->addWidget(m_detailGroup);

    m_pathsGroup = new QGroupBox;
    QVBoxLayout* pathLayout = new QVBoxLayout(m_pathsGroup);
    m_searchPaths = new QListWidget;
    pathLayout->addWidget(m_searchPaths);
    QHBoxLayout* pathButtons = new QHBoxLayout;
    m_addPathButton = new QPushButton;
    m_removePathButton = new QPushButton;
    m_upPathButton = new QPushButton;
    m_downPathButton = new QPushButton;
    connect(m_addPathButton, &QPushButton::clicked, this, &MainWindow::addSearchPath);
    connect(m_removePathButton, &QPushButton::clicked, this, &MainWindow::removeSearchPath);
    connect(m_upPathButton, &QPushButton::clicked, this, [this]() { moveSearchPath(-1); });
    connect(m_downPathButton, &QPushButton::clicked, this, [this]() { moveSearchPath(1); });
    pathButtons->addWidget(m_addPathButton);
    pathButtons->addWidget(m_removePathButton);
    pathButtons->addWidget(m_upPathButton);
    pathButtons->addWidget(m_downPathButton);
    pathLayout->addLayout(pathButtons);
    m_tipLabel = new QLabel;
    m_tipLabel->setWordWrap(true);
    pathLayout->addWidget(m_tipLabel);
    side->addWidget(m_pathsGroup);

    m_redistPanel = new QGroupBox;
    QVBoxLayout* redistLayout = new QVBoxLayout(m_redistPanel);
    m_redistText = new QTextEdit;
    m_redistText->setReadOnly(true);
    m_redistText->setMaximumHeight(120);
    redistLayout->addWidget(m_redistText);
    m_redistPanel->hide();
    side->addWidget(m_redistPanel);
    side->addStretch(1);
    splitter->addWidget(right);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);
    root->addWidget(splitter, 1);

    m_noticeLabel = new QLabel;
    m_noticeLabel->setWordWrap(true);
    m_noticeLabel->hide();
    root->addWidget(m_noticeLabel);

    m_logLabel = new QLabel;
    root->addWidget(m_logLabel);
    m_log = new QPlainTextEdit;
    m_log->setReadOnly(true);
    m_log->setMaximumHeight(150);
    root->addWidget(m_log);

    setCentralWidget(central);
    refreshSearchPaths();
}

void MainWindow::loadConfig()
{
    const QString userConfig = runtimeConfigPath();
    if (QFileInfo::exists(userConfig)) {
        m_config = ConfigManager::Load(str(userConfig));
        return;
    }

    m_config = ConfigManager::Load(str(bundledConfigPath()));
}

void MainWindow::saveConfig()
{
    const QString userConfig = runtimeConfigPath();
    QDir().mkpath(QFileInfo(userConfig).absolutePath());
    ConfigManager::Save(m_config, str(userConfig));
}

void MainWindow::initializeLanguage()
{
    QString language = qstr(m_config.ui_language);
    if (language.isEmpty()) {
        language = QLocale::system().name().startsWith("zh") ? "zh_CN" : "en_US";
    }
    setLanguage(language);
}

void MainWindow::setLanguage(const QString& language)
{
    QString next = language == "zh_CN" ? "zh_CN" : "en_US";
    if (m_currentLanguage == next && qstr(m_config.ui_language) == next) return;

    if (m_translator) {
        qApp->removeTranslator(m_translator.get());
        m_translator.reset();
    }
    if (next == "zh_CN") {
        const QString translationDir =
            QFileInfo(QCoreApplication::applicationFilePath()).absoluteDir().absoluteFilePath("translations");
        auto translator = std::make_unique<QTranslator>();
        if (translator->load("LibDeployQt_zh_CN", translationDir)) {
            qApp->installTranslator(translator.get());
            m_translator = std::move(translator);
        }
    }

    m_currentLanguage = next;
    m_config.ui_language = str(next);
    saveConfig();

    if (centralWidget()) {
        buildMenus();
        retranslateUi();
        populateTree();
        updateDetails(m_tree ? m_tree->currentItem() : nullptr);
        updateRedistPanel();
        statusBar()->showMessage(tr("Language changed."));
    }
}

void MainWindow::initializeTheme()
{
    QString theme = qstr(m_config.ui_theme);
    if (theme != "light" && theme != "dark") theme = "system";
    m_currentTheme = theme;
    applyTheme();
}

void MainWindow::setTheme(const QString& theme)
{
    QString next = theme == "dark" ? "dark" : (theme == "light" ? "light" : "system");
    if (m_currentTheme == next && qstr(m_config.ui_theme) == next) return;
    m_currentTheme = next;
    m_config.ui_theme = str(next);
    saveConfig();
    applyTheme();
    buildMenus();
    statusBar()->showMessage(tr("Theme changed."));
}

void MainWindow::applyTheme()
{
    qApp->setStyle(QStyleFactory::create("Fusion"));

    if (m_currentTheme == "dark") {
        QPalette palette;
        palette.setColor(QPalette::Window, QColor(32, 34, 37));
        palette.setColor(QPalette::WindowText, QColor(232, 235, 239));
        palette.setColor(QPalette::Base, QColor(24, 26, 29));
        palette.setColor(QPalette::AlternateBase, QColor(39, 42, 47));
        palette.setColor(QPalette::Text, QColor(232, 235, 239));
        palette.setColor(QPalette::Button, QColor(43, 46, 52));
        palette.setColor(QPalette::ButtonText, QColor(232, 235, 239));
        palette.setColor(QPalette::BrightText, Qt::white);
        palette.setColor(QPalette::Highlight, QColor(0, 120, 215));
        palette.setColor(QPalette::HighlightedText, Qt::white);
        palette.setColor(QPalette::Disabled, QPalette::Text, QColor(125, 130, 138));
        palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(125, 130, 138));
        qApp->setPalette(palette);
        qApp->setStyleSheet(
            "QToolTip { color: #f1f3f5; background-color: #2b2f36; border: 1px solid #5b616b; }"
            "QTreeWidget, QListWidget, QPlainTextEdit, QTextEdit, QLineEdit {"
            "  background: #181a1d; color: #e8ebef; border: 1px solid #4a5059;"
            "}"
            "QHeaderView::section { background: #2b2f36; color: #e8ebef; border: 1px solid #4a5059; padding: 4px; }"
            "QGroupBox { border: 1px solid #4a5059; margin-top: 8px; padding-top: 8px; }"
            "QGroupBox::title { subcontrol-origin: margin; left: 8px; padding: 0 3px; }"
            "QPushButton { padding: 4px 10px; }"
        );
        return;
    }

    if (m_currentTheme == "light") {
        QPalette palette;
        palette.setColor(QPalette::Window, QColor(245, 247, 250));
        palette.setColor(QPalette::WindowText, QColor(20, 24, 28));
        palette.setColor(QPalette::Base, Qt::white);
        palette.setColor(QPalette::AlternateBase, QColor(238, 242, 247));
        palette.setColor(QPalette::Text, QColor(20, 24, 28));
        palette.setColor(QPalette::Button, QColor(241, 244, 248));
        palette.setColor(QPalette::ButtonText, QColor(20, 24, 28));
        palette.setColor(QPalette::Highlight, QColor(0, 120, 215));
        palette.setColor(QPalette::HighlightedText, Qt::white);
        qApp->setPalette(palette);
        qApp->setStyleSheet(
            "QTreeWidget, QListWidget, QPlainTextEdit, QTextEdit, QLineEdit { border: 1px solid #c8d0d9; }"
            "QHeaderView::section { background: #eef2f7; border: 1px solid #c8d0d9; padding: 4px; }"
            "QGroupBox { border: 1px solid #c8d0d9; margin-top: 8px; padding-top: 8px; }"
            "QGroupBox::title { subcontrol-origin: margin; left: 8px; padding: 0 3px; }"
            "QPushButton { padding: 4px 10px; }"
        );
        return;
    }

    qApp->setPalette(qApp->style()->standardPalette());
    qApp->setStyleSheet(QString());
}

void MainWindow::retranslateUi()
{
    setWindowTitle(tr("LibDeployQt - Dependency Analyzer"));
    if (m_exeLabel) m_exeLabel->setText(tr("EXE:"));
    if (m_browseButton) m_browseButton->setText(tr("Browse..."));
    if (m_targetOsLabel) m_targetOsLabel->setText(tr("Target OS:"));
    if (m_targetOs) {
        const int index = m_targetOs->currentIndex() < 0 ? static_cast<int>(m_config.target_os) : m_targetOs->currentIndex();
        m_targetOs->clear();
        m_targetOs->addItems({tr("Windows 7"), tr("Windows 8.1"), tr("Windows 10"), tr("Windows 11")});
        m_targetOs->setCurrentIndex(std::clamp(index, 0, 3));
    }
    if (m_analyzeButton) m_analyzeButton->setText(tr("Analyze"));
    if (m_deployButton) m_deployButton->setText(tr("Deploy"));
    if (m_reportButton) m_reportButton->setText(tr("Report"));
    if (m_copyButton) m_copyButton->setText(tr("Copy All"));
    if (m_zipButton) m_zipButton->setText(tr("Pack ZIP"));
    if (m_installerButton) m_installerButton->setText(tr("Installer"));
    if (m_tree) m_tree->setHeaderLabels({tr("Dependency"), tr("Status"), tr("Category")});
    if (m_detailGroup) m_detailGroup->setTitle(tr("Detail"));
    if (m_detailName && !m_tree->currentItem()) m_detailName->setText(tr("(select a node)"));
    if (m_pathsGroup) m_pathsGroup->setTitle(tr("Search Paths"));
    if (m_addPathButton) m_addPathButton->setText(tr("Add"));
    if (m_removePathButton) m_removePathButton->setText(tr("Del"));
    if (m_upPathButton) m_upPathButton->setText(tr("Up"));
    if (m_downPathButton) m_downPathButton->setText(tr("Down"));
    if (m_tipLabel) m_tipLabel->setText(tr("Tip: this app's folder is searched first; system PATH is controlled by config."));
    if (m_redistPanel) m_redistPanel->setTitle(tr("Redistributable Requirements:"));
    if (m_logLabel) m_logLabel->setText(tr("Log:"));
    if (statusBar()->currentMessage().isEmpty()) statusBar()->showMessage(tr("Ready"));
}

void MainWindow::openExecutable()
{
    QString path = QFileDialog::getOpenFileName(
        this, tr("Select executable"), QString(),
        tr("Executables (*.exe;*.dll)|*.exe;*.dll|All files|*.*"));
    if (path.isEmpty()) return;
    m_exePath->setText(path);
    m_config.last_target = str(path);
    pushRecentTarget(path);
    saveConfig();
}

void MainWindow::analyze()
{
    startAnalyze(false);
}

void MainWindow::startAnalyze(bool runQtDeployTool)
{
    const QString path = m_exePath->text();
    if (path.isEmpty()) {
        QMessageBox::warning(this, tr("LibDeployQt"), tr("Please select an executable first."));
        return;
    }

    m_config.last_target = str(path);
    m_config.target_os = selectedTargetOs();
    pushRecentTarget(path);
    saveConfig();
    m_hasReport = false;
    m_report = DepReport{};
    // 新建当次会话日志文件名：logs/YYYY-MM-DD_HH-MM-SS_ExeName.log
    QString exeName = QFileInfo(path).completeBaseName();
    m_currentLogFile = m_logDir + "/" +
        QDateTime::currentDateTime().toString("yyyy-MM-dd_HH-mm-ss") +
        "_" + exeName + ".log";
    m_tree->clear();
    m_redistPanel->hide();
    appendLog(tr("Analyzing: ") + path);
    setBusy(true, tr("Analyzing"), tr("Initializing..."));

    AppConfig cfg = m_config;
    QString exePath = path;
    m_analyzeWatcher.setFuture(QtConcurrent::run([cfg, exePath, runQtDeployTool]() {
        AnalyzeResult result;
        std::vector<std::string> paths;
        paths.push_back(str(appDir()));
        for (const auto& p : cfg.search_paths) paths.push_back(p);

        std::vector<std::string> resourcePaths = cfg.search_paths;
        DepResolver resolver(cfg.target_os, paths, cfg.follow_system_path,
            [&result](const std::string& msg) {
                result.logs.push_back(qstr(msg));
            },
            resourcePaths);
        for (const auto& name : cfg.extra_os_core) resolver.AddOsCore(name);
        for (const auto& rule : cfg.extra_redist)
            resolver.AddRedistRule(rule.prefix, rule.package, rule.always_deploy);
        if (!cfg.user_excluded.empty()) resolver.SetUserExcluded(cfg.user_excluded);
        if (!cfg.extra_excluded_dirs.empty()) resolver.SetExtraExcludedDirs(cfg.extra_excluded_dirs);
        if (!cfg.qt_deploy_tool.empty()) resolver.SetQtDeployTool(cfg.qt_deploy_tool);
        resolver.SetRunQtDeployTool(runQtDeployTool);

        result.report = resolver.Resolve(str(exePath));
        return result;
    }));
}

void MainWindow::deployToDefault()
{
    if (!m_hasReport) {
        QMessageBox::warning(this, tr("LibDeployQt"), tr("Run Analyze first."));
        return;
    }
    startDeploy(defaultDeployDir(qstr(m_report.target_exe)));
}

void MainWindow::copyAll()
{
    if (!m_hasReport) {
        QMessageBox::warning(this, tr("LibDeployQt"), tr("Run Analyze first."));
        return;
    }
    QString dest = QFileDialog::getExistingDirectory(
        this, tr("Select deployment destination directory"),
        defaultDeployDir(qstr(m_report.target_exe)));
    if (dest.isEmpty()) return;
    startDeploy(dest);
}

void MainWindow::startDeploy(const QString& destDir)
{
    setBusy(true, tr("Deploying"), tr("Starting..."));
    DepReport report = m_report;
    AppConfig cfg = m_config;
    m_deployWatcher.setFuture(QtConcurrent::run([report, cfg, destDir]() {
        DeployResult result;
        FileDeployer::Options opts;
        opts.copy_redist_dlls = cfg.copy_redist_dlls;
        opts.overwrite = true;
        std::vector<std::string> errors;
        result.copied = FileDeployer::Deploy(report, str(destDir), opts,
            errors,
            [&result](const std::string& msg, int) {
                result.logs.push_back(qstr(msg));
            });
        for (const auto& err : errors) result.errors.push_back(qstr(err));
        return result;
    }));
}

void MainWindow::packZip()
{
    if (!m_hasReport) {
        QMessageBox::warning(this, tr("LibDeployQt"), tr("Run Analyze first."));
        return;
    }
    QFileInfo exe(qstr(m_report.target_exe));
    QString stamp = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
    QString zipPath = exe.absoluteDir().absoluteFilePath(
        exe.completeBaseName() + "_" + stamp + ".zip");
    appendLog(tr("Packing ZIP: ") + zipPath);
    setBusy(true, tr("Packing ZIP"), tr("Collecting files..."), 100);

    DepReport report = m_report;
    const std::uint64_t progressCookie = m_progressCookie;
    m_zipWatcher.setFuture(QtConcurrent::run([report, zipPath, this, progressCookie]() {
        ZipResult result;
        result.zipPath = zipPath;
        FileDeployer::Options opts;
        opts.copy_redist_dlls = true;
        std::vector<std::string> errors;
        int lastPct = -1;
        result.ok = FileDeployer::PackZip(report, str(zipPath), opts, errors,
            [&result, this, &lastPct, progressCookie](const std::string& msg, int pct) {
                if (pct == lastPct && pct < 100)
                    return;
                lastPct = pct;
                result.logs.push_back(qstr(msg));
                QMetaObject::invokeMethod(this, [this, msg, pct, progressCookie]() {
                    if (progressCookie == m_progressCookie && m_progress) {
                        m_progress->setValue(pct);
                        m_progress->setLabelText(QString::fromUtf8(msg.c_str()));
                    }
                }, Qt::QueuedConnection);
            });
        for (const auto& e : errors) result.errors.push_back(qstr(e));
        return result;
    }));
}

void MainWindow::generateInstaller()
{
    if (!m_hasReport) {
        QMessageBox::warning(this, tr("LibDeployQt"), tr("Run Analyze first."));
        return;
    }

    QString nsis = appDir() + "/nsis/Bin/makensis.exe";
    if (!QFileInfo::exists(nsis)) nsis = qstr(m_config.makensis_path);
    if (!QFileInfo::exists(nsis)) {
        QMessageBox::warning(this, tr("NSIS Not Found"),
            tr("makensis.exe not found.\nBundled NSIS should be in nsis/ next to LibDeployQt.exe."));
        return;
    }

    QFileInfo exe(qstr(m_report.target_exe));
    QString appName = exe.completeBaseName();
    QString stamp = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
    QString out = exe.absoluteDir().absoluteFilePath(appName + "_Setup_" + stamp + ".exe");
    appendLog(tr("Generating installer: ") + out);
    setBusy(true, tr("Generating Installer"), tr("Running NSIS..."));

    DepReport report = m_report;
    AppConfig cfg = m_config;
    m_installerWatcher.setFuture(QtConcurrent::run([report, cfg, out, appName, nsis]() {
        InstallerResult result;
        result.outPath = out;
        FileDeployer::Options opts;
        opts.copy_redist_dlls = cfg.copy_redist_dlls;
        opts.bundle_redist_installer = cfg.bundle_redist_installer;
        std::vector<std::string> errors;
        result.ok = FileDeployer::GenInstaller(report, str(out), str(appName), str(nsis), opts, errors,
            [&result](const std::string& msg, int) {
                result.logs.push_back(qstr(msg));
            });
        for (const auto& e : errors) result.errors.push_back(qstr(e));
        return result;
    }));
}

void MainWindow::runQtDeploy()
{
    QString tool = QInputDialog::getText(this, tr("Qt Deploy Tool"),
        tr("Path to windeployqt / linuxdeployqt (leave empty to auto-detect):"),
        QLineEdit::Normal, qstr(m_config.qt_deploy_tool));
    m_config.qt_deploy_tool = str(tool.trimmed());
    saveConfig();
    appendLog(tr("Running Qt deploy tool..."));
    startAnalyze(true);
}

void MainWindow::showReportDialog()
{
    if (!m_hasReport) {
        QMessageBox::warning(this, tr("LibDeployQt"), tr("Run Analyze first."));
        return;
    }
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Full Dependency Report"));
    dialog.resize(860, 640);
    QVBoxLayout* layout = new QVBoxLayout(&dialog);
    QPlainTextEdit* text = new QPlainTextEdit(buildFullReport());
    text->setReadOnly(true);
    text->setLineWrapMode(QPlainTextEdit::NoWrap);
    layout->addWidget(text, 1);
    QHBoxLayout* buttons = new QHBoxLayout;
    QPushButton* copy = new QPushButton(tr("Copy to Clipboard"));
    QPushButton* close = new QPushButton(tr("Close"));
    connect(copy, &QPushButton::clicked, this, [text]() {
        QApplication::clipboard()->setText(text->toPlainText());
    });
    connect(close, &QPushButton::clicked, &dialog, &QDialog::accept);
    buttons->addWidget(copy);
    buttons->addStretch(1);
    buttons->addWidget(close);
    layout->addLayout(buttons);
    dialog.exec();
}

void MainWindow::addSearchPath()
{
    QString startDir;
    if (!m_config.search_paths.empty())
        startDir = qstr(m_config.search_paths.back());
    else if (!m_config.last_target.empty())
        startDir = QFileInfo(qstr(m_config.last_target)).absoluteDir().absolutePath();
    QString path = QFileDialog::getExistingDirectory(this, tr("Select search path"), startDir);
    if (path.isEmpty()) return;
    m_config.search_paths.push_back(str(path));
    refreshSearchPaths();
    saveConfig();
}

void MainWindow::removeSearchPath()
{
    int row = m_searchPaths->currentRow();
    if (row < 0 || row >= static_cast<int>(m_config.search_paths.size())) return;
    m_config.search_paths.erase(m_config.search_paths.begin() + row);
    refreshSearchPaths();
    saveConfig();
}

void MainWindow::moveSearchPath(int direction)
{
    int row = m_searchPaths->currentRow();
    int next = row + direction;
    if (row < 0 || next < 0 || next >= static_cast<int>(m_config.search_paths.size())) return;
    std::swap(m_config.search_paths[row], m_config.search_paths[next]);
    refreshSearchPaths();
    m_searchPaths->setCurrentRow(next);
    saveConfig();
}

void MainWindow::populateTree()
{
    m_tree->clear();
    if (!m_report.root) return;
    QTreeWidgetItem* root = new QTreeWidgetItem(m_tree);
    root->setText(0, qstr(m_report.root->name));
    root->setText(1, statusText(m_report.root->status));
    root->setText(2, categoryText(m_report.root->category));
    root->setIcon(0, iconForNode(*m_report.root));
    root->setData(0, NodePtrRole, QVariant::fromValue<quintptr>(reinterpret_cast<quintptr>(m_report.root.get())));
    std::unordered_set<std::string> expanded;
    addTreeNode(root, m_report.root, expanded);
    root->setExpanded(true);
}

void MainWindow::addTreeNode(QTreeWidgetItem* parent,
                             const std::shared_ptr<DepNode>& node,
                             std::unordered_set<std::string>& expanded)
{
    for (const auto& child : node->children) {
        bool seen = expanded.count(child->name) > 0;
        QTreeWidgetItem* item = new QTreeWidgetItem(parent);
        item->setText(0, qstr(child->name) + (seen ? " (...)" : ""));
        item->setText(1, statusText(child->status));
        item->setText(2, categoryText(child->category));
        item->setIcon(0, iconForNode(*child));
        item->setData(0, NodePtrRole, QVariant::fromValue<quintptr>(reinterpret_cast<quintptr>(child.get())));
        if (!seen) {
            expanded.insert(child->name);
            addTreeNode(item, child, expanded);
        }
    }
}

void MainWindow::updateDetails(QTreeWidgetItem* item)
{
    if (!item) return;
    auto* node = reinterpret_cast<const DepNode*>(item->data(0, NodePtrRole).value<quintptr>());
    if (!node) return;
    m_detailName->setText(tr("Name: ") + qstr(node->name));
    QString path = node->resolved_path.empty() ? tr("(not found)") : qstr(node->resolved_path);
    m_detailPath->setText(tr("Path: ") + path);
    m_detailCategory->setText(tr("Category: ") + categoryText(node->category) +
                              tr("  Status: ") + statusText(node->status));
}

void MainWindow::updateRedistPanel()
{
    if (m_report.redist_needed.empty()) {
        m_redistPanel->hide();
        return;
    }
    QString text;
    for (const auto& r : m_report.redist_needed) {
        text += "  [!] " + qstr(r.name);
        if (!r.url.empty()) text += "\n      " + qstr(r.url);
        text += "\n";
    }
    m_redistText->setPlainText(text);
    m_redistPanel->show();
}

void MainWindow::refreshSearchPaths()
{
    if (!m_searchPaths) return;
    m_searchPaths->clear();
    for (const auto& p : m_config.search_paths) m_searchPaths->addItem(qstr(p));
}

void MainWindow::setBusy(bool busy, const QString& title, const QString& text, int maxValue)
{
    const QList<QWidget*> controls = {
        m_analyzeButton, m_deployButton, m_reportButton, m_copyButton, m_zipButton, m_installerButton
    };
    for (QWidget* w : controls) if (w) w->setEnabled(!busy);

    if (busy) {
        ++m_progressCookie;
        if (!m_progress)
            m_progress = new QProgressDialog(text, QString(), 0, maxValue, this);
        m_progress->setLabelText(text);
        m_progress->setRange(0, maxValue);
        m_progress->setValue(0);
        m_progress->setWindowTitle(title);
        m_progress->setWindowModality(Qt::ApplicationModal);
        m_progress->setCancelButton(nullptr);
        m_progress->show();
    } else {
        ++m_progressCookie;
        if (m_progress) {
            m_progress->hide();
        }
    }
}

void MainWindow::appendLog(const QString& text)
{
    m_log->appendPlainText("[" + QTime::currentTime().toString("HH:mm:ss") + "] " + text);
}

void MainWindow::showNotice(const QString& text, bool isError, int timeoutMs)
{
    if (!m_noticeLabel) return;

    ++m_noticeCookie;
    const std::uint64_t cookie = m_noticeCookie;

    QString style;
    if (isError) {
        style = (m_currentTheme == "dark")
            ? "QLabel { background: #4b1f24; color: #ffd7dc; border: 1px solid #8f3a45; border-radius: 6px; padding: 8px 10px; }"
            : "QLabel { background: #fff1f2; color: #8a1c2a; border: 1px solid #e7a8b0; border-radius: 6px; padding: 8px 10px; }";
    } else {
        style = (m_currentTheme == "dark")
            ? "QLabel { background: #183247; color: #d7f0ff; border: 1px solid #3f6c8b; border-radius: 6px; padding: 8px 10px; }"
            : "QLabel { background: #eef7ff; color: #0f4c81; border: 1px solid #a9cbea; border-radius: 6px; padding: 8px 10px; }";
    }

    m_noticeLabel->setStyleSheet(style);
    m_noticeLabel->setText(text);
    m_noticeLabel->show();

    QTimer::singleShot(timeoutMs, this, [this, cookie]() {
        if (cookie != m_noticeCookie || !m_noticeLabel) return;
        m_noticeLabel->hide();
        m_noticeLabel->clear();
    });
}

TargetOs MainWindow::selectedTargetOs() const
{
    switch (m_targetOs->currentIndex()) {
        case 0: return TargetOs::Win7;
        case 1: return TargetOs::Win81;
        case 3: return TargetOs::Win11;
        default: return TargetOs::Win10;
    }
}

QString MainWindow::categoryText(DllCategory category) const
{
    switch (category) {
        case DllCategory::OsCore: return tr("OsCore (system)");
        case DllCategory::Redist: return tr("Redistributable");
        case DllCategory::ThirdParty: return tr("Third-party");
        case DllCategory::ApiSet: return tr("ApiSet (virtual)");
        default: return tr("Unknown");
    }
}

QString MainWindow::statusText(DepStatus status) const
{
    switch (status) {
        case DepStatus::Found: return tr("Found");
        case DepStatus::SystemPresent: return tr("System (always present)");
        case DepStatus::MaybeAbsent: return tr("Maybe absent on target");
        case DepStatus::Missing: return tr("MISSING");
        case DepStatus::UserExcluded: return tr("Excluded");
    }
    return {};
}

QIcon MainWindow::iconForNode(const DepNode& node) const
{
    if (node.status == DepStatus::Missing) return style()->standardIcon(QStyle::SP_MessageBoxCritical);
    if (node.status == DepStatus::MaybeAbsent) return style()->standardIcon(QStyle::SP_MessageBoxWarning);
    if (node.category == DllCategory::OsCore || node.category == DllCategory::ApiSet)
        return style()->standardIcon(QStyle::SP_MessageBoxInformation);
    return style()->standardIcon(QStyle::SP_DialogApplyButton);
}

// ── Recent targets ────────────────────────────────────────────────────────────

void MainWindow::pushRecentTarget(const QString& path)
{
    auto& v = m_config.recent_targets;
    const std::string s = str(path);
    v.erase(std::remove_if(v.begin(), v.end(),
        [&s](const AppConfig::RecentTarget& rt){ return rt.path == s; }), v.end());
    AppConfig::RecentTarget rt;
    rt.path = s;
    rt.search_paths = m_config.search_paths;
    v.insert(v.begin(), std::move(rt));
    if ((int)v.size() > AppConfig::kMaxRecentTargets)
        v.resize(AppConfig::kMaxRecentTargets);
    if (m_recentMenu) updateRecentMenu();
}

void MainWindow::updateRecentMenu()
{
    if (!m_recentMenu) return;
    m_recentMenu->clear();
    if (m_config.recent_targets.empty()) {
        m_recentMenu->addAction(tr("(empty)"))->setEnabled(false);
        return;
    }
    for (const auto& rt : m_config.recent_targets) {
        QString qp = qstr(rt.path);
        AppConfig::RecentTarget cap = rt;
        QAction* act = m_recentMenu->addAction(qp);
        connect(act, &QAction::triggered, this, [this, cap]() {
            m_exePath->setText(qstr(cap.path));
            m_config.last_target = cap.path;
            m_config.search_paths = cap.search_paths;
            refreshSearchPaths();
            saveConfig();
        });
    }
    m_recentMenu->addSeparator();
    m_recentMenu->addAction(tr("Clear Recent"), this, [this]() {
        m_config.recent_targets.clear();
        saveConfig();
        updateRecentMenu();
    });
}

// ── Log helpers ───────────────────────────────────────────────────────────────

QString MainWindow::logDir() const { return m_logDir; }

void MainWindow::saveSessionLog()
{
    if (m_currentLogFile.isEmpty()) return;
    QString text = m_log->toPlainText();
    if (text.isEmpty()) return;
    QFile f(m_currentLogFile);
    if (f.open(QIODevice::WriteOnly | QIODevice::Text))
        QTextStream(&f) << text;
}

void MainWindow::exportLog()
{
    QString defaultName = m_currentLogFile.isEmpty()
        ? QDir(m_logDir).absoluteFilePath(
              QDateTime::currentDateTime().toString("yyyy-MM-dd_HH-mm-ss") + "_export.log")
        : m_currentLogFile;
    QString path = QFileDialog::getSaveFileName(
        this, tr("Export Log"), defaultName, tr("Log files (*.log);;Text files (*.txt);;All files (*.*)"));
    if (path.isEmpty()) return;
    QFile f(path);
    if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QTextStream(&f) << m_log->toPlainText();
        QMessageBox::information(this, tr("Export Log"), tr("Log saved to:\n%1").arg(path));
    } else {
        QMessageBox::warning(this, tr("Export Log"), tr("Cannot write file:\n%1").arg(path));
    }
}

void MainWindow::showHistoryLogs()
{
    QDialog dlg(this);
    dlg.setWindowTitle(tr("History Logs"));
    dlg.resize(900, 600);
    QVBoxLayout* layout = new QVBoxLayout(&dlg);

    QSplitter* splitter = new QSplitter(Qt::Horizontal, &dlg);

    // Left: file list
    QListWidget* fileList = new QListWidget;
    QDir dir(m_logDir);
    QStringList filters; filters << "*.log";
    QStringList files = dir.entryList(filters, QDir::Files, QDir::Time);
    for (const QString& f : files)
        fileList->addItem(f);
    splitter->addWidget(fileList);

    // Right: log content viewer
    QPlainTextEdit* viewer = new QPlainTextEdit;
    viewer->setReadOnly(true);
    viewer->setLineWrapMode(QPlainTextEdit::NoWrap);
    QFont mono("Courier New", 9);
    viewer->setFont(mono);
    splitter->addWidget(viewer);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 3);
    layout->addWidget(splitter, 1);

    connect(fileList, &QListWidget::currentTextChanged, this,
            [this, viewer](const QString& name) {
        if (name.isEmpty()) return;
        QFile f(m_logDir + "/" + name);
        if (f.open(QIODevice::ReadOnly | QIODevice::Text))
            viewer->setPlainText(QTextStream(&f).readAll());
    });

    QHBoxLayout* buttons = new QHBoxLayout;
    QPushButton* btnOpen = new QPushButton(tr("Open in Explorer"));
    QPushButton* btnDel  = new QPushButton(tr("Delete"));
    QPushButton* btnClose = new QPushButton(tr("Close"));
    connect(btnOpen, &QPushButton::clicked, this, [this]() {
        QDesktopServices::openUrl(QUrl::fromLocalFile(m_logDir));
    });
    connect(btnDel, &QPushButton::clicked, this, [this, fileList, viewer]() {
        QListWidgetItem* item = fileList->currentItem();
        if (!item) return;
        if (QMessageBox::question(this, tr("Delete Log"),
                tr("Delete %1?").arg(item->text())) != QMessageBox::Yes) return;
        QFile::remove(m_logDir + "/" + item->text());
        delete item;
        viewer->clear();
    });
    connect(btnClose, &QPushButton::clicked, &dlg, &QDialog::accept);
    buttons->addWidget(btnOpen);
    buttons->addWidget(btnDel);
    buttons->addStretch(1);
    buttons->addWidget(btnClose);
    layout->addLayout(buttons);

    if (fileList->count() > 0) fileList->setCurrentRow(0);
    dlg.exec();
}

QString MainWindow::buildFullReport() const
{
    if (!m_report.root) return tr("No analysis result.");
    QString out;
    QTextStream s(&out);
    s << tr("Dependency Full Report\n");
    s << "================================================================\n";
    s << tr("Target: ") << qstr(m_report.target_exe) << "\n";
    s << tr("Deploy: %1  |  Maybe absent: %2  |  Missing: %3\n")
            .arg(m_report.to_copy.size())
            .arg(m_report.maybe_absent.size())
            .arg(m_report.missing.size());
    if (!m_report.warnings.empty()) {
        s << "\n[!] " << tr("Warnings") << " (" << m_report.warnings.size() << ")\n";
        for (const auto& w : m_report.warnings)
            s << "  [WARN] " << qstr(w) << "\n";
    }
    s << "\n[!] " << tr("Missing") << " (" << m_report.missing.size() << ")\n";
    for (auto* n : m_report.missing) s << "  [MISSING] " << qstr(n->name) << "\n";
    s << "\n[+] " << tr("To Copy") << " (" << m_report.to_copy.size() << ")\n";
    for (auto* n : m_report.to_copy) s << "  [COPY] " << qstr(n->name) << " <- " << qstr(n->resolved_path) << "\n";
    s << "\n[~] " << tr("Maybe Absent") << " (" << m_report.maybe_absent.size() << ")\n";
    for (auto* n : m_report.maybe_absent) s << "  [REDIST] " << qstr(n->name) << " (" << qstr(n->redist_package) << ")\n";
    if (!m_report.redist_needed.empty()) {
        s << "\n[*] " << tr("Redistributable packages required on target") << "\n";
        for (const auto& r : m_report.redist_needed) s << "  [PKG] " << qstr(r.name) << "\n";
    }
    if (!m_report.resource_dirs.empty()) {
        s << "\n[>] " << tr("Resource Dirs") << " (" << m_report.resource_dirs.size() << ")\n";
        for (const auto& rd : m_report.resource_dirs) s << "  [DIR] " << qstr(rd.dir_name) << " <- " << qstr(rd.src_path) << "\n";
    }
    return out;
}

void MainWindow::collectParentMap(const DepNode&,
                                  std::unordered_map<std::string, std::vector<std::string>>&,
                                  std::unordered_set<std::string>&) const
{
}

QString MainWindow::qstr(const std::string& value)
{
    return QString::fromUtf8(value.c_str());
}

std::string MainWindow::str(const QString& value)
{
    return value.toUtf8().constData();
}
