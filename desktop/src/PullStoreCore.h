#pragma once

#include "PullStore.h"

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

class ForkMeshIdentity;

namespace forkmesh::pullstorecore {

struct Store {
    QString workTree;
    QString mirror;
    QString authorName;
    QString repoKey;
    const ForkMeshIdentity *identity = nullptr;
};

struct Reply {
    bool answered = false;
    bool failed = false;
    QJsonObject result;
    QString error;
};

class Backend
{
public:
    virtual ~Backend() = default;
    virtual bool available() const = 0;
    virtual Reply call(const QString &method, const QJsonObject &params, bool write) = 0;
};

void setBackend(Backend *backend);
bool routed();

QJsonObject pullToJson(const PullRequest &pr);
PullRequest pullFromJson(const QJsonObject &obj);

std::optional<bool> call(const Store &store, const QString &method, const QJsonObject &params,
                         QString *error, QJsonObject *result = nullptr, bool write = true);
std::optional<QList<PullRequest>> loadAll(const Store &store);
std::optional<QList<PullRequest>> loadAllIncludingArchived(const Store &store);
std::optional<int> createPull(const Store &store, const QString &title, const QString &description,
                              const QString &base, const QString &head, const QString &patch,
                              const QString &commits, bool branchBacked, QString *error);
std::optional<bool> isBranchBehindBase(const Store &store, int number, bool *behind, QString *error,
                                       int *behindCount);
std::optional<bool> checkMergeable(const Store &store, int number, bool *clean,
                                   QStringList *conflictFiles, QString *error);
std::optional<QList<int>> adoptForeignPulls(const Store &store, const QString &remoteUrl, QString *error,
                                            bool fetchFirst);
std::optional<QList<int>> reconcileMergedPulls(const Store &store, QString *error,
                                               QList<int> *rewardsAdvanced);
std::optional<bool> deletePulls(const Store &store, const QList<int> &numbers, bool rewriteHistory,
                                QList<int> *deleted, QList<int> *missing, QString *error);
std::optional<bool> addEvent(const Store &store, int number, const PullEvent &input, QString *error);
std::optional<bool> addBranchEvent(const Store &store, const QString &branch, const PullEvent &input,
                                   QString *error);
std::optional<bool> applyEvent(const Store &store, int number, const PullEvent &ev, QString *error);
std::optional<bool> applyBranchEvent(const Store &store, const QString &branch, const PullEvent &ev,
                                     QString *error);
std::optional<QList<PullEvent>> readBranchReviewEvents(const Store &store, const QString &branch);
std::optional<PullStore::BranchSource> ensureLocalBranch(const Store &store, const QString &branch,
                                                         int *pullNumber, QString *error);
std::optional<bool> materializePullRef(const Store &store, const PullRequest &pr, QString *error);
std::optional<bool> checkoutForReview(const Store &store, const PullRequest &pr, const QString &dir,
                                      PullReviewCheckout *out, QString *error);
bool discardReviewCheckout(const Store &store, const QString &dir);
std::optional<bool> sessionStart(const Store &store, int number, const QString &mode,
                                 const QString &path, QStringList *conflicted, bool *resolvedClean,
                                 QString *content, QString *error);
std::optional<bool> sessionFinish(const Store &store, int number, const QString &mode,
                                  const QString &path, const QString &content,
                                  const QString &message, QString *error);
bool sessionAbort(const Store &store);
std::optional<bool> sessionInProgress(const Store &store);
std::optional<QString> sessionEditWorkTree(const Store &store);

bool validRewritePaths(int number, const QStringList &paths);
bool rewriteLedgerHistory(const QString &workTree, const QString &mirror, int number,
                          const QStringList &paths, QString *error);

}
