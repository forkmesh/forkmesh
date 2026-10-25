#include "AgentStore.h"
#include "OrderedAppendWriter.h"
#include "SafeDelete.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMetaObject>
#include <QMutex>
#include <QMutexLocker>
#include <QPointer>
#include <QQueue>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QTemporaryFile>
#include <QThreadPool>
#include <QThread>
#include <QVariant>
#include <QWaitCondition>

#include <algorithm>
#include <atomic>
#include <memory>
#include <optional>
#include <utility>

#ifdef Q_OS_UNIX
#include <fcntl.h>
#include <unistd.h>
#endif

namespace {

using AgentTranscriptWriter = forkmesh::OrderedAppendWriter;

AgentTranscriptWriter &transcriptEventWriter()
{
    // Every AgentStore instance shares the same event writer, so copies used by
    // background scans can never concurrently append events.jsonl.
    static AgentTranscriptWriter writer;
    return writer;
}

std::atomic<bool> g_logObserved{false};

QMutex &logObserverMutex()
{
    static QMutex mutex;
    return mutex;
}

QPointer<QObject> &logObserverContext()
{
    static QPointer<QObject> context;
    return context;
}

std::function<void(int)> &logObserver()
{
    static std::function<void(int)> observer;
    return observer;
}

void notifyLogObserver(int sessionId)
{
    if (!g_logObserved.load(std::memory_order_relaxed))
        return;
    QMutexLocker locker(&logObserverMutex());
    QObject *context = logObserverContext().data();
    const std::function<void(int)> observer = logObserver();
    if (!context || !observer)
        return;
    QMetaObject::invokeMethod(
        context, [observer, sessionId] { observer(sessionId); }, Qt::QueuedConnection);
}

AgentTranscriptWriter &transcriptLogWriter()
{
    // Logs use their own ordered lane.  A multi-megabyte JSON serialization can
    // therefore never make a synchronous readLog() wait behind unrelated event
    // persistence on the GUI thread.
    static AgentTranscriptWriter writer;
    return writer;
}

// A deleted session has to stay deleted. Its directory is the whole record, and
// every writer here calls mkpath first, so one late write — a saveSession from an
// in-memory copy the deletion hasn't reached yet, a transcript append from a
// runner still winding down — recreates it and the next reloadAgents() reads the
// session straight back into the Agents tab. "Merge & clean up" hit exactly that:
// the org-task completion POST is fired from inside the delete, and its reply
// lands a second later and re-saves meta.json (adhoc #1718). Tombstones are
// process-wide because background scans hold their own AgentStore copies of the
// same root, and createSession lifts one so a recycled id (nextId hands the top
// id back out once its session is gone) can be written again.
QMutex &retiredSessionMutex()
{
    static QMutex mutex;
    return mutex;
}

QSet<QString> &retiredSessionDirs()
{
    static QSet<QString> dirs;
    return dirs;
}

QString retiredKey(const QString &dir)
{
    return QDir::cleanPath(QDir(dir).absolutePath());
}

bool sessionRetired(const QString &dir)
{
    const QMutexLocker lock(&retiredSessionMutex());
    // Streaming appends ask this per event, and nothing is usually retired, so
    // the common answer costs a lock and an empty check rather than a path walk.
    if (retiredSessionDirs().isEmpty())
        return false;
    return retiredSessionDirs().contains(retiredKey(dir));
}

void retireSession(const QString &dir)
{
    const QMutexLocker lock(&retiredSessionMutex());
    retiredSessionDirs().insert(retiredKey(dir));
}

void reviveSession(const QString &dir)
{
    const QMutexLocker lock(&retiredSessionMutex());
    retiredSessionDirs().remove(retiredKey(dir));
}

struct CachedSessionMeta {
    QString stamp;
    std::optional<AgentSession> session;
};

struct SessionMetaCache {
    QMutex mutex;
    quint64 generation = 0;
    QHash<QString, CachedSessionMeta> entries;
};

SessionMetaCache &sessionMetaCache()
{
    static SessionMetaCache cache;
    return cache;
}

QString sessionFileStamp(const QString &path)
{
    const QFileInfo info(path);
    if (!info.exists())
        return QStringLiteral("-");
    return QString::number(info.size()) + QLatin1Char(':') +
           QString::number(info.lastModified().toMSecsSinceEpoch());
}

struct WrittenSessionMeta {
    QByteArray bytes;
    QString stamp;
};

struct WrittenSessionMetas {
    QMutex mutex;
    QHash<QString, WrittenSessionMeta> entries;
};

WrittenSessionMetas &writtenSessionMetas()
{
    static WrittenSessionMetas written;
    return written;
}

QThreadPool &sessionMetaSyncPool()
{
    static QThreadPool *pool = [] {
        auto *created = new QThreadPool;
        created->setMaxThreadCount(1);
        created->setExpiryTimeout(30000);
        return created;
    }();
    return *pool;
}

bool replaceSessionMeta(const QString &path, const QByteArray &bytes)
{
#ifdef Q_OS_UNIX
    QTemporaryFile temp(path + QStringLiteral(".XXXXXX"));
    temp.setAutoRemove(true);
    if (!temp.open())
        return false;
    if (temp.write(bytes) != bytes.size() || !temp.flush())
        return false;
    const QByteArray tempName = QFile::encodeName(temp.fileName());
    const QByteArray finalName = QFile::encodeName(path);
    temp.close();
    if (::rename(tempName.constData(), finalName.constData()) != 0)
        return false;
    temp.setAutoRemove(false);
    sessionMetaSyncPool().start([finalName] {
        const int fd = ::open(finalName.constData(), O_RDONLY | O_CLOEXEC);
        if (fd < 0)
            return;
        ::fsync(fd);
        ::close(fd);
    });
    return true;
#else
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    file.write(bytes);
    return file.commit();
#endif
}

void forgetSessionMeta(const QString &path)
{
    SessionMetaCache &cache = sessionMetaCache();
    const QMutexLocker lock(&cache.mutex);
    ++cache.generation;
    cache.entries.remove(path);
}

std::optional<AgentSession> loadSessionMeta(const QString &dir, bool recycled,
                                            QSet<QString> *seen)
{
    const QString path = dir + QStringLiteral("/meta.json");
    const QString stampPath = dir + QStringLiteral("/deleted-at.json");
    seen->insert(path);
    SessionMetaCache &cache = sessionMetaCache();
    quint64 generation = 0;
    {
        const QMutexLocker lock(&cache.mutex);
        generation = cache.generation;
    }
    QString stamp = sessionFileStamp(path);
    if (recycled)
        stamp += QLatin1Char('|') + sessionFileStamp(stampPath);
    {
        const QMutexLocker lock(&cache.mutex);
        const auto it = cache.entries.constFind(path);
        if (it != cache.entries.cend() && it->stamp == stamp)
            return it->session;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return std::nullopt;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    std::optional<AgentSession> session;
    if (doc.isObject()) {
        session = AgentSession::fromJson(doc.object());
        QFile deletedAt(stampPath);
        if (recycled && deletedAt.open(QIODevice::ReadOnly)) {
            session->deletedAtMs = QJsonDocument::fromJson(deletedAt.readAll())
                                       .object()
                                       .value(QStringLiteral("deletedAtMs"))
                                       .toVariant()
                                       .toLongLong();
            deletedAt.close();
        }
    }
    const QMutexLocker lock(&cache.mutex);
    if (cache.generation == generation)
        cache.entries.insert(path, CachedSessionMeta{stamp, session});
    return session;
}

void pruneSessionMeta(const QString &root, const QSet<QString> &seen)
{
    const QString prefix = root + QLatin1Char('/');
    SessionMetaCache &cache = sessionMetaCache();
    const QMutexLocker lock(&cache.mutex);
    for (auto it = cache.entries.begin(); it != cache.entries.end();) {
        if (it.key().startsWith(prefix) && !seen.contains(it.key()))
            it = cache.entries.erase(it);
        else
            ++it;
    }
}

QString sanitize(const QString &owner, const QString &name)
{
    QString key = owner + QStringLiteral("-") + name;
    for (QChar &c : key)
        if (!c.isLetterOrNumber() && c != QLatin1Char('-') && c != QLatin1Char('.'))
            c = QLatin1Char('_');
    return key;
}

} // namespace

QString AgentSession::repoKey() const
{
    return sanitize(owner, name);
}

QString AgentSession::effectiveRuntimeId() const
{
    const QString explicitId = runtimeId.trimmed();
    return explicitId.isEmpty() ? agentRuntimeIdForLegacyProvider(provider)
                                : explicitId;
}

bool AgentSession::schemaSupported() const
{
    return schemaVersion >= AgentSessionSchema::MinimumSupportedVersion &&
           schemaVersion <= AgentSessionSchema::CurrentVersion;
}

bool AgentSession::canResumeWithRuntime(const AgentRuntimeDescriptor &runtime,
                                        QString *reason) const
{
    const auto reject = [reason](const QString &message) {
        if (reason)
            *reason = message;
        return false;
    };
    if (!schemaSupported()) {
        return reject(
            QStringLiteral("Session schema %1 is not supported by this ForkMesh "
                           "version (%2-%3).")
                .arg(schemaVersion)
                .arg(AgentSessionSchema::MinimumSupportedVersion)
                .arg(AgentSessionSchema::CurrentVersion));
    }
    QString descriptorError;
    if (!runtime.isValid(&descriptorError)) {
        return reject(QStringLiteral("The saved runtime is unavailable: %1")
                          .arg(descriptorError));
    }
    if (runtime.id != effectiveRuntimeId()) {
        return reject(QStringLiteral("The saved conversation belongs to runtime "
                                     "%1, not %2.")
                          .arg(effectiveRuntimeId(), runtime.id));
    }
    if (runtimeProtocolVersion < runtime.protocolMinimum ||
        runtimeProtocolVersion > runtime.protocolMaximum) {
        return reject(
            QStringLiteral("Saved runtime protocol %1 is incompatible with %2 "
                           "(supported %3-%4).")
                .arg(runtimeProtocolVersion)
                .arg(runtime.displayName)
                .arg(runtime.protocolMinimum)
                .arg(runtime.protocolMaximum));
    }
    if (reason)
        reason->clear();
    return true;
}

QJsonObject AgentSession::toJson() const
{
    QJsonObject obj;
    obj["schemaVersion"] = schemaVersion;
    obj["id"] = id;
    obj["priority"] = qBound(1, priority, 99);
    obj["owner"] = owner;
    obj["name"] = name;
    obj["issueNumber"] = issueNumber;
    obj["issueTitle"] = issueTitle;
    obj["prompt"] = prompt;
    obj["pendingSteerMessage"] = pendingSteerMessage;
    obj["inFlightSteerMessage"] = inFlightSteerMessage;
    obj["provider"] = provider;
    obj["runtimeId"] = effectiveRuntimeId();
    obj["runtimeProtocolVersion"] = runtimeProtocolVersion;
    obj["handoffFromProvider"] = handoffFromProvider;
    obj["model"] = model;
    obj["mode"] = mode;
    obj["createPr"] = createPr;
    obj["associationOnly"] = associationOnly;
    if (directCheckout)
        obj["directCheckout"] = true;
    if (held) {
        obj["held"] = true;
        if (!heldBy.isEmpty())
            obj["heldBy"] = heldBy.left(200);
    }
    obj["yolo"] = yolo;
    if (vibe)
        obj["vibe"] = true;
    obj["genie"] = genie;
    obj["strength"] = strength;
    obj["orgTask"] = orgTask;
    obj["orgTaskId"] = orgTaskId;
    if (orgTaskAttached)
        obj["orgTaskAttached"] = true;
    obj["startedByBot"] = startedByBot;
    obj["finishedByBot"] = finishedByBot;
    if (!titledBy.isEmpty())
        obj["titledBy"] = titledBy;
    obj["teammateId"] = teammateId;
    QJsonArray savedTeammates;
    for (const QString &id : teammateIds) {
        const QString clean = id.trimmed().left(80);
        if (!clean.isEmpty() && !savedTeammates.contains(clean))
            savedTeammates.append(clean);
        if (savedTeammates.size() >= 64)
            break;
    }
    obj["teammateIds"] = savedTeammates;
    obj["teammateName"] = teammateName;
    obj["teammateAvatar"] = teammateAvatar;
    obj["chatConversation"] = chatConversation.trimmed().left(160);
    obj["chatThreadRootId"] = chatThreadRootId.trimmed().left(96);
    obj["originInstanceId"] = originInstanceId.trimmed().left(96);
    obj["originInstanceUrl"] = originInstanceUrl.trimmed().left(2048);
    QJsonArray savedSkills;
    for (const QString &skill : skillsUsed) {
        const QString clean = skill.trimmed().left(160);
        bool duplicate = false;
        for (const QJsonValue &saved : std::as_const(savedSkills)) {
            if (saved.toString().compare(clean, Qt::CaseInsensitive) == 0) {
                duplicate = true;
                break;
            }
        }
        if (!clean.isEmpty() && !duplicate)
            savedSkills.append(clean);
        if (savedSkills.size() >= 64)
            break;
    }
    obj["skillsUsed"] = savedSkills;
    obj["taskGrade"] = qBound(0, taskGrade, 5);
    obj["orgAgentJob"] = orgAgentJob;
    obj["webSessionId"] = webSessionId;
    if (!claudeDesktopSessionId.isEmpty())
        obj["claudeDesktopSessionId"] = claudeDesktopSessionId;
    if (!claudeImportId.isEmpty())
        obj["claudeImport"] = QJsonObject{{"id", claudeImportId},
                                          {"path", claudeImportPath},
                                          {"cwd", claudeImportCwd},
                                          {"bytes", double(claudeImportBytes)}};
    obj["prNumber"] = prNumber;
    obj["status"] = status;
    obj["branchName"] = branchName;
    obj["worktreeName"] = worktreeName;
    obj["baseRef"] = baseRef;
    obj["baseBranch"] = baseBranch;
    obj["mergeCandidateHead"] = mergeCandidateHead;
    obj["merged"] = merged;
    obj["mergeAttested"] = mergeAttested;
    obj["mergedAtMs"] = mergedAtMs;
    obj["createdAtMs"] = createdAtMs;
    obj["startedAtMs"] = startedAtMs;
    obj["finishedAtMs"] = finishedAtMs;
    obj["promptTokens"] = promptTokens;
    obj["completionTokens"] = completionTokens;
    obj["totalTokens"] = totalTokens;
    obj["contextTokens"] = contextTokens;
    obj["contextWindow"] = contextWindow;
    obj["maxOutputTokens"] = maxOutputTokens;
    obj["estimatedCredits"] = estimatedCredits;
    obj["costUsd"] = costUsd;
    obj["spendBeforeUsd"] = spendBeforeUsd;
    obj["spendAfterUsd"] = spendAfterUsd;
    obj["numTurns"] = numTurns;
    obj["durationMs"] = durationMs;
    obj["runningMs"] = runningMs;
    obj["runningSinceMs"] = runningSinceMs;
    obj["lastError"] = lastError;
    return obj;
}

AgentSession AgentSession::fromJson(const QJsonObject &obj)
{
    AgentSession session;
    const int storedSchemaVersion =
        obj.contains("schemaVersion")
            ? obj.value("schemaVersion").toInt()
            : AgentSessionSchema::MinimumSupportedVersion;
    session.schemaVersion =
        storedSchemaVersion >= AgentSessionSchema::MinimumSupportedVersion &&
                storedSchemaVersion <= AgentSessionSchema::CurrentVersion
            ? AgentSessionSchema::CurrentVersion
            : storedSchemaVersion;
    session.id = obj.value("id").toInt();
    session.priority = qBound(1, obj.value("priority").toInt(50), 99);
    session.owner = obj.value("owner").toString();
    session.name = obj.value("name").toString();
    session.issueNumber = obj.value("issueNumber").toInt();
    session.issueTitle = obj.value("issueTitle").toString();
    session.prompt = obj.value("prompt").toString();
    session.pendingSteerMessage =
        obj.value("pendingSteerMessage").toString();
    session.inFlightSteerMessage =
        obj.value("inFlightSteerMessage").toString();
    session.provider = obj.value("provider").toString();
    session.runtimeId = obj.value("runtimeId").toString().trimmed();
    if (session.runtimeId.isEmpty())
        session.runtimeId = agentRuntimeIdForLegacyProvider(session.provider);
    session.runtimeProtocolVersion =
        qMax(1, obj.value("runtimeProtocolVersion").toInt(1));
    session.handoffFromProvider =
        obj.value("handoffFromProvider").toString();
    session.model = obj.value("model").toString();
    session.mode = obj.value("mode").toString();
    session.createPr = obj.value("createPr").toBool();
    session.associationOnly = obj.value("associationOnly").toBool();
    session.directCheckout = obj.value("directCheckout").toBool();
    session.held = obj.value("held").toBool(false);
    session.heldBy = session.held ? obj.value("heldBy").toString().left(200) : QString();
    session.yolo = obj.value("yolo").toBool();
    session.vibe = obj.value("vibe").toBool();
    session.genie = obj.value("genie").toBool();
    session.strength = obj.value("strength").toString();
    session.orgTask = obj.value("orgTask").toBool();
    session.orgTaskId = obj.value("orgTaskId").toString();
    session.orgTaskAttached = obj.value("orgTaskAttached").toBool();
    session.startedByBot = obj.value("startedByBot").toString();
    session.finishedByBot = obj.value("finishedByBot").toString();
    session.titledBy = obj.value("titledBy").toString();
    session.teammateId = obj.value("teammateId").toString();
    for (const QJsonValue &entry : obj.value("teammateIds").toArray()) {
        const QString id = entry.toString().trimmed().left(80);
        if (!id.isEmpty() && !session.teammateIds.contains(id))
            session.teammateIds.append(id);
        if (session.teammateIds.size() >= 64)
            break;
    }
    session.teammateName = obj.value("teammateName").toString();
    session.teammateAvatar = obj.value("teammateAvatar").toString();
    session.chatConversation =
        obj.value("chatConversation").toString().trimmed().left(160);
    session.chatThreadRootId =
        obj.value("chatThreadRootId").toString().trimmed().left(96);
    session.originInstanceId =
        obj.value("originInstanceId").toString().trimmed().left(96);
    session.originInstanceUrl =
        obj.value("originInstanceUrl").toString().trimmed().left(2048);
    for (const QJsonValue &entry : obj.value("skillsUsed").toArray()) {
        const QString skill = entry.toString().trimmed().left(160);
        if (!skill.isEmpty() &&
            !session.skillsUsed.contains(skill, Qt::CaseInsensitive))
            session.skillsUsed.append(skill);
        if (session.skillsUsed.size() >= 64)
            break;
    }
    session.taskGrade = qBound(0, obj.value("taskGrade").toInt(), 5);
    session.orgAgentJob = obj.value("orgAgentJob").toObject();
    session.webSessionId = obj.value("webSessionId").toString().trimmed();
    // Records written before the field existed still carry the live lease
    // while the run is in flight, so read the link off that rather than lose
    // it on the upgrade.
    if (session.webSessionId.isEmpty())
        session.webSessionId =
            session.orgAgentJob.value("sessionId").toString().trimmed();
    session.claudeDesktopSessionId =
        obj.value("claudeDesktopSessionId").toString().trimmed();
    const QJsonObject claudeImport = obj.value("claudeImport").toObject();
    session.claudeImportId = claudeImport.value("id").toString();
    session.claudeImportPath = claudeImport.value("path").toString();
    session.claudeImportCwd = claudeImport.value("cwd").toString();
    session.claudeImportBytes = qint64(claudeImport.value("bytes").toDouble());
    session.prNumber = obj.value("prNumber").toInt();
    session.status = obj.value("status").toString(AgentStatus::Queued);
    session.branchName = obj.value("branchName").toString();
    session.worktreeName = obj.value("worktreeName").toString();
    session.baseRef = obj.value("baseRef").toString();
    session.baseBranch = obj.value("baseBranch").toString();
    session.mergeCandidateHead = obj.value("mergeCandidateHead").toString();
    session.merged = obj.value("merged").toBool();
    session.mergeAttested = obj.value("mergeAttested").toBool();
    session.mergedAtMs = obj.value("mergedAtMs").toVariant().toLongLong();
    session.createdAtMs = obj.value("createdAtMs").toVariant().toLongLong();
    session.startedAtMs = obj.value("startedAtMs").toVariant().toLongLong();
    session.finishedAtMs = obj.value("finishedAtMs").toVariant().toLongLong();
    session.promptTokens = obj.value("promptTokens").toInt();
    session.completionTokens = obj.value("completionTokens").toInt();
    session.totalTokens = obj.value("totalTokens").toInt();
    session.contextTokens = obj.value("contextTokens").toInt();
    session.contextWindow = obj.value("contextWindow").toInt();
    session.maxOutputTokens = obj.value("maxOutputTokens").toInt();
    session.estimatedCredits = obj.value("estimatedCredits").toInt();
    session.costUsd = obj.value("costUsd").toDouble();
    session.spendBeforeUsd = obj.value("spendBeforeUsd").toDouble();
    session.spendAfterUsd = obj.value("spendAfterUsd").toDouble();
    session.numTurns = obj.value("numTurns").toInt();
    session.durationMs = obj.value("durationMs").toVariant().toLongLong();
    session.runningMs = obj.value("runningMs").toVariant().toLongLong();
    session.runningSinceMs =
        obj.value("runningSinceMs").toVariant().toLongLong();
    session.lastError = obj.value("lastError").toString();
    return session;
}

AgentStore::AgentStore(QString rootDir) : m_root(std::move(rootDir))
{
    QDir().mkpath(sessionsDir());
}

QString AgentStore::sessionsDir() const
{
    return m_root + QStringLiteral("/sessions");
}

QString AgentStore::sessionDir(const AgentSession &session) const
{
    return sessionsDir() + QLatin1Char('/') + session.repoKey() +
           QLatin1Char('/') + QString::number(session.id);
}

QString AgentStore::recycleBinDir() const
{
    return m_root + QStringLiteral("/recycle-bin");
}

QString AgentStore::recycledSessionDir(const AgentSession &session) const
{
    return recycleBinDir() + QLatin1Char('/') + session.repoKey() +
           QLatin1Char('/') + QString::number(session.id);
}

// The bin is keyed by repository like the live tree, but a restore or a purge
// only knows the session id, so the repository folder has to be searched for.
QString AgentStore::findRecycledSessionDir(int sessionId) const
{
    if (sessionId <= 0)
        return QString();
    const QString wanted = QString::number(sessionId);
    QDir root(recycleBinDir());
    const QStringList repos = root.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &repo : repos) {
        QDir repoDir(root.filePath(repo));
        if (repoDir.exists(wanted))
            return repoDir.filePath(wanted);
    }
    return QString();
}

