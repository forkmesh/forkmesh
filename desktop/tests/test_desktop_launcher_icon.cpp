#include "DesktopLauncherIcon.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QTemporaryDir>

#include <cstdio>

using namespace forkmesh::launchericon;

namespace {

int failures = 0;

void check(bool condition, const char *message)
{
    if (condition)
        return;
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
}

bool writeFile(const QString &path, const QByteArray &bytes)
{
    QDir().mkpath(QFileInfo(path).path());
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
           file.write(bytes) == bytes.size();
}

QImage transparentIcon()
{
    QImage image(1024, 1024, QImage::Format_ARGB32);
    image.fill(Qt::transparent);
    for (int y = 256; y < 768; ++y)
        for (int x = 256; x < 768; ++x)
            image.setPixel(x, y, qRgba(19, 19, 22, 255));
    return image;
}

QImage opaqueIcon()
{
    QImage image(256, 256, QImage::Format_ARGB32);
    image.fill(QColor(0, 0, 0));
    return image;
}

void testNothingWithoutLauncher()
{
    QTemporaryDir dir;
    const QStringList changed = refresh(dir.path(), transparentIcon(), "<svg/>");
    check(changed.isEmpty(), "no launcher means no icons are written");
    check(!QFileInfo::exists(dir.path() + "/icons"), "the icons folder is left alone");
}

void testReplacesOpaqueIcon()
{
    QTemporaryDir dir;
    const QString home = dir.path();
    writeFile(home + "/applications/forkmesh.desktop", "[Desktop Entry]\nIcon=forkmesh\n");
    const QString old256 = home + "/icons/hicolor/256x256/apps/forkmesh.png";
    QDir().mkpath(QFileInfo(old256).path());
    check(opaqueIcon().save(old256, "PNG"), "the stale icon is set up");
    check(opaqueIcon().save(home + "/icons/forkmesh.png", "PNG"), "the stale fallback is set up");

    const QStringList changed = refresh(home, transparentIcon(), "<svg/>");
    check(changed.contains(old256), "the stale 256px icon is rewritten");
    check(changed.size() == 9, "every size, the svg and the fallback are written");

    const QImage icon256(old256);
    check(icon256.size() == QSize(256, 256), "the 256px bucket holds a 256px icon");
    check(icon256.hasAlphaChannel(), "the refreshed icon keeps its alpha channel");
    check(qAlpha(icon256.pixel(0, 0)) == 0, "the refreshed icon's corner is transparent");
    check(qAlpha(icon256.pixel(128, 128)) == 255, "the refreshed icon's middle is solid");

    const QImage icon16(home + "/icons/hicolor/16x16/apps/forkmesh.png");
    check(icon16.size() == QSize(16, 16), "the 16px bucket is written");
    check(qAlpha(QImage(home + "/icons/forkmesh.png").pixel(0, 0)) == 0,
          "the unthemed fallback is refreshed too");

    QFile svg(home + "/icons/hicolor/scalable/apps/forkmesh.svg");
    check(svg.open(QIODevice::ReadOnly) && svg.readAll() == "<svg/>",
          "the scalable icon is the bundled svg");
}

void testUnchangedIconsAreNotRewritten()
{
    QTemporaryDir dir;
    const QString home = dir.path();
    writeFile(home + "/applications/forkmesh.desktop", "[Desktop Entry]\n");
    refresh(home, transparentIcon(), "<svg/>");
    check(refresh(home, transparentIcon(), "<svg/>").isEmpty(),
          "a second refresh with the same icon writes nothing");
    check(!QFileInfo::exists(home + "/icons/forkmesh.png"),
          "no unthemed fallback is created when there was none");
}

void testInstallsLauncherForInstalledCopy()
{
    QTemporaryDir dir;
    const QString home = dir.path() + "/share";
    const QString client = dir.path() + "/bin/forkmesh";
    const QString other = dir.path() + "/build/forkmesh";
    writeFile(client, "binary");
    writeFile(other, "binary");

    check(!installLauncher(home, client, other),
          "a build that is not the installed copy writes no launcher");
    check(!launcherInstalled(home), "no launcher after a build-tree run");
    check(!installLauncher(home, dir.path() + "/bin/missing", dir.path() + "/bin/missing"),
          "a missing installed copy writes no launcher");

    check(installLauncher(home, client, client),
          "the installed copy adds itself to the applications menu");
    check(launcherInstalled(home), "the launcher file exists");
    QFile entry(home + "/applications/forkmesh.desktop");
    check(entry.open(QIODevice::ReadOnly) &&
              entry.readAll() == launcherEntry(client),
          "the launcher runs the installed copy");
    check(launcherEntry(client).contains("Exec=\"" + client.toUtf8() + "\" %u\n"),
          "the launcher quotes the installed path");

    writeFile(home + "/applications/forkmesh.desktop", "[Desktop Entry]\nName=Custom\n");
    check(!installLauncher(home, client, client),
          "an existing launcher is never overwritten");
}

}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    testNothingWithoutLauncher();
    testReplacesOpaqueIcon();
    testUnchangedIconsAreNotRewritten();
    testInstallsLauncherForInstalledCopy();
    if (failures == 0)
        std::printf("desktop launcher icon tests passed\n");
    return failures == 0 ? 0 : 1;
}
