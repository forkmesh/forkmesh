#include "ActionStore.h"
#include "HiddenTerms.h"
#include "SafeDelete.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QMutexLocker>
#include <QPair>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QSettings>
#include <QtConcurrentRun>

namespace {

QString legacySanitize(const QString &owner, const QString &name)
{
    QString key = owner + QStringLiteral("-") + name;
    for (QChar &c : key)
        if (!c.isLetterOrNumber() && c != QLatin1Char('-') && c != QLatin1Char('.'))
            c = QLatin1Char('_');
    return key;
}

QString canonicalRepositoryKey(const QString &owner, const QString &name)
{
    const QByteArray ownerBytes = owner.toUtf8();
    const QByteArray nameBytes = name.toUtf8();
    QByteArray material("forkmesh-actions-repository-v2\n");
    material.append(QByteArray::number(ownerBytes.size()));
    material.append(':');
    material.append(ownerBytes);
    material.append(QByteArray::number(nameBytes.size()));
    material.append(':');
    material.append(nameBytes);
    return QStringLiteral("v2-") +
           QString::fromLatin1(
               QCryptographicHash::hash(material, QCryptographicHash::Sha256)
                   .toHex());
}

bool runDirectoryMatches(const QString &directory, const ActionRun &run)
{
    QFile file(QDir(directory).filePath(QStringLiteral("meta.json")));
    if (!file.open(QIODevice::ReadOnly))
        return false;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    if (!document.isObject())
        return false;
    const ActionRun stored = ActionRun::fromJson(document.object());
    return stored.id == run.id && stored.owner == run.owner &&
           stored.name == run.name;
}

QString approvalKey(const QString &repoKey)
{
    return QStringLiteral("actions/approved/") + repoKey;
}

QByteArray reviewOnlyLegacyApprovals(const QByteArray &raw)
{
    const QJsonDocument document = QJsonDocument::fromJson(raw);
    if (!document.isObject())
        return {};
    QJsonObject approvals = document.object();
    for (auto it = approvals.begin(); it != approvals.end(); ++it) {
        if (!it.value().isObject())
            continue;
        QJsonObject approval = it.value().toObject();
        approval.insert(QStringLiteral("schema"), 0);
        approval.insert(QStringLiteral("legacyScopeUnverified"), true);
        it.value() = approval;
    }
    return QJsonDocument(approvals).toJson(QJsonDocument::Compact);
}

struct QueuedApprovals {
    QMutex mutex;
    QHash<QString, QJsonObject> byRepoKey;
};

QueuedApprovals &queuedApprovals()
{
    static QueuedApprovals instance;
    return instance;
}

QMutex &approvalWriteMutex()
{
    static QMutex instance;
    return instance;
}

QJsonObject queuedApprovalsFor(const QString &repoKey)
{
    QueuedApprovals &queued = queuedApprovals();
    QMutexLocker lock(&queued.mutex);
    return queued.byRepoKey.value(repoKey);
}

QJsonObject storedApprovals(const QString &repoKey)
{
    const QJsonObject queued = queuedApprovalsFor(repoKey);
    QJsonObject approvals =
        QJsonDocument::fromJson(
            QSettings().value(approvalKey(repoKey)).toByteArray())
            .object();
    for (auto it = queued.constBegin(); it != queued.constEnd(); ++it)
        approvals.insert(it.key(), it.value());
    return approvals;
}

void writeQueuedApprovals(const QString &repoKey)
{
    QMutexLocker writeLock(&approvalWriteMutex());
    const QJsonObject queued = queuedApprovalsFor(repoKey);
    if (queued.isEmpty())
        return;
    {
        QSettings settings;
        QJsonObject approvals =
            QJsonDocument::fromJson(
                settings.value(approvalKey(repoKey)).toByteArray())
                .object();
        for (auto it = queued.constBegin(); it != queued.constEnd(); ++it)
            approvals.insert(it.key(), it.value());
        settings.setValue(approvalKey(repoKey),
                          QJsonDocument(approvals).toJson(
                              QJsonDocument::Compact));
    }
    QueuedApprovals &pending = queuedApprovals();
    QMutexLocker lock(&pending.mutex);
    const auto entry = pending.byRepoKey.find(repoKey);
    if (entry == pending.byRepoKey.end())
        return;
    for (auto it = queued.constBegin(); it != queued.constEnd(); ++it)
        if (entry->value(it.key()) == it.value())
            entry->remove(it.key());
    if (entry->isEmpty())
        pending.byRepoKey.erase(entry);
}

} // namespace