// code-health: allow(deep-nested-loop, dir-walk-in-loop) outer loop is the 2 trees; name-only readdirs, no file opens, once per createSession
int AgentStore::nextId() const
{
    int maxId = 0;
    // Binned ids count too: handing a deleted agent's id to a new session
    // would block restoring it, and deleting that new session would overwrite
    // its bin entry (issue #578).
    for (const QString &tree : {sessionsDir(), recycleBinDir()}) {
        QDir root(tree);
        const QStringList repos =
            root.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QString &repo : repos) {
            const QStringList ids =
                QDir(root.filePath(repo))
                    .entryList(QDir::Dirs | QDir::NoDotAndDotDot);
            for (const QString &id : ids)
                maxId = qMax(maxId, id.toInt());
        }
    }
    return maxId + 1;
}

AgentSession AgentStore::createSession(AgentSession session)
{
    session.id = nextId();
    session.createdAtMs = QDateTime::currentMSecsSinceEpoch();
    reviveSession(sessionDir(session)); // this id is live again
    QDir().mkpath(sessionDir(session));
    saveSession(session);
    return session;
}

bool AgentStore::saveSession(const AgentSession &session) const
{
    // Never rewrite a record produced by a newer client: doing so would drop
    // fields this build does not understand. The transcript and artifacts stay
    // available for export or a newer ForkMesh build.
    if (!session.schemaSupported())
        return false;
    // A cloud row is the relay's record of a run on another node. Whatever
    // path reached here with one, nothing of it belongs on this disk.
    if (session.cloud)
        return false;
    if (session.id <= 0)
        return false;
    if (sessionRetired(sessionDir(session)))
        return false;
    const QString path = sessionDir(session) + QStringLiteral("/meta.json");
    const QByteArray bytes =
        QJsonDocument(session.toJson()).toJson(QJsonDocument::Indented);
    WrittenSessionMetas &written = writtenSessionMetas();
    {
        const QMutexLocker lock(&written.mutex);
        const auto last = written.entries.constFind(path);
        if (last != written.entries.constEnd() && last->bytes == bytes &&
            last->stamp == sessionFileStamp(path))
            return true;
    }
    QDir().mkpath(sessionDir(session));
    const bool committed = replaceSessionMeta(path, bytes);
    forgetSessionMeta(path);
    {
        const QMutexLocker lock(&written.mutex);
        if (committed)
            written.entries.insert(path,
                                   WrittenSessionMeta{bytes, sessionFileStamp(path)});
        else
            written.entries.remove(path);
    }
    return committed;
}

