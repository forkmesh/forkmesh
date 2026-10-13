#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>

// Branch management requested on the website (adhoc #2098).
//
// The website has no Git and the relay must never grow one, so a branch
// delete / update-from-default / create typed on the Branches page is queued
// for the node that owns the repository's checkout. That queue arrives in the
// node's /api/sync response, and this component applies it to the real
// checkout — the same operations the desktop app's own branch table performs,
// with the same refusals, so a web request can never do something the local
// UI would not.
//
// Every request carries the branch tip the requester saw. A branch that moved
// since is refused rather than applied: nobody deletes or rewrites commits
// they never looked at.
namespace forkmesh::branchops {

struct Outcome {
    QString op;      // "delete", "update", "create"
    QString branch;
    bool applied = false;
    // True when the request was already satisfied (a branch that is gone, a
    // branch that already exists at the requested commit, a branch already
    // containing the base). Nothing changed, and nothing is wrong.
    bool alreadyDone = false;
    QString message; // Log line: what happened, or why it was refused.
    qint64 requestId = 0;
    QString base;
    QString branchOid;
    QString mergeOid;
    QString worktreePath;
    bool cleanupRequested = false;
    bool branchDeleted = false;
};

// Apply one queued operation to ``checkoutPath`` (a normal checkout with a
// working tree, not a bare mirror). ``defaultBranch`` is the repository's
// default branch: it can never be deleted, and it is the base an update
// merges in when the request names none.
Outcome applyBranchOp(const QString &checkoutPath,
                      const QString &defaultBranch,
                      const QJsonObject &op);

QList<Outcome> applyBranchOps(const QString &checkoutPath,
                              const QString &defaultBranch,
                              const QJsonArray &ops);

// The same name validation the relay applies before queueing, repeated here
// because a queued request is data from the network either way.
bool usableBranchName(const QString &branch);

Outcome cleanUpMergedBranch(const QString &checkoutPath,
                            const QString &defaultBranch, Outcome outcome);

QJsonObject resultJson(const Outcome &outcome);

} // namespace forkmesh::branchops