bool ActionNeeds::matches(const QString &token, const QString &workflowName,
                          const QString &workflowPath)
{
    const QString wanted = token.trimmed();
    if (wanted.isEmpty())
        return false;
    const QFileInfo info(workflowPath);
    const QStringList aliases{workflowName.trimmed(), workflowPath.trimmed(),
                              info.fileName(), info.completeBaseName()};
    for (const QString &alias : aliases)
        if (!alias.isEmpty() &&
            QString::compare(alias, wanted, Qt::CaseInsensitive) == 0)
            return true;
    return false;
}

ActionNeeds::State ActionNeeds::resolve(const ActionRun &run,
                                        const QStringList &needs,
                                        const QList<ActionRun> &history,
                                        QString *detail)
{
    State state = State::Ready;
    for (const QString &token : needs) {
        const ActionRun *dependency = nullptr;
        for (const ActionRun &candidate : history) {
            if (candidate.id == run.id || candidate.owner != run.owner ||
                candidate.name != run.name || candidate.commit != run.commit)
                continue;
            if (!matches(token, candidate.workflowName, candidate.workflowPath))
                continue;
            dependency = &candidate;
            break;
        }
        if (!dependency)
            continue; // not scheduled here; never a barrier
        if (dependency->status == ActionStatus::Success)
            continue;
        const bool pending =
            dependency->status == ActionStatus::Queued ||
            dependency->status == ActionStatus::Running ||
            dependency->status == ActionStatus::AwaitingApproval;
        if (!pending) {
            if (detail)
                *detail = QStringLiteral("\"%1\" %2")
                              .arg(dependency->workflowName,
                                   dependency->status);
            return State::Blocked; // can never become ready
        }
        if (state == State::Ready) {
            state = State::Waiting;
            if (detail)
                *detail = QStringLiteral("\"%1\" is %2")
                              .arg(dependency->workflowName,
                                   dependency->status);
        }
    }
    return state;
}

bool ActionRun::requeueAfterDesktopInterruption()
{
    if (status != ActionStatus::Running)
        return false;
    status = ActionStatus::Queued;
    startedAtMs = 0;
    finishedAtMs = 0;
    stepStatuses.clear();
    currentStepIndex = -1;
    return true;
}

QString ActionRun::repoKey() const
{
    return canonicalRepositoryKey(owner, name);
}

QString ActionRun::legacyRepoKey() const
{
    return legacySanitize(owner, name);
}

QJsonObject ActionRun::toJson() const
{
    QJsonObject o;
    o["id"] = id;
    o["owner"] = owner;
    o["name"] = name;
    o["workflowPath"] = workflowPath;
    o["workflowName"] = workflowName;
    o["workflowContent"] = workflowContent;
    o["commit"] = commit;
    o["ref"] = ref;
    if (!machine.isEmpty())
        o["machine"] = machine;
    o["repositoryTree"] = repositoryTree;
    o["executionDigest"] = executionDigest;
    o["status"] = status;
    QJsonArray serializedSteps;
    for (const QString &stepStatus : stepStatuses)
        serializedSteps.append(stepStatus);
    o["stepStatuses"] = serializedSteps;
    o["currentStepIndex"] = currentStepIndex;
    if (pullNumber > 0) {
        o["pullNumber"] = pullNumber;
        o["pullAuthor"] = pullAuthor;
        o["firstTimePullAuthor"] = firstTimePullAuthor;
    }
    o["createdAtMs"] = createdAtMs;
    o["startedAtMs"] = startedAtMs;
    o["finishedAtMs"] = finishedAtMs;
    return o;
}

