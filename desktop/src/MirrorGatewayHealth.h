#pragma once

#include "NetworkBackoff.h"

#include <QHash>
#include <QLatin1String>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QtGlobal>

#include <utility>

// Only transport failures are unreachable; authenticated ref rejects are not.
inline bool mirrorGatewayUnreachable(const QString &pushStderr)
{
    static const QLatin1String kPhrases[] = {
        QLatin1String("connection timed out"),
        QLatin1String("operation timed out"),
        QLatin1String("connection refused"),
        QLatin1String("no route to host"),
        QLatin1String("network is unreachable"),
        QLatin1String("could not resolve hostname"),
        QLatin1String("name or service not known"),
        QLatin1String("nodename nor servname provided"),
        QLatin1String("temporary failure in name resolution"),
        QLatin1String("connection closed by remote host"),
        QLatin1String("connection reset by peer"),
    };
    const QString text = pushStderr.toLower();
    for (const QLatin1String &phrase : kPhrases) {
        if (text.contains(phrase))
            return true;
    }
    return false;
}

// Does this ssh:// host belong to the mesh node called `node`? A node saved as
// "mirror10" is reached through whatever the remote was configured with: the
// bare name, the "forkmesh-mirror10-sync" ssh_config alias the provisioner
// writes, or a DNS name like "mirror10.forkmesh.com". So compare on whole
// dash/dot separated segments and require the node's segments to appear as an
// unbroken run — "mirror1" then matches none of mirror10's spellings, which is
// the one mistake this must never make.
inline bool mirrorGatewayHostBelongsToNode(const QString &host,
                                           const QString &node)
{
    const auto segments = [](const QString &value) {
        QStringList parts;
        QString current;
        for (const QChar &ch : value.trimmed().toLower()) {
            if (ch == QLatin1Char('-') || ch == QLatin1Char('.')) {
                if (!current.isEmpty())
                    parts.append(current);
                current.clear();
                continue;
            }
            current.append(ch);
        }
        if (!current.isEmpty())
            parts.append(current);
        return parts;
    };
    const QStringList wanted = segments(node);
    const QStringList have = segments(host);
    if (wanted.isEmpty() || have.isEmpty() || wanted.size() > have.size())
        return false;
    for (int start = 0; start + wanted.size() <= have.size(); ++start) {
        if (have.mid(start, wanted.size()) == wanted)
            return true;
    }
    return false;
}

// Direct managed-host ref pushes are triggered by commits, mirror refreshes,
// and the periodic safety pass. Those events can arrive while the previous
// asynchronous Git process is still running. Starting another writer for the
// same repository/host races Git's ref compare-and-swap and produces a burst
// of "cannot lock ref ... is at X but expected Y" failures even though a later
// retry converges. Reserve each destination once and remember that the newest
// repository state needs one replay after the whole in-flight batch drains.
class MirrorRefPushGate {
public:
    struct Reservation {
        QSet<QString> readyHosts;
        bool replayQueued = false;
    };

    Reservation reserve(const QString &repository,
                        const QStringList &hosts)
    {
        const QString repo = normalized(repository);
        Reservation result;
        if (repo.isEmpty())
            return result;
        QSet<QString> seen;
        for (const QString &rawHost : hosts) {
            const QString host = normalized(rawHost);
            if (host.isEmpty() || seen.contains(host))
                continue;
            seen.insert(host);
            const QString key = pushKey(repo, host);
            if (m_inFlight.contains(key)) {
                result.replayQueued = true;
                continue;
            }
            m_inFlight.insert(key);
            result.readyHosts.insert(host);
        }
        if (result.replayQueued)
            m_replayPending.insert(repo);
        return result;
    }

    // Returns true exactly once when a queued replay may start. Waiting for
    // every host in this repository to drain prevents the replay itself from
    // becoming another partial overlapping batch.
    bool finish(const QString &repository, const QString &host)
    {
        const QString repo = normalized(repository);
        if (repo.isEmpty() ||
            !m_inFlight.remove(pushKey(repo, normalized(host)))) {
            return false;
        }
        const QString prefix = repo + QLatin1Char('\x1f');
        for (const QString &key : std::as_const(m_inFlight)) {
            if (key.startsWith(prefix))
                return false;
        }
        return m_replayPending.remove(repo) > 0;
    }

