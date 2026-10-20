#pragma once

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>

#include <functional>

namespace forkmesh::upstream {

bool configureForkCheckoutRemote(const QString &checkoutPath,
                                 const QString &sourceUrl,
                                 const QString &forkMirrorPath,
                                 QString *error = nullptr);

struct RefreshOutcome {
    bool fetchOk = false;
    int branchesUpdated = 0; // fast-forwarded, created, or force-tracked
    int branchesPruned = 0;  // deleted: gone upstream and fully contained
    int branchesKept = 0;    // diverged local heads deliberately untouched
    bool headFastForwarded = false; // the primary checkout's branch moved
    QString error;                  // first fatal failure; empty on success

    QString summary() const;
};

struct MirrorRoute {
    QString url;
    // Empty uses the hardened non-interactive process environment. Managed
    // hosts carry their saved SSH transport here so every exact node can be
    // read before the source prunes or republishes refs.
    QProcessEnvironment environment;
    // Non-empty values identify alternative transports for the same mirror.
    // Alternatives are tried in order and the mirror is considered readable
    // as soon as one succeeds; an empty value keeps this route independent.
    QString fallbackGroup;
    QHash<QString, QStringList> deletedBranches;
};

RefreshOutcome refreshManagedCheckoutFromUpstream(
    const QString &checkoutPath, const QString &upstreamUrl,
    const QStringList &forcedBranches = {QStringLiteral("forkmesh/pulls")});

RefreshOutcome convergeSourceCheckoutFromMesh(const QString &checkoutPath,
                                              const QString &meshUrl);

// Converge an owner checkout from every exact managed-mirror route, not just
// one round-robin public clone. Clean repository-native issue, discussion, and
// pull-ledger changes are retained even when they sit after unrelated stale
// mirror commits; ordinary source-code content remains source-owned.
RefreshOutcome convergeSourceCheckoutFromMirrors(
    const QString &checkoutPath, const QStringList &mirrorUrls);
RefreshOutcome convergeSourceCheckoutFromRoutes(
    const QString &checkoutPath, const QList<MirrorRoute> &mirrorRoutes,
    const std::function<void()> &afterEachReachableRoute = {});

struct ServedBranchSync {
    QStringList updated;
    QStringList current;
    QStringList kept;
    QStringList missing;
    QStringList present;
    bool reachedSource = false;
};

ServedBranchSync syncServedAgentBranches(const QString &checkoutPath,
                                         const QStringList &sources,
                                         const QStringList &branches);

QString deletedBranchTombstoneRef(const QString &branch);
QString restoreDeletedBranch(const QString &checkoutPath,
                             const QString &branch);
QByteArray branchTrashRequestCanonical(const QString &owner,
                                       const QString &repo,
                                       const QString &timestampMs);
bool parseServedBranchTrash(const QByteArray &body, const QString &owner,
                            const QString &repo,
                            QHash<QString, QStringList> *deleted);
QStringList refsNamedAsBadObjects(const QString &gitErrors);
QStringList dropUnreadableRefs(const QString &gitDir, const QString &gitErrors);

} // namespace forkmesh::upstream