ActionRun ActionRun::fromJson(const QJsonObject &o)
{
    ActionRun r;
    r.id = o.value("id").toInt();
    r.owner = o.value("owner").toString();
    r.name = o.value("name").toString();
    r.workflowPath = o.value("workflowPath").toString();
    r.workflowName = o.value("workflowName").toString();
    r.workflowContent = o.value("workflowContent").toString();
    r.commit = o.value("commit").toString();
    r.ref = o.value("ref").toString();
    r.machine = o.value("machine").toString();
    r.repositoryTree = o.value("repositoryTree").toString();
    r.executionDigest = o.value("executionDigest").toString();
    r.status = o.value("status").toString(ActionStatus::Queued);
    for (const QJsonValue &value :
         o.value("stepStatuses").toArray()) {
        const QString stepStatus = value.toString().trimmed();
        if (!stepStatus.isEmpty())
            r.stepStatuses.append(stepStatus);
    }
    r.currentStepIndex = o.value("currentStepIndex").toInt(-1);
    r.pullNumber = o.value("pullNumber").toInt();
    r.pullAuthor = o.value("pullAuthor").toString();
    r.firstTimePullAuthor = o.value("firstTimePullAuthor").toBool();
    r.createdAtMs = o.value("createdAtMs").toVariant().toLongLong();
    r.startedAtMs = o.value("startedAtMs").toVariant().toLongLong();
    r.finishedAtMs = o.value("finishedAtMs").toVariant().toLongLong();
    return r;
}

ActionStore::ActionStore(QString rootDir) : m_root(std::move(rootDir))
{
    QDir().mkpath(runsDir());
    QDir().mkpath(spoolDir());
    // migrateLegacyRecords() used to run here. It opens and parses every run
    // record on disk, so on a node with a few thousand runs it cost ~155 ms of
    // the pre-event-loop launch window on every single start, forever. It now
    // runs from ensureRunCacheLoadedLocked() instead: still exactly once per
    // process, still before any run record is read or a new id is handed out,
    // but on whichever thread/turn first actually needs the run history.
}

QString ActionStore::runsDir() const { return m_root + QStringLiteral("/runs"); }
QString ActionStore::spoolDir() const { return m_root + QStringLiteral("/spool"); }

bool ActionStore::spoolPushEvent(const QString &owner, const QString &name,
                                 const QString &commit, const QString &ref,
                                 const QString &before) const
{
    static const QRegularExpression objectId(
        QStringLiteral("^(?:[0-9a-f]{40}|[0-9a-f]{64})$"));
    static const QRegularExpression field(QStringLiteral("^[^\\s]+$"));
    const QString lowered = commit.trimmed().toLower();
    if (!objectId.match(lowered).hasMatch() ||
        !ref.startsWith(QLatin1String("refs/heads/")) ||
        !field.match(ref).hasMatch() || !field.match(owner).hasMatch() ||
        !field.match(name).hasMatch())
        return false;
    const QString dir = spoolDir();
    if (!QDir().mkpath(dir))
        return false;
    const QString base =
        QStringLiteral("%1-%2-%3.push")
            .arg(QDateTime::currentSecsSinceEpoch())
            .arg(QCoreApplication::applicationPid())
            .arg(lowered.left(12));
    const QString path = QDir(dir).filePath(base);
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    const QString loweredBefore = before.trimmed().toLower();
    const QString oldValue =
        objectId.match(loweredBefore).hasMatch() &&
                loweredBefore.size() == lowered.size()
            ? loweredBefore
            : QString(lowered.size(), QLatin1Char('0'));
    const QByteArray text =
        QStringLiteral("owner %1\nname %2\nref %3 %4 %5\n")
            .arg(owner, name, oldValue, lowered, ref)
            .toUtf8();
    return file.write(text) == text.size() && file.commit();
}
QString ActionStore::artifactsDir() const
{
    const QString path = m_root + QStringLiteral("/artifacts");
    QDir().mkpath(path);
    return path;
}

