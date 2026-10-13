#pragma once

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QString>

namespace forkmesh::ui {

// The first file or directory under the desktop client's source tree that
// changed after `since`, as a path, or an empty string when nothing did. The
// rebuild reads the checkout the app itself keeps merging into: an agent
// session that finishes mid-build lands its branch here, so a header the
// precompiled header was built from can change under the compiler halfway
// through, which GCC then reports as every symbol defined twice. Checking for
// this after the build step is what lets the rebuild run the step again on one
// consistent tree instead of failing.
//
// Directories count too: deleting or adding a file touches its parent, which
// is the only trace a removed file leaves. Build trees (`build`, `build-*`),
// hidden entries and the tests tree are skipped — the first two are outputs
// the build itself writes, the last is not an input to the app binary. The
// marketplace beside the client is scanned as well: its desktop packages
// compile into the binary.
inline QString sourceChangedSince(const QString &clientDir, const QDateTime &since)
{
    const QString root = QDir(clientDir).absolutePath();
    auto skipped = [](const QString &name) {
        return name.startsWith(QLatin1Char('.')) ||
               name == QLatin1String("build") ||
               name.startsWith(QLatin1String("build-")) ||
               name == QLatin1String("tests");
    };
    auto scan = [&](const QString &dir) -> QString {
        if (!QFileInfo(dir).isDir())
            return QString();
        QDirIterator it(dir,
                        QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot |
                            QDir::NoSymLinks,
                        QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString path = it.next();
            const QFileInfo info = it.fileInfo();
            // A skipped directory is pruned by name at every depth; QDirIterator
            // has no prune hook, so entries below one are recognised by prefix.
            const QString relative = QDir(dir).relativeFilePath(path);
            bool pruned = false;
            for (const QString &segment : relative.split(QLatin1Char('/'))) {
                if (skipped(segment)) {
                    pruned = true;
                    break;
                }
            }
            if (pruned)
                continue;
            if (info.lastModified() > since)
                return path;
        }
        return QString();
    };
    if (const QString changed = scan(root); !changed.isEmpty())
        return changed;
    return scan(QDir(root).absoluteFilePath(QStringLiteral("../marketplace")));
}

} // namespace forkmesh::ui
