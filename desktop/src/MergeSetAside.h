#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QString>
#include <QStringList>

#include <functional>

namespace forkmesh::mergesetaside {

struct GitResult {
    bool ok = false;
    QByteArray out;
    QString error;
};

using Git = std::function<GitResult(const QStringList &args, const QByteArray &input)>;

Git processGit(const QString &dir);

bool canSetAside(const QByteArray &statusPorcelainZ, const QStringList &paths);
bool canSetAside(const Git &git, const QStringList &paths);

QString backupBranchName(const QString &base, const QString &branch,
                         const QDateTime &when);

struct Aside {
    bool ok = false;
    QString error;
    QString backupBranch;
    QStringList paths;
    QByteArray patch;
};

Aside setAside(const Git &git, const QString &base, const QString &branch,
               const QStringList &paths, const QDateTime &when);

struct PutBack {
    bool ok = false;
    QStringList clashed;
    QString error;
};

PutBack putBack(const Git &git, const Aside &aside);

QString putBackNote(const Aside &aside, const PutBack &back);

}