QString ActionStore::sandboxDir() const
{
    const QString path = m_root + QStringLiteral("/sandbox");
    QDir().mkpath(path);
    return path;
}

QString ActionStore::cacheRoot() const
{
    return m_root + QStringLiteral("/cache");
}

QString ActionStore::buildCacheDir(const ActionRun &run) const
{
    const QString root =
        m_root + QStringLiteral("/cache/") + run.repoKey();
    const QString path = run.workflowPath ==
                                 QStringLiteral(".forkmesh/desktop-ci.yml")
                             ? root
                             : root + QStringLiteral("/workflow-") +
                                   QString::fromLatin1(
                                       QCryptographicHash::hash(
                                           run.workflowPath.toUtf8(),
                                           QCryptographicHash::Sha256)
                                           .toHex());
    QDir().mkpath(path);
    return path;
}

QString ActionStore::toolchainBinDir() const
{
    const QString path = m_root + QStringLiteral("/toolchain/bin");
    QDir().mkpath(path);
    return path;
}

QString ActionStore::runDir(const ActionRun &run) const
{
    return runsDir() + QLatin1Char('/') + run.repoKey() + QLatin1Char('/') +
           QString::number(run.id);
}

QString ActionStore::legacyRunDir(const ActionRun &run) const
{
    return runsDir() + QLatin1Char('/') + run.legacyRepoKey() +
           QLatin1Char('/') + QString::number(run.id);
}

QString ActionStore::existingRunDir(const ActionRun &run) const
{
    const QString canonical = runDir(run);
    if (QFileInfo::exists(canonical))
        return canonical;
    const QString legacy = legacyRunDir(run);
    return runDirectoryMatches(legacy, run) ? legacy : canonical;
}

// code-health: allow(dir-walk-in-loop) once per process from ensureRunCacheLoadedLocked(), first reached on restoreActionRuns()' worker
void ActionStore::migrateLegacyRecords() const
{
    QDir root(runsDir());
    const QStringList repositoryDirectories =
        root.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    QMap<QString, QSet<QString>> canonicalKeysByLegacyKey;

    for (const QString &repositoryDirectory : repositoryDirectories) {
        QDir repositoryDir(root.filePath(repositoryDirectory));
        const QStringList ids =
            repositoryDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QString &id : ids) {
            QFile meta(repositoryDir.filePath(id) +
                       QStringLiteral("/meta.json"));
            if (!meta.open(QIODevice::ReadOnly))
                continue;
            const QJsonDocument document =
                QJsonDocument::fromJson(meta.readAll());
            if (!document.isObject())
                continue;
            const ActionRun run = ActionRun::fromJson(document.object());
            if (run.id <= 0 || QString::number(run.id) != id ||
                run.owner.isEmpty() || run.name.isEmpty()) {
                continue;
            }

            canonicalKeysByLegacyKey[run.legacyRepoKey()].insert(
                run.repoKey());
            if (repositoryDirectory == run.repoKey())
                continue;

            const QString destinationParent =
                root.filePath(run.repoKey());
            const QString destination =
                QDir(destinationParent).filePath(id);
            if (!QFileInfo::exists(destination) &&
                QDir().mkpath(destinationParent)) {
                QDir().rename(repositoryDir.filePath(id), destination);
            }

            const QString oldArtifact =
                QDir(m_root + QStringLiteral("/artifacts/") +
                     run.legacyRepoKey())
                    .filePath(id);
            const QString newArtifactParent =
                m_root + QStringLiteral("/artifacts/") + run.repoKey();
            const QString newArtifact =
                QDir(newArtifactParent).filePath(id);
            if (QFileInfo::exists(oldArtifact) &&
                !QFileInfo::exists(newArtifact) &&
                QDir().mkpath(newArtifactParent)) {
                QDir().rename(oldArtifact, newArtifact);
            }
        }
        if (repositoryDir.entryList(QDir::AllEntries |
                                    QDir::NoDotAndDotDot)
                .isEmpty()) {
            root.rmdir(repositoryDirectory);
        }
    }

    // A lossy legacy approval can be associated for review only when every
    // persisted run under that old key names one canonical repository. Even
    // then it is downgraded so it can show the previous YAML diff but cannot
    // execute or receive secrets without a fresh approval. If old records
    // prove a collision, leave the ambiguous record behind and fail closed.
    QSettings settings;
    for (auto it = canonicalKeysByLegacyKey.constBegin();
         it != canonicalKeysByLegacyKey.constEnd(); ++it) {
        if (it.value().size() != 1)
            continue;
        const QString oldKey = approvalKey(it.key());
        if (!settings.contains(oldKey))
            continue;
        const QByteArray raw = settings.value(oldKey).toByteArray();
        const QByteArray reviewOnly = reviewOnlyLegacyApprovals(raw);
        if (reviewOnly.isEmpty())
            continue;
        const QString newKey = approvalKey(*it.value().constBegin());
        if (!settings.contains(newKey))
            settings.setValue(newKey, reviewOnly);
        settings.sync();
        if (settings.status() == QSettings::NoError &&
            (settings.value(newKey).toByteArray() == reviewOnly ||
             settings.contains(newKey))) {
            settings.remove(oldKey);
            settings.sync();
        }
    }
}

