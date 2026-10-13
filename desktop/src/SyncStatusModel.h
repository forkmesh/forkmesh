#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringList>
#include <QtGlobal>

#include <algorithm>

// Human-readable synchronization status (forkmesh-sync-status-v1).
// Header-only, like MirrorSyncVerdict.h, so forkmesh-control-tests checks it
// against conformance/sync-status/v1/fixture.json without linking the app.
//
// An independent implementation of conformance/sync-status/v1/model.json; the
// Worker (app/src/sync_status.py), the Go mirror node, and mobile carry their
// own and the shared fixture keeps all of them identical. The output is built
// only from counts, timestamps, and closed vocabularies, so node names, peer
// addresses, and raw error text in the evidence never reach the summary.
namespace forkmesh::syncstatus {

inline QString schema()
{
    return QStringLiteral("forkmesh-sync-status-v1");
}

constexpr qint64 kStaleAfterMs = 60 * 60 * 1000;
constexpr qint64 kSyncDeadlineMs = 15 * 60 * 1000;

namespace detail {

struct OptionalMs {
    bool present = false;
    qint64 value = 0;
};

inline OptionalMs integer(const QJsonValue &value)
{
    if (!value.isDouble())
        return {};
    return {true, qint64(value.toDouble())};
}

inline void keepNewest(OptionalMs &newest, qint64 value)
{
    if (!newest.present || value > newest.value)
        newest = {true, value};
}

inline QJsonValue optionalJson(const OptionalMs &value)
{
    return value.present ? QJsonValue(value.value) : QJsonValue(QJsonValue::Null);
}

inline QString label(const QString &state)
{
    if (state == QLatin1String("queued")) return QStringLiteral("Queued");
    if (state == QLatin1String("syncing")) return QStringLiteral("Syncing");
    if (state == QLatin1String("verified")) return QStringLiteral("Verified");
    if (state == QLatin1String("degraded")) return QStringLiteral("Degraded");
    if (state == QLatin1String("conflicted")) return QStringLiteral("Conflicted");
    if (state == QLatin1String("paused")) return QStringLiteral("Paused");
    if (state == QLatin1String("external-only")) return QStringLiteral("External only");
    if (state == QLatin1String("failed")) return QStringLiteral("Failed");
    return QStringLiteral("Stale");
}

inline QString severity(const QString &state)
{
    if (state == QLatin1String("verified")) return QStringLiteral("ok");
    if (state == QLatin1String("stale") || state == QLatin1String("degraded"))
        return QStringLiteral("warning");
    if (state == QLatin1String("conflicted") || state == QLatin1String("failed"))
        return QStringLiteral("error");
    return QStringLiteral("info");
}

inline QString headline(const QString &state)
{
    if (state == QLatin1String("queued")) return QStringLiteral("Waiting to synchronize");
    if (state == QLatin1String("syncing")) return QStringLiteral("Synchronizing now");
    if (state == QLatin1String("verified")) return QStringLiteral("Every copy is verified and current");
    if (state == QLatin1String("degraded")) return QStringLiteral("Verified, but some copies are not current");
    if (state == QLatin1String("conflicted")) return QStringLiteral("Copies disagree about the latest state");
    if (state == QLatin1String("paused")) return QStringLiteral("Synchronization is paused");
    if (state == QLatin1String("external-only")) return QStringLiteral("Only stored on the external provider so far");
    if (state == QLatin1String("failed")) return QStringLiteral("Synchronization failed");
    return QStringLiteral("The visible state may be out of date");
}

inline bool reportedError(const QString &code)
{
    static const QStringList codes = {
        QStringLiteral("rejected"), QStringLiteral("signature_invalid"),
        QStringLiteral("unauthorized"), QStringLiteral("provider_unavailable"),
        QStringLiteral("network"), QStringLiteral("quota_exceeded"),
    };
    return codes.contains(code);
}

inline QString action(const QString &code)
{
    if (code == QLatin1String("conflict")) return QStringLiteral("Review the conflicting changes and choose which state to keep.");
    if (code == QLatin1String("rejected")) return QStringLiteral("A copy rejected the update; check the node's signing key and push again.");
    if (code == QLatin1String("signature_invalid")) return QStringLiteral("The update's signature did not verify; re-sign it from a trusted device.");
    if (code == QLatin1String("unauthorized")) return QStringLiteral("This node is not authorized to publish; reconnect the account and retry.");
    if (code == QLatin1String("provider_unavailable")) return QStringLiteral("The external provider did not respond; ForkMesh retries automatically.");
    if (code == QLatin1String("network")) return QStringLiteral("A network error interrupted synchronization; it retries when the connection returns.");
    if (code == QLatin1String("quota_exceeded")) return QStringLiteral("A service quota was reached; synchronization resumes when the quota resets.");
    if (code == QLatin1String("sync_delayed")) return QStringLiteral("Synchronization is taking longer than expected; check that the source node is online.");
    if (code == QLatin1String("unreachable")) return QStringLiteral("One or more copies could not be reached; check that the mirrors are online.");
    if (code == QLatin1String("unverified")) return QStringLiteral("A copy reports the latest state without a verified signature; wait for it to re-attest.");
    if (code == QLatin1String("copies_behind")) return QStringLiteral("Some copies have not received the latest state yet; they normally catch up within minutes.");
    if (code == QLatin1String("confirmation_expired")) return QStringLiteral("No copy has confirmed the state recently; check that the mirrors are online.");
    if (code == QLatin1String("unconfirmed")) return QStringLiteral("No copy has confirmed the latest state yet.");
    return QStringLiteral("An unexpected error stopped synchronization; see the node's log for details.");
}

inline QString plural(int count, const char *singular, const char *pluralForm)
{
    return QStringLiteral("%1 %2").arg(count).arg(
        QLatin1String(count == 1 ? singular : pluralForm));
}

inline QString age(qint64 ms)
{
    const qint64 minutes = ms / 60000;
    if (minutes < 60)
        return QStringLiteral("%1 min").arg(minutes);
    const qint64 hours = ms / 3600000;
    if (hours < 48)
        return QStringLiteral("%1 h").arg(hours);
    return QStringLiteral("%1 d").arg(ms / 86400000);
}

} // namespace detail

// Applies the model's precedence rules to one object's evidence.
inline QJsonObject evaluate(const QJsonObject &evidence)
{
    using namespace detail;
    const qint64 now = integer(evidence.value(QStringLiteral("now"))).value;
    const QJsonArray copies = evidence.value(QStringLiteral("copies")).toArray();
    const OptionalMs requiredValue =
        integer(evidence.value(QStringLiteral("requiredCopies")));
    const qint64 required =
        std::max<qint64>(1, requiredValue.present ? requiredValue.value : 1);
    const qint64 conflicts =
        integer(evidence.value(QStringLiteral("conflicts"))).value;
    const OptionalMs queuedAt = integer(evidence.value(QStringLiteral("queuedAt")));
    const OptionalMs startedAt =
        integer(evidence.value(QStringLiteral("syncStartedAt")));
    const OptionalMs sourceUpdatedAt =
        integer(evidence.value(QStringLiteral("sourceUpdatedAt")));
    QString lastError = evidence.value(QStringLiteral("lastError")).toString();
    if (!lastError.isEmpty() && !reportedError(lastError))
        lastError = QStringLiteral("unknown");

    int total = 0;
    int reached = 0, verifiedCurrent = 0, trustedCurrent = 0;
    int rejected = 0, unreachable = 0, unverified = 0;
    OptionalMs lastConfirmed, currentConfirmed;
    for (const QJsonValue &value : copies) {
        if (!value.isObject())
            continue;
        ++total;
        const QJsonObject copy = value.toObject();
        QString status = copy.value(QStringLiteral("status")).toString();
        if (status != QLatin1String("current") && status != QLatin1String("behind") &&
            status != QLatin1String("rejected"))
            status = QStringLiteral("unreachable");
        const bool verified = copy.value(QStringLiteral("verified")).toBool(false);
        const OptionalMs confirmedAt =
            integer(copy.value(QStringLiteral("confirmedAt")));
        if (status != QLatin1String("unreachable"))
            ++reached;
        if (status == QLatin1String("rejected"))
            ++rejected;
        else if (status == QLatin1String("unreachable"))
            ++unreachable;
        if (status == QLatin1String("current") && verified) {
            ++verifiedCurrent;
            if (copy.value(QStringLiteral("trusted")).toBool(false))
                ++trustedCurrent;
            if (confirmedAt.present)
                keepNewest(currentConfirmed, confirmedAt.value);
        } else if (status == QLatin1String("current")) {
            ++unverified;
        }
        if (verified && status != QLatin1String("rejected") && confirmedAt.present)
            keepNewest(lastConfirmed, confirmedAt.value);
    }

    const bool inFlight = startedAt.present;
    const bool delayed = inFlight && now - startedAt.value > kSyncDeadlineMs;

    QString state;
    QString error;
    if (evidence.value(QStringLiteral("paused")).toBool(false)) {
        state = QStringLiteral("paused");
    } else if (conflicts > 0) {
        state = QStringLiteral("conflicted");
        error = QStringLiteral("conflict");
    } else if (evidence.value(QStringLiteral("externalOnly")).toBool(false) &&
               total == 0 && lastError.isEmpty()) {
        state = QStringLiteral("external-only");
    } else if (verifiedCurrent >= required) {
        if (!currentConfirmed.present ||
            now - currentConfirmed.value > kStaleAfterMs) {
            state = QStringLiteral("stale");
            error = QStringLiteral("confirmation_expired");
        } else if (verifiedCurrent < total) {
            state = QStringLiteral("degraded");
            error = rejected ? QStringLiteral("rejected")
                    : unreachable ? QStringLiteral("unreachable")
                    : unverified ? QStringLiteral("unverified")
                                 : QStringLiteral("copies_behind");
        } else {
            state = QStringLiteral("verified");
        }
    } else if (inFlight && !delayed) {
        state = QStringLiteral("syncing");
    } else if (queuedAt.present && !inFlight) {
        state = QStringLiteral("queued");
    } else if (rejected) {
        state = QStringLiteral("failed");
        error = lastError.isEmpty() ? QStringLiteral("rejected") : lastError;
    } else if (!lastError.isEmpty()) {
        state = QStringLiteral("failed");
        error = lastError;
    } else if (delayed) {
        state = QStringLiteral("stale");
        error = QStringLiteral("sync_delayed");
    } else if (total > 0 && unreachable == total) {
        state = QStringLiteral("stale");
        error = QStringLiteral("unreachable");
    } else {
        state = QStringLiteral("stale");
        error = QStringLiteral("unconfirmed");
    }

    OptionalMs lag;
    if (state == QLatin1String("verified"))
        lag = {true, 0};
    else if (sourceUpdatedAt.present)
        lag = {true, std::max<qint64>(0, now - sourceUpdatedAt.value)};

    QStringList parts;
    parts << headline(state) + QLatin1Char('.');
    parts << QStringLiteral("%1 of %2 verified, %3, %4 reached.")
                 .arg(verifiedCurrent)
                 .arg(plural(total, "copy", "copies"),
                      plural(trustedCurrent, "trusted mirror", "trusted mirrors"),
                      plural(reached, "node", "nodes"));
    if (!lastConfirmed.present)
        parts << QStringLiteral("Never confirmed.");
    else if (now - lastConfirmed.value < 60000)
        parts << QStringLiteral("Last confirmed just now.");
    else
        parts << QStringLiteral("Last confirmed %1 ago.")
                     .arg(age(now - lastConfirmed.value));
    if (lag.present && lag.value >= 60000)
        parts << QStringLiteral("%1 behind the source.").arg(age(lag.value));
    const QString actionText = error.isEmpty() ? QString() : action(error);
    if (!actionText.isEmpty())
        parts << actionText;

    return QJsonObject{
        {QStringLiteral("schema"), schema()},
        {QStringLiteral("state"), state},
        {QStringLiteral("label"), label(state)},
        {QStringLiteral("severity"), severity(state)},
        {QStringLiteral("current"),
         state == QLatin1String("verified") || state == QLatin1String("degraded")},
        {QStringLiteral("totalCopies"), total},
        {QStringLiteral("nodesReached"), reached},
        {QStringLiteral("verifiedCopies"), verifiedCurrent},
        {QStringLiteral("trustedMirrors"), trustedCurrent},
        {QStringLiteral("lastConfirmedAt"), optionalJson(lastConfirmed)},
        {QStringLiteral("lagMs"), optionalJson(lag)},
        {QStringLiteral("error"),
         error.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(error)},
        {QStringLiteral("action"),
         actionText.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(actionText)},
        {QStringLiteral("summary"), parts.join(QLatin1Char(' '))},
    };
}

// Evidence for a GET /api/repo/<owner>/<repo>/mirrors payload.
inline QJsonObject mirrorsPayloadEvidence(const QJsonObject &payload)
{
    QJsonArray copies;
    for (const QJsonValue &value : payload.value(QStringLiteral("mirrors")).toArray()) {
        if (!value.isObject())
            continue;
        const QJsonObject row = value.toObject();
        const QString integrity = row.value(QStringLiteral("integrity")).toString();
        QString status = QStringLiteral("current");
        if (row.value(QStringLiteral("status")).toString() != QLatin1String("online"))
            status = QStringLiteral("unreachable");
        else if (integrity == QLatin1String("rejected"))
            status = QStringLiteral("rejected");
        else if (row.value(QStringLiteral("behind")).toBool(false) ||
                 integrity == QLatin1String("healing"))
            status = QStringLiteral("behind");
        copies.append(QJsonObject{
            {QStringLiteral("status"), status},
            {QStringLiteral("verified"), integrity == QLatin1String("ok")},
            {QStringLiteral("trusted"),
             row.value(QStringLiteral("cloneAvailable")).toBool(false) ||
                 row.value(QStringLiteral("endpointHealthy")).toBool(false)},
            {QStringLiteral("confirmedAt"),
             detail::optionalJson(detail::integer(row.value(QStringLiteral("lastSync"))))},
        });
    }
    const detail::OptionalMs freshest = detail::integer(
        payload.value(QStringLiteral("summary")).toObject().value(QStringLiteral("freshestSync")));
    return QJsonObject{
        {QStringLiteral("now"),
         detail::integer(payload.value(QStringLiteral("generatedAt"))).value},
        {QStringLiteral("sourceUpdatedAt"),
         freshest.present && freshest.value != 0 ? QJsonValue(freshest.value)
                                                 : QJsonValue(QJsonValue::Null)},
        {QStringLiteral("copies"), copies},
    };
}

inline QJsonObject mirrorsPayloadStatus(const QJsonObject &payload)
{
    return evaluate(mirrorsPayloadEvidence(payload));
}

// The Mirror nodes panel keeps only the payload's mirror rows (merged with the
// signed direct probes); rebuild the payload envelope around them.
inline QJsonObject mirrorRowsStatus(const QJsonArray &rows, qint64 nowMs)
{
    qint64 freshest = 0;
    for (const QJsonValue &value : rows)
        freshest = std::max(freshest, detail::integer(
            value.toObject().value(QStringLiteral("lastSync"))).value);
    return mirrorsPayloadStatus(QJsonObject{
        {QStringLiteral("generatedAt"), nowMs},
        {QStringLiteral("summary"),
         QJsonObject{{QStringLiteral("freshestSync"), freshest}}},
        {QStringLiteral("mirrors"), rows},
    });
}

} // namespace forkmesh::syncstatus
