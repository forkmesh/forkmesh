#pragma once

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileDevice>
#include <QFileInfo>
#include <QSaveFile>
#include <QString>

namespace forkmesh::ownerfile {

inline constexpr QFileDevice::Permissions kOwnerOnly =
    QFileDevice::ReadOwner | QFileDevice::WriteOwner;

inline bool write(const QString &path, const QByteArray &data)
{
    if (path.isEmpty() || !QDir().mkpath(QFileInfo(path).absolutePath()))
        return false;
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.setPermissions(kOwnerOnly);
    if (file.write(data) != data.size()) {
        file.cancelWriting();
        return false;
    }
    return file.commit();
}

inline bool restrictToOwner(const QString &path)
{
    if (path.isEmpty() || !QFileInfo::exists(path))
        return true;
    return QFile::setPermissions(path, kOwnerOnly);
}

} // namespace forkmesh::ownerfile