bool AgentStore::deleteSession(const AgentSession &session) const
{
    const QString path = sessionDir(session);
    transcriptEventWriter().flushPath(path + QStringLiteral("/events.jsonl"));
    transcriptLogWriter().flushPath(path + QStringLiteral("/transcript.txt"));
    // Tombstone before the removal, not after: writes are what races us here, and
    // a save that slips in between would otherwise leave a directory nothing
    // deletes again.
    retireSession(path);
    QDir dir(path);
    if (!dir.exists())
        return true;
    if (!forkmesh::safedelete::removeTree(path, sessionsDir())) {
        reviveSession(path); // the record survives, so it must stay writable
        return false;
    }
    recordRetiredSession(session);
    return true;
}

// Same contract as deleteSession() from the caller's side: the live session is
// gone and its row can leave the UI. The directory is moved rather than removed
// so a mistaken delete stays recoverable (issue #578).
bool AgentStore::recycleSession(const AgentSession &session) const
{
    const QString path = sessionDir(session);
    transcriptEventWriter().flushPath(path + QStringLiteral("/events.jsonl"));
    transcriptLogWriter().flushPath(path + QStringLiteral("/transcript.txt"));
    // Tombstone before the move for the reason deleteSession() gives: a
    // transcript append that slips in afterwards would recreate the directory
    // that was just moved away.
    retireSession(path);
    QDir dir(path);
    if (!dir.exists())
        return true;

    const QString target = recycledSessionDir(session);
    if (!QDir().mkpath(QFileInfo(target).absolutePath())) {
        reviveSession(path);
        return false;
    }
    // An id is only reused once its bin entry is gone, but a half-finished
    // earlier move would otherwise block this rename permanently.
    if (QDir(target).exists())
        forkmesh::safedelete::removeTree(target, recycleBinDir());
    if (!QDir().rename(path, target)) {
        reviveSession(path);
        return false;
    }

    QFile stamp(target + QStringLiteral("/deleted-at.json"));
    if (stamp.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QJsonObject meta;
        meta.insert(QStringLiteral("deletedAtMs"),
                    QDateTime::currentMSecsSinceEpoch());
        stamp.write(QJsonDocument(meta).toJson(QJsonDocument::Compact));
        stamp.close();
    }
    recordRetiredSession(session);
    return true;
}

