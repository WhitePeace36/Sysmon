#include "mainwindow.h"
#include "samplerworker.h"

#include <QApplication>
#include <QIcon>
#include <QSettings>
#include <QStringList>

// The application used to be called sysmon-qt. Settings saved under that name
// (window size, column layout, options) are taken over on the first start.
static void adoptFormerSettings()
{
    QSettings current;
    if (!current.allKeys().isEmpty())
        return;
    QSettings former(QStringLiteral("sysmon-qt"), QStringLiteral("sysmon-qt"));
    const QStringList keys = former.allKeys();
    for (const QString& key : keys)
        current.setValue(key, former.value(key));
}

int main(int argc, char** argv)
{
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
#endif
    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("sysmon"));
    QApplication::setApplicationName(QStringLiteral("sysmon"));
    QApplication::setDesktopFileName(QStringLiteral("sysmon"));  // ties the window to sysmon.desktop
    QApplication::setApplicationDisplayName(QObject::tr("System Monitor"));
    // The logo is compiled in (data/sysmon.qrc), so the window shows it even
    // when the program runs straight from the build directory.
    QIcon logo;
    for (int size : {16, 22, 24, 32, 48, 64, 128, 256})
        logo.addFile(QStringLiteral(":/icons/sysmon-%1.png").arg(size), QSize(size, size));
    QApplication::setWindowIcon(logo);

    // Snapshots cross from the sampling thread to the GUI thread.
    qRegisterMetaType<SnapshotPtr>("SnapshotPtr");

    adoptFormerSettings();

    MainWindow window;
    window.show();
    return QApplication::exec();
}
