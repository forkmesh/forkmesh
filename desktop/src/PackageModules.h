#pragma once

#include <QHash>
#include <QJsonArray>
#include <QString>
#include <QStringList>

#include <optional>

namespace forkmesh::market::modules {

struct Module {
    QString id;
    QString path;
    QString abi;
    int api = 0;
    QStringList entrypoints;
    bool attempted = false;
    bool loaded = false;
    QString error;
    qint64 loadMs = 0;
};

QStringList searchDirectories();
void scan(const QStringList &directories);
void install();
bool loadForEntrypoint(const QString &entrypoint);
bool providesEntrypoint(const QString &entrypoint);
QList<Module> modules();
std::optional<Module> moduleForPackage(const QString &packageId);
QHash<QString, qint64> residentBytesByPackage();
QHash<QString, qint64> residentBytesIn(const QString &smaps, const QHash<QString, QString> &packageByFile);
QJsonArray status();
QString installedDirectoryFor(const QString &appPath);
int installBeside(const QString &builtBinary, const QString &appPath, QString *error = nullptr);

} // namespace forkmesh::market::modules