// code-health: allow(dir-walk-in-loop) once per process, first reached from restoreActionRuns()' QtConcurrent worker; later calls return from m_runCache
void ActionStore::ensureRunCacheLoadedLocked() const
{
    if (m_runCacheLoaded)
        return;
    // Repository-key migration has to finish before the scan below, or runs
    // still under a legacy directory name would be cached from the old path.
    migrateLegacyRecords();
#ifdef FORKMESH_ACTION_STORE_TESTS
    ++m_runDirectoryScanCount;
#endif

    m_runCache.clear();
    int maxId = 0;
    QDir root(runsDir());
    const QStringList repos =
        root.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &repo : repos) {
        const QDir repoDir(root.filePath(repo));
        const QStringList ids =
            repoDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QString &id : ids) {
            bool numericId = false;
            const int directoryId = id.toInt(&numericId);
            if (numericId && directoryId > 0)
                maxId = qMax(maxId, directoryId);
            QFile f(repoDir.filePath(id) + QStringLiteral("/meta.json"));
            if (!f.open(QIODevice::ReadOnly))
                continue;
            const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
            if (!doc.isObject())
                continue;
            const ActionRun run = ActionRun::fromJson(doc.object());
            if (run.id <= 0)
                continue;
            m_runCache.insert(run.id, run);
            maxId = qMax(maxId, run.id);
        }
    }
    m_nextRunId = maxId + 1;
    m_runCacheLoaded = true;
}

int ActionStore::nextId() const
{
    QMutexLocker lock(&m_runCacheMutex);
    ensureRunCacheLoadedLocked();
    return m_nextRunId++;
}

ActionRun ActionStore::createRun(ActionRun run)
{
    run.id = nextId();
    run.createdAtMs = QDateTime::currentMSecsSinceEpoch();
    QDir().mkpath(runDir(run));
    saveRun(run);
    return run;
}

bool ActionStore::saveRun(const ActionRun &run) const
{
    const QString directory = existingRunDir(run);
    QDir().mkpath(directory);
    QFile f(directory + QStringLiteral("/meta.json"));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    f.write(QJsonDocument(run.toJson()).toJson(QJsonDocument::Indented));
    f.close();
    QMutexLocker lock(&m_runCacheMutex);
    if (m_runCacheLoaded)
        m_runCache.insert(run.id, run);
    if (run.id >= m_nextRunId)
        m_nextRunId = run.id + 1;
    return true;
}

