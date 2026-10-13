#pragma once

#include <QMap>
#include <QString>

namespace forkmesh::vaultfile {

struct Entry {
    QString value;
    qint64 at = 0;
};

QString defaultPath();
QString identityPath();
bool exists(const QString &path);
QString entryName(const QString &ns, const QString &name);
bool readNamespace(const QString &vaultPath, const QString &identityPemPath, const QString &ns,
                   QMap<QString, Entry> *out, QString *error);

}
