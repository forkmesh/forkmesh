#include "PullStoreCore.h"

#include "ForkMeshIdentity.h"

#include <QHash>
#include <QJsonArray>
#include <QMutex>
#include <QMutexLocker>
#include <QRegularExpression>

#include <atomic>

namespace forkmesh::pullstorecore {

namespace {

std::atomic<Backend *> &backendSlot()
{
    static std::atomic<Backend *> slot{nullptr};
    return slot;
}

struct LoadCacheEntry {
    QString stamp;
    QList<PullRequest> pulls;
};

QMutex &loadCacheMutex()
{
    static QMutex mutex;
    return mutex;
}

QHash<QString, LoadCacheEntry> &loadCache()
{
    static QHash<QString, LoadCacheEntry> cache;
    return cache;
}

constexpr int kLoadCacheEntries = 8;

bool identityValid(const Store &store)
{
    return store.identity && store.identity->isValid();
}

QJsonObject storeJson(const Store &store)
{
    const bool valid = identityValid(store);
    return {{QStringLiteral("workTree"), store.workTree},
            {QStringLiteral("mirror"), store.mirror},
            {QStringLiteral("authorName"), store.authorName},
            {QStringLiteral("repoKey"), store.repoKey},
            {QStringLiteral("identityKey"), valid ? store.identity->publicKey() : QString()},
            {QStringLiteral("identityValid"), valid},
            {QStringLiteral("nativeAmLive"), PullStore::conflictSessionLive(store.workTree)}};
}

QList<PullRequest> pullsFrom(const QJsonValue &value)
{
    QList<PullRequest> pulls;
    for (const QJsonValue &item : value.toArray())
        pulls.append(pullFromJson(item.toObject()));
    return pulls;
}

QList<int> intsFrom(const QJsonValue &value)
{
    QList<int> out;
    for (const QJsonValue &item : value.toArray())
        out.append(item.toInt());
    return out;
}

QStringList stringsFrom(const QJsonValue &value)
{
    QStringList out;
    for (const QJsonValue &item : value.toArray())
        out.append(item.toString());
    return out;
}

QJsonArray intsTo(const QList<int> &numbers)
{
    QJsonArray out;
    for (const int number : numbers)
        out.append(number);
    return out;
}

void setError(QString *error, const QJsonObject &result)
{
    const QString text = result.value(QStringLiteral("error")).toString();
    if (error && !text.isEmpty())
        *error = text;
}

QJsonObject eventInput(const PullEvent &ev)
{
    return {{QStringLiteral("type"), ev.type},
            {QStringLiteral("body"), ev.body},
            {QStringLiteral("state"), ev.state},
            {QStringLiteral("path"), ev.path},
            {QStringLiteral("side"), ev.side},
            {QStringLiteral("line"), ev.line},
            {QStringLiteral("lineStart"), ev.lineStart},
            {QStringLiteral("lineEnd"), ev.lineEnd},
            {QStringLiteral("suggestionPatch"), ev.suggestionPatch},
            {QStringLiteral("threadId"), ev.threadId},
            {QStringLiteral("parentId"), ev.parentId},
            {QStringLiteral("appliedCommit"), ev.appliedCommit},
            {QStringLiteral("patchset"), ev.patchset},
            {QStringLiteral("anchorLines"), ev.anchorLines}};
}

std::optional<QJsonObject> query(const Store &store, const QString &method, const QJsonObject &params,
                                 bool write, QString *error)
{
    QJsonObject result;
    const std::optional<bool> answered = call(store, method, params, error, &result, write);
    if (!answered)
        return std::nullopt;
    if (!*answered && result.isEmpty()) {
        result.insert(QStringLiteral("ok"), false);
        result.insert(QStringLiteral("transportFailed"), true);
    }
    return result;
}

} // namespace

void setBackend(Backend *backend)
{
    backendSlot().store(backend);
}

bool routed()
{
    Backend *backend = backendSlot().load();
    return backend && backend->available();
}

QJsonObject pullToJson(const PullRequest &pr)
{
    QJsonObject out = pr.toJson();
    QJsonArray events;
    for (const PullEvent &ev : pr.events)
        events.append(ev.toJson());
    out.insert(QStringLiteral("events"), events);
    out.insert(QStringLiteral("branchBacked"), pr.branchBacked);
    out.insert(QStringLiteral("mergeBase"), pr.mergeBase);
    out.insert(QStringLiteral("mergeHead"), pr.mergeHead);
    out.insert(QStringLiteral("filesChanged"), pr.filesChanged);
    out.insert(QStringLiteral("additions"), pr.additions);
    out.insert(QStringLiteral("deletions"), pr.deletions);
    out.insert(QStringLiteral("patchHeadOid"), pr.patchHeadOid);
    out.insert(QStringLiteral("archivedOnCodeBranch"), pr.archivedOnCodeBranch);
    return out;
}

PullRequest pullFromJson(const QJsonObject &obj)
{
    PullRequest pr = PullRequest::fromJson(obj);
    pr.branchBacked = obj.value(QStringLiteral("branchBacked")).toBool();
    pr.mergeBase = obj.value(QStringLiteral("mergeBase")).toString();
    pr.mergeHead = obj.value(QStringLiteral("mergeHead")).toString();
    pr.patchHeadOid = obj.value(QStringLiteral("patchHeadOid")).toString();
    pr.archivedOnCodeBranch = obj.value(QStringLiteral("archivedOnCodeBranch")).toBool();
    for (const QJsonValue &event : obj.value(QStringLiteral("events")).toArray())
        pr.events.append(PullEvent::fromJson(event.toObject()));
    return pr;
}

std::optional<bool> call(const Store &store, const QString &method, const QJsonObject &params,
                         QString *error, QJsonObject *result, bool write)
{
    Backend *backend = backendSlot().load();
    if (!backend || !backend->available())
        return std::nullopt;
    QJsonObject request = params;
    request.insert(QStringLiteral("store"), storeJson(store));
    const Reply reply = backend->call(method, request, write);
    if (!reply.answered) {
        if (reply.failed && write) {
            if (error)
                *error = QStringLiteral("forkmesh-core did not finish %1 (%2)").arg(method, reply.error);
            return false;
        }
        return std::nullopt;
    }
    if (result)
        *result = reply.result;
    setError(error, reply.result);
    return reply.result.value(QStringLiteral("ok")).toBool();
}

std::optional<QList<PullRequest>> loadAll(const Store &store)
{
    const QString key = store.workTree + QLatin1Char('\x1f') + store.mirror + QLatin1Char('\x1f') +
                        (identityValid(store) ? QLatin1Char('w') : QLatin1Char('r'));
    QJsonObject params;
    {
        const QMutexLocker lock(&loadCacheMutex());
        const auto cached = loadCache().constFind(key);
        if (cached != loadCache().constEnd())
            params.insert(QStringLiteral("stamp"), cached->stamp);
    }
    QJsonObject result;
    const std::optional<bool> ok = call(store, QStringLiteral("core.pulls.load"), params, nullptr, &result, false);
    if (!ok || !*ok)
        return std::nullopt;
    const QString stamp = result.value(QStringLiteral("stamp")).toString();
    const QMutexLocker lock(&loadCacheMutex());
    if (result.value(QStringLiteral("unchanged")).toBool()) {
        const auto cached = loadCache().constFind(key);
        if (cached != loadCache().constEnd() && cached->stamp == stamp)
            return cached->pulls;
        return std::nullopt;
    }
    QList<PullRequest> pulls = pullsFrom(result.value(QStringLiteral("pulls")));
    if (stamp.isEmpty()) {
        loadCache().remove(key);
    } else {
        if (!loadCache().contains(key) && loadCache().size() >= kLoadCacheEntries)
            loadCache().clear();
        loadCache().insert(key, LoadCacheEntry{stamp, pulls});
    }
    return pulls;
}

std::optional<QList<PullRequest>> loadAllIncludingArchived(const Store &store)
{
    QJsonObject result;
    const std::optional<bool> ok =
        call(store, QStringLiteral("core.pulls.loadArchived"), {}, nullptr, &result, false);
    if (!ok || !*ok)
        return std::nullopt;
    return pullsFrom(result.value(QStringLiteral("pulls")));
}

std::optional<int> createPull(const Store &store, const QString &title, const QString &description,
                              const QString &base, const QString &head, const QString &patch,
                              const QString &commits, bool branchBacked, QString *error)
{
    QJsonObject result;
    const std::optional<bool> ok =
        call(store, QStringLiteral("core.pulls.create"),
             {{QStringLiteral("title"), title},
              {QStringLiteral("description"), description},
              {QStringLiteral("base"), base},
              {QStringLiteral("head"), head},
              {QStringLiteral("patch"), patch},
              {QStringLiteral("commits"), commits},
              {QStringLiteral("branchBacked"), branchBacked}},
             error, &result);
    if (!ok)
        return std::nullopt;
    return *ok ? result.value(QStringLiteral("number")).toInt() : -1;
}

std::optional<bool> isBranchBehindBase(const Store &store, int number, bool *behind, QString *error,
                                       int *behindCount)
{
    if (behind)
        *behind = false;
    if (behindCount)
        *behindCount = 0;
    QJsonObject result;
    const std::optional<bool> ok = call(store, QStringLiteral("core.pulls.behind"),
                                        {{QStringLiteral("number"), number}}, error, &result, false);
    if (ok && *ok) {
        if (behind)
            *behind = result.value(QStringLiteral("behind")).toBool();
        if (behindCount)
            *behindCount = result.value(QStringLiteral("count")).toInt();
    }
    return ok;
}

std::optional<bool> checkMergeable(const Store &store, int number, bool *clean,
                                   QStringList *conflictFiles, QString *error)
{
    if (clean)
        *clean = false;
    if (conflictFiles)
        conflictFiles->clear();
    QJsonObject result;
    const std::optional<bool> ok = call(store, QStringLiteral("core.pulls.checkMergeable"),
                                        {{QStringLiteral("number"), number}}, error, &result, false);
    if (ok && *ok) {
        if (clean)
            *clean = result.value(QStringLiteral("clean")).toBool();
        if (conflictFiles)
            *conflictFiles = stringsFrom(result.value(QStringLiteral("conflicts")));
    }
    return ok;
}

std::optional<QList<int>> adoptForeignPulls(const Store &store, const QString &remoteUrl, QString *error,
                                            bool fetchFirst)
{
    const std::optional<QJsonObject> result =
        query(store, QStringLiteral("core.pulls.adoptForeign"),
              {{QStringLiteral("remoteUrl"), remoteUrl}, {QStringLiteral("fetchFirst"), fetchFirst}}, true,
              error);
    if (!result)
        return std::nullopt;
    return intsFrom(result->value(QStringLiteral("adopted")));
}

std::optional<QList<int>> reconcileMergedPulls(const Store &store, QString *error,
                                               QList<int> *rewardsAdvanced)
{
    if (rewardsAdvanced)
        rewardsAdvanced->clear();
    const std::optional<QJsonObject> result =
        query(store, QStringLiteral("core.pulls.reconcile"), {}, true, error);
    if (!result)
        return std::nullopt;
    if (rewardsAdvanced)
        *rewardsAdvanced = intsFrom(result->value(QStringLiteral("rewards")));
    return intsFrom(result->value(QStringLiteral("flipped")));
}

std::optional<bool> deletePulls(const Store &store, const QList<int> &numbers, bool rewriteHistory,
                                QList<int> *deleted, QList<int> *missing, QString *error)
{
    QJsonObject result;
    const std::optional<bool> ok =
        call(store, QStringLiteral("core.pulls.deleteMany"),
             {{QStringLiteral("numbers"), intsTo(numbers)}, {QStringLiteral("rewriteHistory"), rewriteHistory}},
             error, &result);
    if (!ok)
        return std::nullopt;
    if (deleted)
        *deleted = intsFrom(result.value(QStringLiteral("deleted")));
    if (missing)
        missing->append(intsFrom(result.value(QStringLiteral("missing"))));
    return ok;
}

std::optional<bool> addEvent(const Store &store, int number, const PullEvent &input, QString *error)
{
    QJsonObject params = eventInput(input);
    params.insert(QStringLiteral("number"), number);
    return call(store, QStringLiteral("core.pulls.event"), params, error);
}

std::optional<bool> addBranchEvent(const Store &store, const QString &branch, const PullEvent &input,
                                   QString *error)
{
    QJsonObject params = eventInput(input);
    params.insert(QStringLiteral("branch"), branch);
    return call(store, QStringLiteral("core.pulls.branchEvent"), params, error);
}

std::optional<bool> applyEvent(const Store &store, int number, const PullEvent &ev, QString *error)
{
    return call(store, QStringLiteral("core.pulls.applyEvent"),
                {{QStringLiteral("number"), number}, {QStringLiteral("event"), ev.toJson()}}, error);
}

std::optional<bool> applyBranchEvent(const Store &store, const QString &branch, const PullEvent &ev,
                                     QString *error)
{
    return call(store, QStringLiteral("core.pulls.applyBranchEvent"),
                {{QStringLiteral("branch"), branch}, {QStringLiteral("event"), ev.toJson()}}, error);
}

std::optional<QList<PullEvent>> readBranchReviewEvents(const Store &store, const QString &branch)
{
    QJsonObject result;
    const std::optional<bool> ok = call(store, QStringLiteral("core.pulls.branchEvents"),
                                        {{QStringLiteral("branch"), branch}}, nullptr, &result, false);
    if (!ok || !*ok)
        return std::nullopt;
    QList<PullEvent> events;
    for (const QJsonValue &item : result.value(QStringLiteral("events")).toArray())
        events.append(PullEvent::fromJson(item.toObject()));
    return events;
}

std::optional<PullStore::BranchSource> ensureLocalBranch(const Store &store, const QString &branch,
                                                         int *pullNumber, QString *error)
{
    if (pullNumber)
        *pullNumber = 0;
    QJsonObject result;
    const std::optional<bool> ok =
        call(store, QStringLiteral("core.pulls.ensureBranch"), {{QStringLiteral("branch"), branch}}, error,
             &result);
    if (!ok)
        return std::nullopt;
    const QString source = result.value(QStringLiteral("source")).toString();
    if (pullNumber)
        *pullNumber = result.value(QStringLiteral("pullNumber")).toInt();
    if (source == QLatin1String("local"))
        return PullStore::BranchSource::Local;
    if (source == QLatin1String("mirror"))
        return PullStore::BranchSource::Mirror;
    if (source == QLatin1String("pull"))
        return PullStore::BranchSource::Pull;
    return PullStore::BranchSource::Missing;
}

std::optional<bool> materializePullRef(const Store &store, const PullRequest &pr, QString *error)
{
    return call(store, QStringLiteral("core.pulls.ensureHeadRef"), {{QStringLiteral("pull"), pullToJson(pr)}},
                error);
}

std::optional<bool> checkoutForReview(const Store &store, const PullRequest &pr, const QString &dir,
                                      PullReviewCheckout *out, QString *error)
{
    QJsonObject result;
    const std::optional<bool> ok =
        call(store, QStringLiteral("core.pulls.reviewCheckout"),
             {{QStringLiteral("pull"), pullToJson(pr)}, {QStringLiteral("dir"), dir}}, error, &result);
    if (!ok)
        return std::nullopt;
    if (out)
        *out = PullReviewCheckout{};
    if (*ok && out) {
        const QJsonObject checkout = result.value(QStringLiteral("checkout")).toObject();
        out->path = checkout.value(QStringLiteral("path")).toString();
        out->baseOid = checkout.value(QStringLiteral("baseOid")).toString();
        out->headOid = checkout.value(QStringLiteral("headOid")).toString();
        out->commitCount = checkout.value(QStringLiteral("commitCount")).toInt();
        out->patch = checkout.value(QStringLiteral("patch")).toString();
        out->uncommitted = checkout.value(QStringLiteral("uncommitted")).toBool();
        out->conflicts = stringsFrom(checkout.value(QStringLiteral("conflicts")));
    }
    return ok;
}

bool discardReviewCheckout(const Store &store, const QString &dir)
{
    return call(store, QStringLiteral("core.pulls.discardReview"), {{QStringLiteral("dir"), dir}}, nullptr)
        .has_value();
}

std::optional<bool> sessionStart(const Store &store, int number, const QString &mode,
                                 const QString &path, QStringList *conflicted, bool *resolvedClean,
                                 QString *content, QString *error)
{
    if (conflicted)
        conflicted->clear();
    if (resolvedClean)
        *resolvedClean = false;
    QJsonObject result;
    const std::optional<bool> ok =
        call(store, QStringLiteral("core.pulls.session.start"),
             {{QStringLiteral("number"), number}, {QStringLiteral("mode"), mode}, {QStringLiteral("path"), path}},
             error, &result);
    if (!ok)
        return std::nullopt;
    if (conflicted)
        *conflicted = stringsFrom(result.value(QStringLiteral("conflicted")));
    if (resolvedClean)
        *resolvedClean = result.value(QStringLiteral("resolvedClean")).toBool();
    if (*ok && content)
        *content = result.value(QStringLiteral("content")).toString();
    return ok;
}

std::optional<bool> sessionFinish(const Store &store, int number, const QString &mode,
                                  const QString &path, const QString &content,
                                  const QString &message, QString *error)
{
    return call(store, QStringLiteral("core.pulls.session.finish"),
                {{QStringLiteral("number"), number},
                 {QStringLiteral("mode"), mode},
                 {QStringLiteral("path"), path},
                 {QStringLiteral("content"), content},
                 {QStringLiteral("message"), message}},
                error);
}

bool sessionAbort(const Store &store)
{
    return call(store, QStringLiteral("core.pulls.session.abort"), {}, nullptr).has_value();
}

std::optional<bool> sessionInProgress(const Store &store)
{
    QJsonObject result;
    const std::optional<bool> ok =
        call(store, QStringLiteral("core.pulls.session.state"), {}, nullptr, &result, false);
    if (!ok || !*ok)
        return std::nullopt;
    return result.value(QStringLiteral("inProgress")).toBool();
}

std::optional<QString> sessionEditWorkTree(const Store &store)
{
    QJsonObject result;
    const std::optional<bool> ok =
        call(store, QStringLiteral("core.pulls.session.state"), {}, nullptr, &result, false);
    if (!ok || !*ok)
        return std::nullopt;
    return result.value(QStringLiteral("editWorkTree")).toString();
}

bool validRewritePaths(int number, const QStringList &paths)
{
    static const QRegularExpression path(
        QStringLiteral("^\\.forkmesh/pulls/[1-9][0-9]*(/changes\\.patch|/commits\\.mbox)?$"));
    if (number <= 0 || paths.isEmpty())
        return false;
    for (const QString &candidate : paths)
        if (!path.match(candidate).hasMatch())
            return false;
    return true;
}

}
