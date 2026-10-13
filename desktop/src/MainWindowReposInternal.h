#pragma once

// Helpers shared by the MainWindow repository files split out of
// MainWindowRepos.cpp (MainWindowRepos, MainWindowRepoSettings,
// MainWindowRepoSync, MainWindowRepoFetch). They were file-local
// helpers of that one translation unit; each is still defined once, in
// the file noted below. Include after MainWindow.h and MainWindowInternal.h,
// then `using namespace mainwindowrepos;`.

#include "MainWindow.h"

#include <QByteArray>
#include <QList>
#include <QString>

class QWidget;

namespace mainwindowrepos {

struct RepoRemoteRow {
    QString name;
    QString fetchUrl;
    QString pushUrl;
};

// Defined in MainWindowRepos.cpp.
bool promptAndSetGitIdentity(QWidget *parent, const QString &dir,
                             const QString &suggestedName = QString());
QString privateReplicaRoot();
QString privateIdentityVaultPath();
QString publicArchiveRoot();
QByteArray privateIdentityVaultSecret(const ForkMeshIdentity &identity);
QString repositoryIdentityKey(const RepositoryRecord &repo);
void stampGitRepositoryIdentity(const QString &gitDir, const QString &identity);
QList<RepoRemoteRow> readRepoRemotes(const QString &gitDir);

// Defined in MainWindowRepoSync.cpp.
QString mirrorRefsDigest(const QString &mirrorPath);

} // namespace mainwindowrepos
