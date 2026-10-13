#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QLatin1String>
#include <QString>
#include <QtGlobal>

// Sync and clone-admission verdicts for a source repository's mirror fleet.
// Header-only, like MirrorPushOutcome.h, so forkmesh-control-tests exercises
// them without linking the app.
//
// The Mirror nodes panel compares each mirror's cached observation - the SSH
// metadata snapshot and the signed direct health/branches probe, both cached
// for minutes - with the source's LIVE refs. Agents merge into main every few
// minutes and the fan-out lands each merge on every mirror within about a
// second, yet the cached observation still described the previous source
// state, so every merge painted the whole fleet "out of sync" until the cache
// expired (2026-09-12). An observation therefore records the source refs it was
// taken against, and one taken against older source refs is re-requested
// instead of being reported as divergence.
namespace forkmesh::mirrorsync {

// Observation field holding the source's heads+tags digest
// (MainWindow::mirrorStateHash) at the moment the observation was requested.
inline QString observedSourceRefsKey()
{
    return QStringLiteral("observedSourceRefsSha256");
}

// Whether `observation` may be compared with the source's current refs. An
// observation without the stamp (a test fixture, a pre-stamp disk cache) keeps
// the direct comparison, as does a source whose own digest is unknown.
inline bool observationMatchesSourceRefs(const QJsonObject &observation,
                                         const QString &currentSourceRefs)
{
    const QString observed = observation.value(observedSourceRefsKey())
                                 .toString()
                                 .trimmed()
                                 .toLower();
    const QString current = currentSourceRefs.trimmed().toLower();
    return observed.isEmpty() || current.isEmpty() || observed == current;
}

// Re-probe cadence for a settled, in-sync fleet (the historical value).
constexpr qint64 kSettledRefreshMs = 5 * 60 * 1000;
// A cached observation that predates the source's current refs is re-requested
// almost at once; the floor only stops a burst of agent ref updates from
// turning the panel into an SSH poll.
constexpr qint64 kStaleObservationRefreshMs = 20 * 1000;
// A current observation showing a mirror behind is re-checked within a minute:
// real fan-out lag clears in seconds, so a transient lag no longer lingers for
// the settled interval, while a genuinely stuck mirror stays reported.
constexpr qint64 kBehindRefreshMs = 60 * 1000;

inline qint64 refreshIntervalMs(bool staleObservation, bool observedBehind)
{
    if (staleObservation)
        return kStaleObservationRefreshMs;
    if (observedBehind)
        return kBehindRefreshMs;
    return kSettledRefreshMs;
}

// Fields that describe a node's repository state (as opposed to machine facts
// such as CPU, disk, or version).
inline bool isRepositoryStateField(const QString &key)
{
    return key == QLatin1String("commit") || key == QLatin1String("branch") ||
           key == QLatin1String("branchCount") ||
           key == QLatin1String("branchHeads") ||
           key == QLatin1String("allRefsSha256") ||
           key == QLatin1String("refsSha256") ||
           key == QLatin1String("lastCommitMessage") ||
           key == QLatin1String("lastCommitAuthorName") ||
           key == QLatin1String("lastCommitAt") ||
           key == observedSourceRefsKey();
}

// A saved host's SSH snapshot is merged over the same node's direct-probe row.
// While that SSH read is in flight, or after it failed, the snapshot still
// holds an older repository state; merged over a live signed direct probe it
// made a mirror that already had the new commit look behind (and, stamped with
// old source refs, kept a failing host from ever being re-evaluated). Its
// repository fields therefore apply only when the SSH read itself succeeded or
// there is no live direct probe to prefer.
inline bool sshSnapshotProvidesRepositoryState(const QJsonObject &sshSnapshot,
                                               const QJsonObject &directRow)
{
    return sshSnapshot.value(QStringLiteral("sshHealthy")).toBool() ||
           !directRow.value(QStringLiteral("endpointHealthy")).toBool();
}

// The relay's own clone-admission verdict for one repository, read from its
// /api/repo/<owner>/<repo>/mirrors payload: clones are rejected when at least
// one mirror is online and none is admitted to serve them (cloneAvailable).
// This replaces comparing local refs with the source's catalog `stateHash`.
// The desktop stopped publishing catalog rows on 2026-08-12, so that pin froze
// and the comparison reported "clones are being rejected" permanently while
// every mirror served. With no mirror online there is no admission verdict to
// read (the Mirror nodes table reports that outage itself), and an unreadable
// payload never raises the alarm.
inline bool clonesRejected(const QJsonObject &mirrorsPayload)
{
    bool anyOnline = false;
    const QJsonArray mirrors =
        mirrorsPayload.value(QStringLiteral("mirrors")).toArray();
    for (const QJsonValue &value : mirrors) {
        const QJsonObject mirror = value.toObject();
        if (mirror.value(QStringLiteral("cloneAvailable")).toBool())
            return false;
        if (mirror.value(QStringLiteral("status")).toString() ==
            QLatin1String("online"))
            anyOnline = true;
    }
    return anyOnline;
}

enum class MirrorLiveness { Offline, Room, Endpoint, Ssh, RelayServing };

inline MirrorLiveness catalogMirrorLiveness(const QJsonObject &record,
                                            bool relayServing)
{
    if (record.value(QStringLiteral("status")).toString() ==
        QLatin1String("online"))
        return MirrorLiveness::Room;
    if (record.value(QStringLiteral("endpointHealthy")).toBool() &&
        record.value(QStringLiteral("endpointFresh")).toBool())
        return MirrorLiveness::Endpoint;
    if (record.value(QStringLiteral("sshHealthy")).toBool())
        return MirrorLiveness::Ssh;
    if (relayServing)
        return MirrorLiveness::RelayServing;
    return MirrorLiveness::Offline;
}

} // namespace forkmesh::mirrorsync
