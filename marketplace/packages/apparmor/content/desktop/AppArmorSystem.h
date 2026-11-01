#pragma once

#include "AppArmorModel.h"

#include <QDateTime>
#include <QList>
#include <QString>
#include <QStringList>

namespace forkmesh::apparmor {

struct ProfileFile {
    QString fileName;
    QString path;
    QString source;
    bool editable = false;
    bool disabled = false;
    bool forceComplain = false;
    QString text;
    QList<ProfileDeclaration> profiles;
};

struct ProcessInfo {
    qint64 pid = 0;
    QString name;
    QString command;
    QString executable;
    ProcessLabel label;
};

struct Snapshot {
    bool supported = false;
    bool moduleLoaded = false;
    bool enabled = false;
    bool inLsmList = false;
    QString lsm;
    QString parserPath;
    QString policyDir;
    QStringList abiVersions;
    QList<ProfileFile> files;
    QList<ProcessInfo> processes;
    QList<Event> events;
    QString eventSource;
    QDateTime readAt;
};

Snapshot readSnapshot(const QString &root, bool readUserLogs);
QList<ProfileFile> readPolicyFiles(const QString &dir, const QString &source, bool editable);
QList<ProcessInfo> readProcesses(const QString &procDir);
QString readUserEvents(QString *source);

QString backupDir();
QString adminLiveScript();
QString adminInstallScript();
QString adminDisableScript();
QString adminEnableScript();
QString adminReloadScript();
QString adminUnloadScript();
QString adminReloadAllScript();

}
