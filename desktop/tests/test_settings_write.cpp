#include "SettingsWrite.h"

#include <QCoreApplication>
#include <QFile>
#include <QSettings>
#include <QStringList>
#include <QTemporaryDir>

#include <cstdio>

namespace {

int failures = 0;

void check(bool ok, const char *what)
{
    if (ok)
        return;
    std::fprintf(stderr, "FAIL %s\n", what);
    ++failures;
}

bool writeFile(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    return file.write(bytes) == bytes.size();
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.readAll();
}

}

int main(int argc, char **argv)
{
    QTemporaryDir dir;
    if (!dir.isValid()) {
        std::fprintf(stderr, "FAIL: no temporary dir\n");
        return 1;
    }
    qputenv("XDG_CONFIG_HOME", dir.path().toUtf8());
    qunsetenv("FORKMESH_HOME");
    qunsetenv("FORKMESH_PROJECTS_DIR");
    QCoreApplication app(argc, argv);

    using forkmesh::settings::drop;
    using forkmesh::settings::put;

    {
        const QString path = dir.filePath(QStringLiteral("fresh.ini"));
        QSettings settings(path, QSettings::IniFormat);
        check(put(settings, QStringLiteral("a/list"),
                  QStringList{QStringLiteral("#one"), QStringLiteral("#two")}),
              "first put of a list writes");
        check(!put(settings, QStringLiteral("a/list"),
                   QStringList{QStringLiteral("#one"), QStringLiteral("#two")}),
              "same list in the same process is skipped");
        check(put(settings, QStringLiteral("a/list"),
                  QStringList{QStringLiteral("#one")}),
              "shorter list writes");
        check(put(settings, QStringLiteral("a/count"), 3), "first int writes");
        check(!put(settings, QStringLiteral("a/count"), 3), "same int is skipped");
        check(drop(settings, QStringLiteral("a/count")), "drop of a stored key");
        check(!drop(settings, QStringLiteral("a/count")), "drop of a missing key");
    }

    {
        const QString path = dir.filePath(QStringLiteral("reloaded.ini"));
        {
            QSettings writer(path, QSettings::IniFormat);
            writer.setValue(QStringLiteral("chat/single"),
                            QStringList{QStringLiteral("#team")});
            writer.setValue(QStringLiteral("chat/multi"),
                            QStringList{QStringLiteral("#a"), QStringLiteral("#b, c"),
                                        QStringLiteral("#d")});
            writer.setValue(QStringLiteral("chat/empty"), QStringList{});
            writer.setValue(QStringLiteral("chat/count"), 7);
            writer.setValue(QStringLiteral("chat/on"), true);
            writer.sync();
        }
        const QByteArray written = readFile(path);
        check(!written.isEmpty(), "writer produced the ini file");
        const QString copy = dir.filePath(QStringLiteral("reloaded-copy.ini"));
        check(writeFile(copy, written), "copy the ini so it is parsed fresh");

        QSettings settings(copy, QSettings::IniFormat);
        check(!put(settings, QStringLiteral("chat/single"),
                   QStringList{QStringLiteral("#team")}),
              "one-entry list read back as a string is unchanged");
        check(!put(settings, QStringLiteral("chat/multi"),
                   QStringList{QStringLiteral("#a"), QStringLiteral("#b, c"),
                               QStringLiteral("#d")}),
              "list read back as a variant list is unchanged");
        check(!put(settings, QStringLiteral("chat/empty"), QStringList{}),
              "empty list read back as invalid is unchanged");
        check(!put(settings, QStringLiteral("chat/count"), 7),
              "int read back as a string is unchanged");
        check(!put(settings, QStringLiteral("chat/on"), true),
              "bool read back as a string is unchanged");
        settings.sync();
        check(readFile(copy) == written, "skipped puts leave the file untouched");

        check(put(settings, QStringLiteral("chat/single"),
                  QStringList{QStringLiteral("#team"), QStringLiteral("#ops")}),
              "grown list writes");
        check(put(settings, QStringLiteral("chat/multi"),
                  QStringList{QStringLiteral("#a"), QStringLiteral("#d")}),
              "shrunk list writes");
        check(put(settings, QStringLiteral("chat/empty"),
                  QStringList{QStringLiteral("#new")}),
              "list replacing an empty one writes");
        settings.sync();
        check(readFile(copy) != written, "real changes reach the file");
    }

    {
        const QString path = dir.filePath(QStringLiteral("strings.ini"));
        check(writeFile(path, QByteArrayLiteral("[k]\nname=\"#a, b\"\nblank=\n")),
              "write a hand-made ini");
        QSettings settings(path, QSettings::IniFormat);
        check(!put(settings, QStringLiteral("k/name"),
                   QStringList{QStringLiteral("#a, b")}),
              "quoted single entry with a comma is unchanged");
        check(put(settings, QStringLiteral("k/name"),
                  QStringList{QStringLiteral("#a"), QStringLiteral("b")}),
              "two entries differ from one quoted entry");
        check(put(settings, QStringLiteral("k/blank"), QStringList{}),
              "empty list differs from an empty string");
    }

    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("settings write tests passed\n");
    return 0;
}