QList<AgentSession> AgentStore::loadRecycledSessions() const
{
    QList<AgentSession> sessions;
    QSet<QString> seen;
    QDir root(recycleBinDir());
    const QStringList repos = root.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &repo : repos) {
        QDir repoDir(root.filePath(repo));
        const QStringList ids =
            repoDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QString &id : ids) {
            if (std::optional<AgentSession> session =
                    loadSessionMeta(repoDir.filePath(id), true, &seen))
                sessions.append(*session);
        }
    }
    pruneSessionMeta(recycleBinDir(), seen);
    // Most recently deleted first: that is what someone opening the bin is
    // looking for.
    std::sort(sessions.begin(), sessions.end(),
              [](const AgentSession &a, const AgentSession &b) {
                  if (a.deletedAtMs != b.deletedAtMs)
                      return a.deletedAtMs > b.deletedAtMs;
                  return a.id > b.id;
              });
    return sessions;
}

bool AgentStore::restoreRecycledSession(int sessionId) const
{
    const QString binDir = findRecycledSessionDir(sessionId);
    if (binDir.isEmpty())
        return false;
    QFile file(binDir + QStringLiteral("/meta.json"));
    if (!file.open(QIODevice::ReadOnly))
        return false;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    file.close();
    if (!doc.isObject())
        return false;
    AgentSession session = AgentSession::fromJson(doc.object());
    const QString target = sessionDir(session);
    if (QDir(target).exists())
        return false; // a live session already owns this id
    if (!QDir().mkpath(QFileInfo(target).absolutePath()))
        return false;
    if (!QDir().rename(binDir, target))
        return false;
    QFile::remove(target + QStringLiteral("/deleted-at.json"));
    // The directory is live again, so transcript writes must be accepted.
    reviveSession(target);
    // Deleting stopped whatever was running, and the tombstone swallowed the
    // runner's final status write, so a record binned mid-run still says so.
    // Nothing drives it after a restore; show it as stopped rather than as a
    // run that never ends.
    if (session.status == AgentStatus::Queued ||
        session.status == AgentStatus::Running ||
        session.status == AgentStatus::Waiting) {
        session.status = AgentStatus::Stopped;
        saveSession(session);
    }
    return true;
}