bool ActionStore::deleteRun(const ActionRun &run) const
{
    bool removed = true;
    const QString canonicalPath = runDir(run);
    if (QDir(canonicalPath).exists())
        removed = forkmesh::safedelete::removeTree(
            canonicalPath, QFileInfo(canonicalPath).absolutePath());
    const QString legacyPath = legacyRunDir(run);
    if (runDirectoryMatches(legacyPath, run))
        removed = forkmesh::safedelete::removeTree(
                      legacyPath, QFileInfo(legacyPath).absolutePath()) &&
                  removed;
    if (removed) {
        QMutexLocker lock(&m_runCacheMutex);
        if (m_runCacheLoaded)
            m_runCache.remove(run.id);
        // Deliberately do not rewind m_nextRunId. Reusing a just-deleted id can
        // attach late signals or cached UI state to an unrelated new run.
    }
    return removed;
}

// code-health: allow(dir-walk-in-loop) the recursive removals run inside the QtConcurrent::run worker below
void ActionStore::deleteRunsInBackground(const QList<ActionRun> &runs) const
{
    if (runs.isEmpty())
        return;
    // "Clear runs" deleted a whole run tree — meta plus its log — per run, and a
    // 6.6 s stall was sampled inside those recursive removals on the GUI thread.
    // Drop the runs from the cache here, so every reader agrees they are gone the
    // moment this returns, and unlink the trees on a worker. Only paths and
    // plain-value runs cross the thread boundary, so nothing here depends on this
    // store outliving the deletion.
    struct Doomed {
        QString canonical;
        QString legacy;
        ActionRun run;
    };
    QList<Doomed> doomed;
    doomed.reserve(runs.size());
    for (const ActionRun &run : runs)
        doomed.append({runDir(run), legacyRunDir(run), run});
    {
        QMutexLocker lock(&m_runCacheMutex);
        if (m_runCacheLoaded)
            for (const ActionRun &run : runs)
                m_runCache.remove(run.id);
    }
    (void)QtConcurrent::run([doomed = std::move(doomed)] {
        for (const Doomed &item : doomed) {
            if (QDir(item.canonical).exists())
                forkmesh::safedelete::removeTree(
                    item.canonical, QFileInfo(item.canonical).absolutePath());
            if (runDirectoryMatches(item.legacy, item.run))
                forkmesh::safedelete::removeTree(
                    item.legacy, QFileInfo(item.legacy).absolutePath());
        }
    });
}

void ActionStore::appendLog(const ActionRun &run, const QString &text) const
{
    const QString directory = existingRunDir(run);
    QDir().mkpath(directory);
    QFile f(directory + QStringLiteral("/log.txt"));
    if (!f.open(QIODevice::Append | QIODevice::Text))
        return;
    f.write(text.toUtf8());
    if (!text.endsWith(QLatin1Char('\n')))
        f.write("\n");
}

QString ActionStore::readLog(const ActionRun &run) const
{
    QFile f(logPath(run));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return QString();
    return QString::fromUtf8(f.readAll());
}

QString ActionStore::logPath(const ActionRun &run) const
{
    return existingRunDir(run) + QStringLiteral("/log.txt");
}

QList<ActionRun> ActionStore::loadAllRuns() const
{
    QMutexLocker lock(&m_runCacheMutex);
    ensureRunCacheLoadedLocked();
    QList<ActionRun> runs = m_runCache.values();
    std::sort(runs.begin(), runs.end(),
              [](const ActionRun &a, const ActionRun &b) {
                  return a.createdAtMs > b.createdAtMs;
              });
    return runs;
}

#ifdef FORKMESH_ACTION_STORE_TESTS
int ActionStore::runDirectoryScanCountForTesting() const
{
    QMutexLocker lock(&m_runCacheMutex);
    return m_runDirectoryScanCount;
}
#endif


