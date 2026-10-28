#include "DesktopLauncherIcon.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QSaveFile>
#include <QStandardPaths>

namespace forkmesh::launchericon {

namespace {

constexpr int kSizes[] = {16, 32, 48, 64, 128, 256, 512};

QByteArray pngBytes(const QImage &image)
{
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return bytes;
}

QByteArray fileBytes(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.readAll();
}

bool writeIfChanged(const QString &path, const QByteArray &bytes)
{
    if (bytes.isEmpty() || fileBytes(path) == bytes)
        return false;
    QDir().mkpath(QFileInfo(path).path());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.write(bytes);
    return file.commit();
}

}

QString dataHome()
{
    const QString xdg = qEnvironmentVariable("XDG_DATA_HOME");
    if (!xdg.isEmpty() && QDir::isAbsolutePath(xdg))
        return QDir::cleanPath(xdg);
    return QDir::homePath() + QStringLiteral("/.local/share");
}

bool launcherInstalled(const QString &dataHome)
{
    return QFileInfo::exists(dataHome + QStringLiteral("/applications/forkmesh.desktop"));
}

QByteArray launcherEntry(const QString &clientBin)
{
    return QStringLiteral("[Desktop Entry]\n"
                          "Type=Application\n"
                          "Version=1.0\n"
                          "Name=ForkMesh\n"
                          "GenericName=Decentralized Git\n"
                          "Comment=Peer-to-peer Git hosting, chat, issues, and CI\n"
                          "Exec=\"%1\" %u\n"
                          "Icon=forkmesh\n"
                          "Terminal=false\n"
                          "Categories=Development;RevisionControl;\n"
                          "MimeType=x-scheme-handler/forkmesh;\n"
                          "Keywords=git;fork;mesh;p2p;repository;\n"
                          "StartupNotify=true\n"
                          "StartupWMClass=forkmesh\n")
        .arg(clientBin)
        .toUtf8();
}

bool installLauncher(const QString &dataHome, const QString &clientBin,
                     const QString &runningBin)
{
    if (dataHome.isEmpty() || launcherInstalled(dataHome))
        return false;
    const QString installed = QFileInfo(clientBin).canonicalFilePath();
    if (installed.isEmpty() ||
        installed != QFileInfo(runningBin).canonicalFilePath())
        return false;
    const QString path = dataHome + QStringLiteral("/applications/forkmesh.desktop");
    if (!writeIfChanged(path, launcherEntry(clientBin)))
        return false;
    QFile::setPermissions(path, QFile::permissions(path) | QFileDevice::ExeOwner);
    return true;
}

QStringList refresh(const QString &dataHome, const QImage &icon, const QByteArray &svg)
{
    QStringList changed;
    if (icon.isNull() || !launcherInstalled(dataHome))
        return changed;
    const QImage source = icon.convertToFormat(QImage::Format_ARGB32);
    const QString hicolor = dataHome + QStringLiteral("/icons/hicolor");
    for (const int size : kSizes) {
        const QString path =
            QStringLiteral("%1/%2x%2/apps/forkmesh.png").arg(hicolor).arg(size);
        const QImage scaled =
            source.scaled(size, size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        if (writeIfChanged(path, pngBytes(scaled)))
            changed << path;
    }
    const QString scalable = hicolor + QStringLiteral("/scalable/apps/forkmesh.svg");
    if (writeIfChanged(scalable, svg))
        changed << scalable;
    const QString fallback = dataHome + QStringLiteral("/icons/forkmesh.png");
    if (QFileInfo::exists(fallback) && writeIfChanged(fallback, pngBytes(source)))
        changed << fallback;
    return changed;
}

void refreshInstalled()
{
#if defined(Q_OS_LINUX)
    const QString home = dataHome();
    if (installLauncher(home, QDir::homePath() + QStringLiteral("/.local/bin/forkmesh"),
                        QCoreApplication::applicationFilePath())) {
        const QString databaseTool =
            QStandardPaths::findExecutable(QStringLiteral("update-desktop-database"));
        if (!databaseTool.isEmpty())
            QProcess::startDetached(databaseTool, {home + QStringLiteral("/applications")});
    }
    QFile svg(QStringLiteral(":/app/forkmesh.svg"));
    const QByteArray svgBytes = svg.open(QIODevice::ReadOnly) ? svg.readAll() : QByteArray();
    const QStringList changed =
        refresh(home, QImage(QStringLiteral(":/app/forkmesh.png")), svgBytes);
    if (changed.isEmpty())
        return;
    const QString hicolor = home + QStringLiteral("/icons/hicolor");
    const QString cacheTool =
        QStandardPaths::findExecutable(QStringLiteral("gtk-update-icon-cache"));
    if (!cacheTool.isEmpty() &&
        QFileInfo::exists(hicolor + QStringLiteral("/icon-theme.cache")))
        QProcess::startDetached(cacheTool, {QStringLiteral("-f"), QStringLiteral("-t"), hicolor});
#endif
}

}
