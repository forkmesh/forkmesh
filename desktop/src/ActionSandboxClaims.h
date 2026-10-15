#pragma once

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QSet>
#include <QString>

#ifndef Q_OS_WIN
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace forkmesh::actions {

inline QString sandboxClaimFileName()
{
    return QStringLiteral(".forkmesh-claim");
}

inline QMutex &sandboxClaimMutex()
{
    static QMutex mutex;
    return mutex;
}

inline QHash<QString, int> &sandboxClaimDescriptors()
{
    static QHash<QString, int> descriptors;
    return descriptors;
}

inline QSet<QString> &coreClaimedSandboxStorage()
{
    static QSet<QString> names;
    return names;
}

inline void lockSandboxClaim(const QString &root)
{
#ifndef Q_OS_WIN
    if (root.isEmpty())
        return;
    const QByteArray path = QFile::encodeName(QDir(root).filePath(sandboxClaimFileName()));
    const int fd = ::open(path.constData(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0)
        return;
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        ::close(fd);
        return;
    }
    QMutexLocker lock(&sandboxClaimMutex());
    const QString name = QFileInfo(root).fileName();
    const int previous = sandboxClaimDescriptors().value(name, -1);
    if (previous >= 0)
        ::close(previous);
    sandboxClaimDescriptors().insert(name, fd);
#else
    Q_UNUSED(root)
#endif
}

inline void unlockSandboxClaim(const QString &root)
{
#ifndef Q_OS_WIN
    if (root.isEmpty())
        return;
    QMutexLocker lock(&sandboxClaimMutex());
    const auto it = sandboxClaimDescriptors().find(QFileInfo(root).fileName());
    if (it == sandboxClaimDescriptors().end())
        return;
    ::close(it.value());
    sandboxClaimDescriptors().erase(it);
#else
    Q_UNUSED(root)
#endif
}

inline bool sandboxClaimedElsewhere(const QString &tree)
{
#ifndef Q_OS_WIN
    const QByteArray path = QFile::encodeName(QDir(tree).filePath(sandboxClaimFileName()));
    const int fd = ::open(path.constData(), O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return false;
    const bool held = ::flock(fd, LOCK_EX | LOCK_NB) != 0 && errno == EWOULDBLOCK;
    ::close(fd);
    return held;
#else
    Q_UNUSED(tree)
    return false;
#endif
}

inline void setCoreClaimedSandbox(const QString &name, bool claimed)
{
    if (name.isEmpty())
        return;
    QMutexLocker lock(&sandboxClaimMutex());
    if (claimed)
        coreClaimedSandboxStorage().insert(name);
    else
        coreClaimedSandboxStorage().remove(name);
}

inline void replaceCoreClaimedSandboxes(const QSet<QString> &names)
{
    QMutexLocker lock(&sandboxClaimMutex());
    coreClaimedSandboxStorage() = names;
}

inline QSet<QString> coreClaimedSandboxes()
{
    QMutexLocker lock(&sandboxClaimMutex());
    return coreClaimedSandboxStorage();
}

}
