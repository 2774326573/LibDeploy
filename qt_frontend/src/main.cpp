#include "main_window.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QIcon>

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName("LibDeployQt");
    QApplication::setOrganizationName("LibDeploy");
    app.setWindowIcon(QIcon(QFileInfo(QCoreApplication::applicationFilePath())
                                .absoluteDir()
                                .absoluteFilePath("libdeploy.ico")));

    MainWindow window;
    window.show();
    return app.exec();
}