    bool busy(const QString &repository, const QStringList &hosts) const
    {
        const QString repo = normalized(repository);
        for (const QString &rawHost : hosts) {
            if (m_inFlight.contains(pushKey(repo, normalized(rawHost))))
                return true;
        }
        return false;
    }

private:
    static QString normalized(const QString &value)
    {
        return value.trimmed().toLower();
    }

    static QString pushKey(const QString &repository, const QString &host)
    {
        return repository + QLatin1Char('\x1f') + host;
    }

    QSet<QString> m_inFlight;
    QSet<QString> m_replayPending;
};

class MirrorGatewayHealth {
public:
    static constexpr qint64 kBaseCooldownMs = 10LL * 1000;
    static constexpr qint64 kMaxCooldownMs = 5LL * 60 * 1000;
    static constexpr qint64 kRepeatNoticeMs = 60LL * 60 * 1000;    // hourly
    static constexpr qint64 kRetiredAfterMs = 24LL * 60 * 60 * 1000;
    // A gateway that has not answered for a whole day is a destroyed node
    // until proven otherwise. Keep probing it — a node can come back — but at
    // a cadence that costs four connect timeouts a day instead of ninety-six,
    // and stop repeating the log line: the one retirement notice said it all.
    static constexpr qint64 kRetiredCooldownMs = 6LL * 60 * 60 * 1000;

    struct Notice {
        qint64 cooldownMs = 0;
        qint64 downForMs = 0;
        bool announce = false;
        bool retired = false;
        // The streak crossed kRetiredAfterMs: this gateway is now parked on
        // the slow probe, whether or not this pass is the one that says so.
        bool parked = false;
        // Something worth writing back to disk changed. The streak has to
        // outlive the process or a desktop that restarts daily never reaches
        // the retirement threshold, re-announces the same dead gateway on
        // every launch, and pays the full retry cadence forever.
        bool persist = false;
    };

    bool readyToPush(const QString &host, qint64 nowMs) const
    {
        return m_backoff.ready(host, nowMs);
    }

    qint64 cooldownRemainingMs(const QString &host, qint64 nowMs) const
    {
        return m_backoff.msUntilReady(host, nowMs);
    }

    // How long this gateway has been silent, across restarts. 0 when it is
    // not currently in an outage.
    qint64 downForMs(const QString &host, qint64 nowMs) const
    {
        const Streak streak = m_streaks.value(host);
        if (streak.sinceMs == 0)
            return 0;
        return qMax<qint64>(0, nowMs - streak.sinceMs);
    }

    bool retired(const QString &host, qint64 nowMs) const
    {
        return downForMs(host, nowMs) >= kRetiredAfterMs;
    }

    Notice noteUnreachable(const QString &host, qint64 nowMs)
    {
        Streak &streak = m_streaks[host];
        Notice notice;
        if (streak.sinceMs == 0) {
            streak.sinceMs = nowMs;
            notice.persist = true;
        }
        notice.downForMs = qMax<qint64>(0, nowMs - streak.sinceMs);
        notice.parked = notice.downForMs >= kRetiredAfterMs;
        // Parking sets the interval outright rather than raising the ceiling
        // the ordinary ramp climbs towards: a gateway restored from a saved
        // day-long outage has only failed once in this process, and must not
        // spend another day doubling its way up from ten seconds.
        m_backoff.noteFailure(host, nowMs, kBaseCooldownMs,
                              notice.parked ? kRetiredCooldownMs
                                            : kMaxCooldownMs,
                              notice.parked ? kRetiredCooldownMs : 0);
        notice.cooldownMs = m_backoff.msUntilReady(host, nowMs);
        // Report it as it happens and once an hour while it could still be a
        // blip. Past a day the node is gone; the retirement line below is the
        // last word until it answers again.
        notice.announce =
            !notice.parked && nowMs - streak.announcedMs >= kRepeatNoticeMs;
        if (notice.announce) {
            streak.announcedMs = nowMs;
            notice.persist = true;
        }
        if (!streak.retiredNoticed && notice.parked) {
            streak.retiredNoticed = true;
            notice.retired = true;
            notice.persist = true;
        }
        return notice;
    }