bool AgentStore::purgeRecycledSession(int sessionId) const
{
    const QString binDir = findRecycledSessionDir(sessionId);
    if (binDir.isEmpty())
        return false;
    return forkmesh::safedelete::removeTree(binDir, recycleBinDir());
}

int AgentStore::purgeExpiredRecycledSessions(qint64 retentionMs,
                                             qint64 nowMs) const
{
    if (retentionMs <= 0)
        return 0;
    int purged = 0;
    QDir root(recycleBinDir());
    const QStringList repos = root.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &repo : repos) {
        QDir repoDir(root.filePath(repo));
        const QStringList ids =
            repoDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QString &id : ids) {
            const QString dir = repoDir.filePath(id);
            qint64 deletedAtMs = 0;
            QFile stamp(dir + QStringLiteral("/deleted-at.json"));
            if (stamp.open(QIODevice::ReadOnly)) {
                deletedAtMs = QJsonDocument::fromJson(stamp.readAll())
                                  .object()
                                  .value(QStringLiteral("deletedAtMs"))
                                  .toVariant()
                                  .toLongLong();
                stamp.close();
            }
            // A bin with no readable stamp cannot be aged, and skipping it
            // would keep it forever. Fall back to the directory's own
            // timestamp so the sweep still terminates.
            if (deletedAtMs <= 0)
                deletedAtMs = QFileInfo(dir).lastModified().toMSecsSinceEpoch();
            if (nowMs - deletedAtMs < retentionMs)
                continue;
            if (forkmesh::safedelete::removeTree(dir, recycleBinDir()))
                purged += 1;
        }
        // Drop the repository folder once its last bin is gone.
        if (repoDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot).isEmpty())
            forkmesh::safedelete::removeTree(repoDir.absolutePath(), recycleBinDir());
    }
    return purged;
}