QMap<QString, QString> ActionStore::variables()
{
    QMap<QString, QString> out;
    const QByteArray raw =
        QSettings().value(QStringLiteral("actions/variables")).toByteArray();
    const QJsonObject obj = QJsonDocument::fromJson(raw).object();
    for (auto it = obj.constBegin(); it != obj.constEnd(); ++it)
        out.insert(it.key(), it.value().toString());
    return out;
}

void ActionStore::setVariables(const QMap<QString, QString> &vars)
{
    QJsonObject obj;
    for (auto it = vars.constBegin(); it != vars.constEnd(); ++it)
        obj.insert(it.key(), it.value());
    QSettings().setValue(QStringLiteral("actions/variables"),
                         QJsonDocument(obj).toJson(QJsonDocument::Compact));
    forkmesh::hiddenterms::refreshVariableValues(vars);
}


bool ActionStore::isApproved(const QString &repoKey, const QString &path,
                             const QString &content,
                             const QString &repositoryTree,
                             const QString &executionDigest)
{
    if (repositoryTree.isEmpty() || executionDigest.isEmpty())
        return false;
    const QJsonValue stored = storedApprovals(repoKey).value(path);
    if (!stored.isObject())
        return false; // legacy YAML-only approvals deliberately fail closed
    const QJsonObject approval = stored.toObject();
    return approval.value(QStringLiteral("schema")).toInt() == 2 &&
           approval.value(QStringLiteral("workflowContent")).toString() == content &&
           approval.value(QStringLiteral("repositoryTree")).toString() ==
               repositoryTree &&
           approval.value(QStringLiteral("executionDigest")).toString() ==
               executionDigest;
}

void ActionStore::approve(const QString &repoKey, const QString &path,
                          const QString &content,
                          const QString &repositoryTree,
                          const QString &executionDigest)
{
    if (repositoryTree.isEmpty() || executionDigest.isEmpty())
        return; // an unbound approval is never persisted
    {
        QueuedApprovals &queued = queuedApprovals();
        QMutexLocker lock(&queued.mutex);
        queued.byRepoKey[repoKey].insert(
            path,
            QJsonObject{
                {QStringLiteral("schema"), 2},
                {QStringLiteral("workflowContent"), content},
                {QStringLiteral("repositoryTree"), repositoryTree},
                {QStringLiteral("executionDigest"), executionDigest},
            });
    }
    (void)QtConcurrent::run([repoKey] { writeQueuedApprovals(repoKey); });
}

QString ActionStore::lastApprovedContent(const QString &repoKey,
                                         const QString &path)
{
    const QJsonValue value = storedApprovals(repoKey).value(path);
    return value.isObject()
               ? value.toObject()
                     .value(QStringLiteral("workflowContent"))
                     .toString()
               : value.toString();
}

