#pragma once

#include "ActionStore.h"

#include <QJsonObject>
#include <QList>
#include <QString>

namespace forkmesh::mirror_actions {

inline constexpr qsizetype kMaximumSummaryRuns = 20;
inline constexpr qsizetype kMaximumSummaryLogTailBytes = 16 * 1024;
inline constexpr qint64 kSummaryLeaseMilliseconds = 10 * 60 * 1000;
inline constexpr auto kSystemSummaryPath =
    "/var/lib/forkmesh-mirror/gateway/actions-summary.json";
inline constexpr auto kSystemStatePath =
    "/var/lib/forkmesh-mirror/gateway/actions-state.json";

namespace detail {

enum class SummaryWritePolicy {
    Reject,
    SameAccount,
    RootGatewayHandoff,
};

// Deterministic ownership/mode policy shared by the filesystem writer and its
// tests. Cross-account publication is deliberately limited to the packaged
// gateway path; custom paths retain the original same-account-only contract.
SummaryWritePolicy classifySummaryWritePolicy(
    const QString &path,
    quint64 effectiveUserId,
    quint64 parentUserId,
    quint64 parentGroupId,
    quint32 parentMode,
    bool parentIsProtectedRealDirectory,
    bool targetExists,
    bool targetIsRegular,
    bool targetIsSymbolicLink,
    quint64 targetUserId,
    quint64 targetGroupId,
    quint32 targetMode,
    quint64 targetLinkCount);

} // namespace detail

// Build the exact secret-free document exposed by an actions-status gateway.
// ActionStore logs are already variable-redacted by ActionRunner. This function
// takes only a bounded UTF-8 tail and never serializes workflow content, command
// text, variable data, workflow paths, or local filesystem paths.
QJsonObject buildSummary(const QString &node,
                         const QList<ActionRun> &runs,
                         const ActionStore &store,
                         qint64 nowMs);

// Atomically replace one actions-summary.json in a protected real directory.
// Same-account output remains mode 0600. A root controller may additionally
// publish only kSystemSummaryPath as root:<protected-parent-group> mode 0640
// for the unprivileged packaged gateway.
bool writeSummaryFile(const QString &path,
                      const QString &node,
                      const QList<ActionRun> &runs,
                      const ActionStore &store,
                      qint64 nowMs);

// Atomically write the short-lived local Actions state consumed by the managed
// mirror daemon, through the same boundary as the redacted run summary.
bool writeStateFile(const QString &path,
                    const QString &node,
                    const QString &state,
                    qint64 nowMs);

} // namespace forkmesh::mirror_actions