QString AgentStore::modelOutcomeKey(const QString &provider, const QString &model)
{
    const QString id = model.trimmed().toLower();
    if (id.isEmpty())
        return QString();
    return provider + QLatin1Char('\x1f') + id;
}

QString AgentStore::modelOutcomesPath() const
{
    return m_root + QStringLiteral("/model-outcomes.json");
}

QHash<QString, AgentModelOutcome> AgentStore::retiredModelOutcomes() const
{
    QHash<QString, AgentModelOutcome> outcomes;
    QFile file(modelOutcomesPath());
    if (!file.open(QIODevice::ReadOnly))
        return outcomes;
    const QJsonArray models =
        QJsonDocument::fromJson(file.readAll()).object().value("models").toArray();
    for (const QJsonValue &value : models) {
        const QJsonObject entry = value.toObject();
        const QString key = modelOutcomeKey(entry.value("provider").toString(),
                                            entry.value("model").toString());
        if (key.isEmpty())
            continue;
        AgentModelOutcome &outcome = outcomes[key];
        outcome.runs += qMax(0, entry.value("runs").toInt());
        outcome.merged += qMax(0, entry.value("merged").toInt());
    }
    return outcomes;
}

void AgentStore::recordRetiredSession(const AgentSession &session) const
{
    const QString key = modelOutcomeKey(session.provider, session.model);
    if (key.isEmpty())
        return;
    QJsonArray models;
    QFile existing(modelOutcomesPath());
    if (existing.open(QIODevice::ReadOnly)) {
        models = QJsonDocument::fromJson(existing.readAll())
                     .object()
                     .value("models")
                     .toArray();
        existing.close();
    }
    bool credited = false;
    for (int i = 0; i < models.size() && !credited; ++i) {
        QJsonObject entry = models.at(i).toObject();
        if (modelOutcomeKey(entry.value("provider").toString(),
                            entry.value("model").toString()) != key)
            continue;
        entry["runs"] = qMax(0, entry.value("runs").toInt()) + 1;
        entry["merged"] =
            qMax(0, entry.value("merged").toInt()) + (session.merged ? 1 : 0);
        models.replace(i, entry);
        credited = true;
    }
    if (!credited) {
        models.append(QJsonObject{
            {"provider", session.provider},
            {"model", session.model.trimmed().toLower()},
            {"runs", 1},
            {"merged", session.merged ? 1 : 0},
        });
    }
    QDir().mkpath(m_root);
    QSaveFile file(modelOutcomesPath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return;
    file.write(QJsonDocument(QJsonObject{{"version", 1}, {"models", models}})
                   .toJson(QJsonDocument::Indented));
    file.commit();
}

void AgentStore::appendLog(const AgentSession &session, const QString &text) const
{
    const QString dir = sessionDir(session);
    if (sessionRetired(dir))
        return;
    transcriptLogWriter().appendLog(dir + QStringLiteral("/transcript.txt"), text);
    notifyLogObserver(session.id);
}

QString AgentStore::logPath(const AgentSession &session) const
{
    return sessionDir(session) + QStringLiteral("/transcript.txt");
}

void AgentStore::flushLog(const AgentSession &session) const
{
    transcriptLogWriter().flushPath(logPath(session));
}

void AgentStore::setLogObserver(QObject *context, std::function<void(int sessionId)> observer)
{
    QMutexLocker locker(&logObserverMutex());
    logObserverContext() = context;
    logObserver() = std::move(observer);
    g_logObserved.store(context && logObserver(), std::memory_order_relaxed);
}

QString AgentStore::readLog(const AgentSession &session) const
{
    const QString path =
        sessionDir(session) + QStringLiteral("/transcript.txt");
    transcriptLogWriter().flushPath(path);
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return QString();
    return QString::fromUtf8(file.readAll());
}

void AgentStore::appendEvent(const AgentSession &session, const QJsonObject &ev) const
{
    const QString dir = sessionDir(session);
    if (sessionRetired(dir))
        return;
    transcriptEventWriter().appendEvent(dir + QStringLiteral("/events.jsonl"), ev);
}

QList<QJsonObject> AgentStore::loadEvents(const AgentSession &session) const
{
    const QString path = sessionDir(session) + QStringLiteral("/events.jsonl");
    transcriptEventWriter().flushPath(path);
    QList<QJsonObject> events;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return events;
    while (!file.atEnd()) {
        const QByteArray line = file.readLine().trimmed();
        if (line.isEmpty())
            continue;
        const QJsonDocument doc = QJsonDocument::fromJson(line);
        if (doc.isObject())
            events.append(doc.object());
    }
    return events;
}

QList<QJsonObject> AgentStore::loadRecentEvents(const AgentSession &session,
                                                qint64 tailBytes) const
{
    const QString path = sessionDir(session) + QStringLiteral("/events.jsonl");
    transcriptEventWriter().flushPath(path);
    QList<QJsonObject> events;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return events;
    const qint64 size = file.size();
    if (tailBytes > 0 && size > tailBytes) {
        file.seek(size - tailBytes - 1);
        file.readLine();
    }
    while (!file.atEnd()) {
        const QByteArray line = file.readLine().trimmed();
        if (line.isEmpty())
            continue;
        const QJsonDocument doc = QJsonDocument::fromJson(line);
        if (doc.isObject())
            events.append(doc.object());
    }
    return events;
}

void AgentStore::clearEvents(const AgentSession &session) const
{
    transcriptEventWriter().removeFile(
        sessionDir(session) + QStringLiteral("/events.jsonl"));
}

void AgentStore::whenTranscriptWritesDrained(
    QObject *context, std::function<void()> callback) const
{
    if (!context || !callback)
        return;
    struct DrainState {
        int remaining = 2;
        std::function<void()> callback;
    };
    auto state = std::make_shared<DrainState>();
    state->callback = std::move(callback);
    const auto arrived = [state] {
        if (--state->remaining == 0)
            state->callback();
    };
    transcriptEventWriter().whenDrained(context, arrived);
    transcriptLogWriter().whenDrained(context, arrived);
}

void AgentStore::flushTranscriptWrites()
{
    transcriptEventWriter().flush();
    transcriptLogWriter().flush();
}

void AgentStore::writePatch(const AgentSession &session, const QString &patch) const
{
    if (sessionRetired(sessionDir(session)))
        return;
    QDir().mkpath(sessionDir(session));
    QFile file(sessionDir(session) + QStringLiteral("/changes.patch"));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
        return;
    file.write(patch.toUtf8());
}

QString AgentStore::readPatch(const AgentSession &session) const
{
    QFile file(sessionDir(session) + QStringLiteral("/changes.patch"));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return QString();
    return QString::fromUtf8(file.readAll());
}

namespace {

QString readTranscriptTail(const QString &path, qint64 tailBytes)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return QString();
    const qint64 size = file.size();
    if (size > tailBytes) {
        file.seek(size - tailBytes);
        file.readLine();
    }
    return QString::fromUtf8(file.readAll());
}

QString readTranscriptEnds(const QString &path, qint64 endBytes)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return QString();
    const qint64 size = file.size();
    if (size <= 2 * endBytes)
        return QString::fromUtf8(file.readAll());
    QByteArray head = file.read(endBytes);
    const qsizetype lastNewline = head.lastIndexOf('\n');
    head.truncate(lastNewline < 0 ? 0 : lastNewline + 1);
    file.seek(size - endBytes);
    file.readLine(); // …and the leading partial line of the tail
    return QString::fromUtf8(head) + QString::fromUtf8(file.readAll());
}

