#pragma once

#include <QSet>
#include <QString>
#include <QStringList>

namespace forkmesh::gitrefusal {

bool overLocalChanges(const QString &gitOutput);

enum class MergeSourceKind { Local, RemoteTracking, Ambiguous, Missing };

struct MergeSource {
    MergeSourceKind kind = MergeSourceKind::Missing;
    QString ref;
    QString commit;
    QStringList candidates;
};

QStringList mergeSourceRefPatterns(const QString &branch);
MergeSource mergeSourceFromRefs(const QString &branch,
                                const QString &forEachRefOutput);

QString firstFailureLine(const QString &gitOutput);
bool remoteUnavailable(const QString &gitOutput);
QString forkmeshRemoteMessage(const QString &gitOutput);
QString connectionFailure(const QString &gitOutput);
QString explainFailure(const QString &gitOutput);
bool notFastForward(const QString &gitOutput);

QSet<QString> branchesCheckedOutInWorktrees(const QString &worktreeListPorcelain);

}