bool ActionStore::repositoryStateDigest(const QString &repository,
                                        const QString &commit,
                                        QString *repositoryTree,
                                        QString *executionDigest,
                                        QString *error)
{
    if (repositoryTree)
        repositoryTree->clear();
    if (executionDigest)
        executionDigest->clear();
    if (error)
        error->clear();
    if (repository.trimmed().isEmpty() || commit.trimmed().isEmpty()) {
        if (error)
            *error = QStringLiteral("repository and commit are required");
        return false;
    }

    struct DigestMemo {
        QMutex mutex;
        QHash<QString, QPair<QString, QString>> entries;
        QStringList order; // insertion order, for eviction
    };
    static DigestMemo memo;
    const QString memoKey = QDir(repository).absolutePath() +
                            QLatin1Char('\0') + commit.trimmed().toLower();
    {
        QMutexLocker lock(&memo.mutex);
        const auto hit = memo.entries.constFind(memoKey);
        if (hit != memo.entries.constEnd()) {
            if (repositoryTree)
                *repositoryTree = hit->first;
            if (executionDigest)
                *executionDigest = hit->second;
            return true;
        }
    }
    auto remember = [&memoKey](const QString &tree, const QString &digest) {
        constexpr int kMaxEntries = 64;
        QMutexLocker lock(&memo.mutex);
        if (memo.entries.contains(memoKey))
            return;
        memo.entries.insert(memoKey, {tree, digest});
        memo.order.append(memoKey);
        while (memo.order.size() > kMaxEntries)
            memo.entries.remove(memo.order.takeFirst());
    };

    auto fail = [&](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    auto capture = [&](const QStringList &arguments, int timeoutMs,
                       QByteArray *output) {
        QProcess process;
        process.setProcessChannelMode(QProcess::SeparateChannels);
        process.start(QStringLiteral("git"), arguments);
        if (!process.waitForStarted(3000))
            return false;
        if (!process.waitForFinished(timeoutMs)) {
            process.kill();
            process.waitForFinished(1000);
            return false;
        }
        if (process.exitStatus() != QProcess::NormalExit ||
            process.exitCode() != 0)
            return false;
        *output = process.readAllStandardOutput();
        return true;
    };

    const QStringList base{QStringLiteral("-C"), repository};
    QByteArray treeBytes;
    if (!capture(base +
                     QStringList{QStringLiteral("rev-parse"),
                                 QStringLiteral("--verify"),
                                 commit + QStringLiteral("^{tree}")},
                 10000, &treeBytes)) {
        return fail(QStringLiteral("could not resolve the repository tree"));
    }
    const QString tree = QString::fromLatin1(treeBytes).trimmed().toLower();
    static const QRegularExpression objectId(
        QStringLiteral("^(?:[0-9a-f]{40}|[0-9a-f]{64})$"));
    if (!objectId.match(tree).hasMatch())
        return fail(QStringLiteral("repository returned an invalid tree id"));

    // Hash the raw recursive manifest first. This binds executable bits,
    // symlinks, paths, object ids, and submodule commit pins.
    QByteArray manifest;
    if (!capture(base +
                     QStringList{QStringLiteral("ls-tree"),
                                 QStringLiteral("-r"), QStringLiteral("-z"),
                                 QStringLiteral("--full-tree"), commit},
                 120000, &manifest)) {
        return fail(QStringLiteral("could not read the recursive repository tree"));
    }
    QCryptographicHash digest(QCryptographicHash::Sha256);
    digest.addData(QByteArray("forkmesh-action-snapshot-v2\0", 28));
    digest.addData(tree.toLatin1());
    digest.addData(QByteArray("\0tree-manifest\0", 15));
    digest.addData(manifest);
    digest.addData(QByteArray("\0archive\0", 9));

    QProcess archive;
    archive.setProcessChannelMode(QProcess::SeparateChannels);
    archive.start(QStringLiteral("git"),
                  base +
                      QStringList{QStringLiteral("archive"),
                                  QStringLiteral("--format=tar"), commit});
    if (!archive.waitForStarted(3000))
        return fail(QStringLiteral("could not start repository snapshot hashing"));
    QElapsedTimer timer;
    timer.start();
    constexpr qint64 kDigestTimeoutMs = 5 * 60 * 1000;
    while (archive.state() != QProcess::NotRunning) {
        if (archive.waitForReadyRead(100))
            digest.addData(archive.readAllStandardOutput());
        if (timer.elapsed() > kDigestTimeoutMs) {
            archive.kill();
            archive.waitForFinished(1000);
            return fail(QStringLiteral("repository snapshot hashing timed out"));
        }
    }
    digest.addData(archive.readAllStandardOutput());
    if (archive.exitStatus() != QProcess::NormalExit ||
        archive.exitCode() != 0) {
        return fail(QStringLiteral("could not archive the repository snapshot"));
    }

    const QString execution = QString::fromLatin1(digest.result().toHex());
    remember(tree, execution); // successes only; a failure must be retried
    if (repositoryTree)
        *repositoryTree = tree;
    if (executionDigest)
        *executionDigest = execution;
    return true;
}