QString fileStamp(const QString &path)
{
    const QFileInfo info(path);
    if (!info.exists())
        return QStringLiteral("-");
    return QStringLiteral("%1:%2")
        .arg(info.size())
        .arg(info.lastModified().toMSecsSinceEpoch());
}

QString hitSnippet(const QString &text, int at, int length)
{
    constexpr int kContext = 60;
    const qsizetype from = std::max<qsizetype>(0, at - kContext);
    const qsizetype to =
        std::min<qsizetype>(text.size(), at + length + kContext);
    QString snippet = text.mid(from, to - from).simplified();
    if (from > 0)
        snippet.prepend(QString::fromUtf8("\xE2\x80\xA6"));
    if (to < text.size())
        snippet.append(QString::fromUtf8("\xE2\x80\xA6"));
    return snippet;
}

} // namespace

int AgentStore::searchTranscript(const AgentSession &session,
                                 const QString &needle, QString *snippet) const
{
    transcriptEventWriter().flush();
    transcriptLogWriter().flush();
    const QString query = needle.trimmed();
    if (query.isEmpty())
        return 0;
    const QString dir = sessionDir(session);
    const QStringList files{QStringLiteral("/transcript.txt"),
                            QStringLiteral("/events.jsonl")};
    int hits = 0;
    for (const QString &name : files) {
        const QString text =
            readTranscriptTail(dir + name, kTranscriptSearchTailBytes);
        int at = text.indexOf(query, 0, Qt::CaseInsensitive);
        while (at >= 0) {
            if (snippet && snippet->isEmpty())
                *snippet = hitSnippet(text, at, query.size());
            ++hits;
            at = text.indexOf(query, at + query.size(), Qt::CaseInsensitive);
        }
    }
    return hits;
}

