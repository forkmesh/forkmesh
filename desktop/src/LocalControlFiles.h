#pragma once

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

// The "files.browse." local control family (WS16; docs/native/DESKTOP_PARITY.md
// section 19): read-only browsing of the desktop's repositories and worktrees. This is the policy half, kept free of widgets so the
// tests can drive it; the requests are answered in
// MainWindowLocalControlFiles.cpp. Gate 2 creates it empty; WS16 owns it.
namespace forkmesh::localcontrol::filesbrowse {

// The method prefix this family answers.
QString familyPrefix();

QString normalizeRelativePath(const QString &path, bool *ok);

bool isSensitiveName(const QString &name);

QString resolveInside(const QString &rootPath, const QString &relative,
                      QString *error);

struct Listing {
    QJsonArray entries;
    bool truncated = false;
    int total = 0;
};
Listing listDirectory(const QString &rootPath, const QString &dirPath,
                      int maxEntries);

inline constexpr qint64 kMaxReadBytes = 256 * 1024;
bool looksBinary(const QByteArray &bytes);
QJsonObject readFileSlice(const QString &path, qint64 offset, qint64 maxBytes);

struct Root {
    QString id;
    QString label;
    QString kind;
    QString path;
    QString branch;
    QString repo;
};
bool validRootId(const QString &id);
QString worktreeRootId(const QString &repoKey, const QString &folder);
QList<Root> discoverWorktrees(const QString &repoKey, const QString &repoPath,
                              int maxWorktrees);
QJsonObject rootRow(const Root &root);

QJsonObject parsePorcelainStatus(const QByteArray &output, int maxFiles);

QStringList gitActions();
QStringList gitArguments(const QString &action);
QString gitActionSummary(const QString &action, const QString &label);

} // namespace forkmesh::localcontrol::filesbrowse
