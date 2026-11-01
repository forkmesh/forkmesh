#pragma once

#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>

namespace forkmesh::claudedesktop {

struct DesktopRecord {
    QString key;
    QString sessionId;
    QString cwd;
    QString title;
    QString model;
    QString transcript;
    qint64 createdMs = 0;
    qint64 activityMs = 0;
    qint64 writtenMs = 0;
    bool waiting = false;
};

struct DesktopSection {
    QString id;
    QString kind;
    QString name;
    QString groupBy;
    bool ascending = false;
    QStringList members;
};

struct DesktopIndex {
    QList<DesktopRecord> records;
    QList<DesktopSection> sections;
    QStringList pinned;
};

DesktopIndex readDesktopIndex(const QString &desktopRoot, const QString &claudeHome,
                              const QHash<QString, QString> &knownTranscripts = {});

QString folderLabel(const QString &cwd);

bool naturalLess(const QString &a, const QString &b);

struct SidebarItem {
    QString recordKey;
    QString folder;
    qint64 at = 0;
};

struct SidebarGroup {
    QString key;
    QString label;
    QList<int> items;
};

QList<SidebarGroup> arrangeSidebar(const DesktopIndex &index, const QList<SidebarItem> &items);

} // namespace forkmesh::claudedesktop