    bool noteReachable(const QString &host)
    {
        m_backoff.noteSuccess(host);
        return m_streaks.remove(host) > 0;
    }

    // Drop a gateway that is no longer configured at all, so a name reused
    // later starts from a clean streak instead of inheriting a retirement.
    bool forget(const QString &host) { return m_streaks.remove(host) > 0; }

    // One "host\tsince\tannounced\tretired" record per outage in flight.
    // QStringList so the saved value stays legible in the settings file.
    QStringList persistedState() const
    {
        QStringList records;
        records.reserve(m_streaks.size());
        for (auto it = m_streaks.cbegin(); it != m_streaks.cend(); ++it) {
            if (it.key().isEmpty() || it.value().sinceMs == 0)
                continue;
            records.append(QStringLiteral("%1\t%2\t%3\t%4")
                               .arg(it.key())
                               .arg(it.value().sinceMs)
                               .arg(it.value().announcedMs)
                               .arg(it.value().retiredNoticed ? 1 : 0));
        }
        records.sort();
        return records;
    }

    // Restores the streaks only, never the backoff: the first pass after a
    // launch is entitled to probe every gateway once. A restored streak that
    // is already past the retirement threshold parks the gateway on that one
    // failure, so a dead node costs a single timeout per launch.
    void restoreState(const QStringList &records, qint64 nowMs)
    {
        m_streaks.clear();
        for (const QString &record : records) {
            const QStringList fields = record.split(QLatin1Char('\t'));
            if (fields.size() < 4)
                continue;
            const QString host = fields.at(0).trimmed();
            if (host.isEmpty())
                continue;
            Streak streak;
            streak.sinceMs = fields.at(1).toLongLong();
            streak.announcedMs = fields.at(2).toLongLong();
            streak.retiredNoticed = fields.at(3).trimmed() == QLatin1String("1");
            // A clock that moved backwards (or a settings file copied to
            // another machine) must not strand a gateway in a future outage.
            if (streak.sinceMs <= 0 || streak.sinceMs > nowMs)
                continue;
            if (streak.announcedMs > nowMs)
                streak.announcedMs = 0;
            m_streaks.insert(host, streak);
        }
    }

private:
    struct Streak {
        qint64 sinceMs = 0;
        qint64 announcedMs = 0;
        bool retiredNoticed = false;
    };
    NetworkBackoff m_backoff;
    QHash<QString, Streak> m_streaks;
};

// How long the fan-out should wait before re-pushing to the gateways an
// automatic pass had to skip, or 0 when there is nothing worth waking for.
//
// The refs a skipped gateway did not receive are not queued anywhere: the pass
// simply returns, and nothing re-pushes them until the next periodic safety
// pass. So one transient connect failure cost that mirror its whole cooldown
// *plus* up to a full sync interval of extra staleness — the fleet looked like
// it had stopped following pushes for minutes after a single blip. Waking
// exactly when the earliest skipped gateway becomes eligible again closes that
// second, avoidable half of the wait.
//
// A gateway held off longer than the periodic pass is left to that pass: it
// gets there first, so an extra timer would only add a redundant push.
inline qint64 mirrorPushRetryDelayMs(const MirrorGatewayHealth &health,
                                     const QStringList &skippedHosts,
                                     qint64 nowMs, qint64 periodicPassMs)
{
    qint64 soonest = 0;
    for (const QString &host : skippedHosts) {
        const qint64 remaining = health.cooldownRemainingMs(host, nowMs);
        if (remaining <= 0 || remaining >= periodicPassMs)
            continue;
        if (soonest == 0 || remaining < soonest)
            soonest = remaining;
    }
    // Land just past the deadline rather than on it: a timer that fires a tick
    // early finds the gateway still cooling down, and would re-arm forever.
    return soonest > 0 ? soonest + 250 : 0;
}