QStringList AgentStore::attachmentPathsIn(const QString &text)
{
    // The common case is neither: no regex pass over a whole transcript.
    if (!text.contains(QLatin1String("Attached image:")) &&
        !text.contains(QLatin1String("Attached video:")))
        return {};
    static const QRegularExpression marker(
        QStringLiteral("Attached (?:image|video):[ \\t]*([^\"\\\\\\r\\n]+)"));
    QStringList paths;
    QRegularExpressionMatchIterator it = marker.globalMatch(text);
    while (it.hasNext()) {
        const QString path = it.next().captured(1).trimmed();
        if (!path.isEmpty() && !paths.contains(path))
            paths.append(path);
    }
    return paths;
}

QStringList AgentStore::attachmentPaths(const AgentSession &session) const
{
    const QString dir = sessionDir(session);
    transcriptEventWriter().flushPath(
        dir + QStringLiteral("/events.jsonl"));
    transcriptLogWriter().flushPath(
        dir + QStringLiteral("/transcript.txt"));
    QStringList paths = attachmentPathsIn(session.prompt);
    for (const QString &name : {QStringLiteral("/transcript.txt"),
                                QStringLiteral("/events.jsonl")}) {
        const QStringList found = attachmentPathsIn(
            readTranscriptEnds(dir + name, kAttachmentScanBytes));
        for (const QString &path : found)
            if (!paths.contains(path))
                paths.append(path);
    }
    return paths;
}

QString AgentStore::transcriptStamp(const AgentSession &session) const
{
    const QString dir = sessionDir(session);
    transcriptEventWriter().flushPath(
        dir + QStringLiteral("/events.jsonl"));
    transcriptLogWriter().flushPath(
        dir + QStringLiteral("/transcript.txt"));
    return fileStamp(dir + QStringLiteral("/transcript.txt")) +
           QLatin1Char('|') + fileStamp(dir + QStringLiteral("/events.jsonl"));
}

QList<AgentSession> AgentStore::loadAllSessions() const
{
    QList<AgentSession> sessions;
    QSet<QString> seen;
    QDir root(sessionsDir());
    const QStringList repos =
        root.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &repo : repos) {
        QDir repoDir(root.filePath(repo));
        const QStringList ids =
            repoDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QString &id : ids) {
            if (std::optional<AgentSession> session =
                    loadSessionMeta(repoDir.filePath(id), false, &seen);
                session && session->id > 0)
                sessions.append(*session);
        }
    }
    pruneSessionMeta(sessionsDir(), seen);
    const auto updatedAtMs = [](const AgentSession &session) {
        return qMax(qMax(session.createdAtMs, session.startedAtMs),
                    qMax(session.finishedAtMs, session.mergedAtMs));
    };
    std::sort(sessions.begin(), sessions.end(),
              [updatedAtMs](const AgentSession &a, const AgentSession &b) {
                  const qint64 aUpdated = updatedAtMs(a);
                  const qint64 bUpdated = updatedAtMs(b);
                  if (aUpdated != bUpdated)
                      return aUpdated > bUpdated;
                  return a.id > b.id;
              });
    return sessions;
}

QList<int> AgentStore::queuedSessionIdsOldestFirst(
    const QList<AgentSession> &sessions)
{
    QList<const AgentSession *> queued;
    for (const AgentSession &session : sessions) {
        if (session.status == AgentStatus::Queued && !session.held)
            queued.append(&session);
    }
    std::sort(queued.begin(), queued.end(),
              [](const AgentSession *a, const AgentSession *b) {
                  if (a->priority != b->priority)
                      return a->priority < b->priority;
                  if (a->createdAtMs != b->createdAtMs)
                      return a->createdAtMs < b->createdAtMs;
                  return a->id < b->id;
              });
    QList<int> ids;
    ids.reserve(queued.size());
    for (const AgentSession *session : std::as_const(queued))
        ids.append(session->id);
    return ids;
}
