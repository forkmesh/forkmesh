// Deleting an agent moves it to a recycle bin instead of removing it, and the
// bin is swept once the retention window passes (issue #578).
#include "AgentStore.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <iostream>

static int failures = 0;

static void check(bool condition, const char *what)
{
    if (condition)
        return;
    std::cerr << "FAIL: " << what << "\n";
    ++failures;
}

static AgentSession makeSession(AgentStore &store, const QString &owner,
                                const QString &name)
{
    AgentSession session;
    session.owner = owner;
    session.name = name;
    session.prompt = QStringLiteral("do the thing");
    session.provider = QStringLiteral("claude-code");
    session.status = AgentStatus::Success;
    session.createdAtMs = QDateTime::currentMSecsSinceEpoch();
    return store.createSession(session);
}

// Backdates the bin's stamp so retention can be exercised without waiting.
static void backdate(const QString &root, int sessionId, qint64 deletedAtMs)
{
    QDir bin(root + QStringLiteral("/recycle-bin"));
    const QStringList repos = bin.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &repo : repos) {
        QDir repoDir(bin.filePath(repo));
        const QString id = QString::number(sessionId);
        if (!repoDir.exists(id))
            continue;
        QFile stamp(repoDir.filePath(id) + QStringLiteral("/deleted-at.json"));
        if (!stamp.open(QIODevice::WriteOnly | QIODevice::Truncate))
            return;
        QJsonObject meta;
        meta.insert(QStringLiteral("deletedAtMs"), deletedAtMs);
        stamp.write(QJsonDocument(meta).toJson(QJsonDocument::Compact));
        stamp.close();
        return;
    }
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir tmp;
    check(tmp.isValid(), "temporary root");
    const QString root = tmp.path();
    AgentStore store(root);

    // A deleted agent leaves the live list but survives in the bin.
    const AgentSession session = makeSession(store, "acme", "widget");
    check(session.id > 0, "session created");
    store.appendLog(session, QStringLiteral("hello"));
    check(store.loadAllSessions().size() == 1, "one live session");

    check(store.recycleSession(session), "recycleSession succeeds");
    check(store.loadAllSessions().isEmpty(), "live list is empty after delete");

    QList<AgentSession> binned = store.loadRecycledSessions();
    check(binned.size() == 1, "one session in the bin");
    check(binned.value(0).id == session.id, "bin keeps the session id");
    check(binned.value(0).prompt == session.prompt, "bin keeps the record");
    check(binned.value(0).deletedAtMs > 0, "bin stamps a deletion time");

    // Restoring puts it back where it was, transcript and all.
    check(store.restoreRecycledSession(session.id), "restore succeeds");
    check(store.loadAllSessions().size() == 1, "session is live again");
    check(store.loadRecycledSessions().isEmpty(), "bin is empty after restore");
    check(store.readLog(session).contains(QStringLiteral("hello")),
          "restored session keeps its transcript");

    // Retention: a fresh bin entry is kept, an expired one is swept.
    check(store.recycleSession(session), "recycle again");
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 dayMs = qint64(24) * 60 * 60 * 1000;
    check(store.purgeExpiredRecycledSessions(dayMs, now) == 0,
          "a fresh bin entry is not swept");
    check(store.loadRecycledSessions().size() == 1, "fresh entry still there");

    backdate(root, session.id, now - dayMs - 1000);
    check(store.purgeExpiredRecycledSessions(dayMs, now) == 1,
          "an expired bin entry is swept");
    check(store.loadRecycledSessions().isEmpty(), "bin is empty after sweep");
    check(store.loadAllSessions().isEmpty(), "sweeping does not revive it");

    // A zero or negative window must not be read as "delete everything".
    const AgentSession second = makeSession(store, "acme", "widget");
    check(store.recycleSession(second), "recycle second session");
    check(store.purgeExpiredRecycledSessions(0, now) == 0,
          "a zero retention window sweeps nothing");
    check(store.loadRecycledSessions().size() == 1, "second entry survives");

    // A bin whose stamp is missing must still age out rather than persist
    // forever.
    QDir bin(root + QStringLiteral("/recycle-bin"));
    const QStringList repos = bin.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &repo : repos) {
        QDir repoDir(bin.filePath(repo));
        for (const QString &id :
             repoDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            QFile::remove(repoDir.filePath(id) +
                          QStringLiteral("/deleted-at.json"));
        }
    }
    check(store.purgeExpiredRecycledSessions(1, now + dayMs) == 1,
          "a stamp-less bin entry still ages out");

    // Restoring something that was never binned is a no-op, not a crash.
    check(!store.restoreRecycledSession(4242), "restoring an unknown id fails");
    check(!store.purgeRecycledSession(4242), "purging an unknown id fails");

    // A new agent must not take a binned agent's id: that would block the
    // restore, and deleting the newcomer would overwrite the bin entry.
    const AgentSession top = makeSession(store, "acme", "widget");
    check(store.recycleSession(top), "recycle the newest session");
    const AgentSession next = makeSession(store, "acme", "widget");
    check(next.id > top.id, "a new session skips the binned id");
    check(store.recycleSession(next), "recycle the newcomer");
    check(store.loadRecycledSessions().size() == 2,
          "both deleted sessions stay in the bin");
    check(store.restoreRecycledSession(top.id), "the older one still restores");

    // Restoring over a live session with the same id refuses rather than
    // clobbering it. Reproduce the collision by hand: bin entries from before
    // ids skipped the bin could already share one with a live session.
    QDir binRepo(root + QStringLiteral("/recycle-bin/") + top.repoKey());
    check(QDir().mkpath(binRepo.filePath(QString::number(top.id))),
          "fabricate a colliding bin entry");
    QFile::copy(root + QStringLiteral("/sessions/") + top.repoKey() +
                    QLatin1Char('/') + QString::number(top.id) +
                    QStringLiteral("/meta.json"),
                binRepo.filePath(QString::number(top.id)) +
                    QStringLiteral("/meta.json"));
    check(!store.restoreRecycledSession(top.id),
          "restoring over a live id is refused");
    check(store.purgeRecycledSession(top.id), "purge the colliding entry");
    check(store.loadAllSessions().size() == 1, "the live session is untouched");

    // A session deleted mid-run comes back stopped, not running forever.
    AgentSession running = makeSession(store, "acme", "gadget");
    running.status = AgentStatus::Running;
    check(store.saveSession(running), "mark the session running");
    check(store.recycleSession(running), "recycle the running session");
    check(store.loadRecycledSessions().value(0).status == AgentStatus::Running,
          "the bin keeps the record as deleted");
    check(store.restoreRecycledSession(running.id), "restore the running one");
    bool sawStopped = false;
    for (const AgentSession &live : store.loadAllSessions()) {
        if (live.id == running.id)
            sawStopped = live.status == AgentStatus::Stopped;
    }
    check(sawStopped, "a restored mid-run session is marked stopped");

    // The default retention is the 24 hours the request asked for.
    check(AgentStore::kDefaultRecycleRetentionHours == 24,
          "default retention is 24 hours");
    check(AgentStore::kMinRecycleRetentionHours >= 1 &&
              AgentStore::kMinRecycleRetentionHours <=
                  AgentStore::kDefaultRecycleRetentionHours &&
              AgentStore::kDefaultRecycleRetentionHours <=
                  AgentStore::kMaxRecycleRetentionHours,
          "the default sits inside the Settings range");

    if (failures == 0)
        std::cout << "agent recycle bin: all checks passed\n";
    return failures == 0 ? 0 : 1;
}
