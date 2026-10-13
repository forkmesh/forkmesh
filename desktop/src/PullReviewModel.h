#pragma once

#include "PullStore.h"

#include <QHash>
#include <QList>
#include <QString>

struct PullReviewFileSummary {
    QString path;
    int totalThreads = 0;
    int unresolvedThreads = 0;
    int resolvedThreads = 0;
    int suggestions = 0;
    int outdatedThreads = 0;
};

// Where a review thread sits relative to the diff being shown right now.
//
//   Current  — the thread's patchset is the one on screen, or its anchored
//              text is still at the same lines: paint it there.
//   Moved    — the diff was re-cut since the thread was written and its
//              anchored text now sits at different line numbers: paint it at
//              currentLineStart..currentLineEnd and say where it came from.
//   Outdated — the anchored text is no longer in the diff (or a legacy thread
//              points at a line the diff no longer has): keep the conversation,
//              but never paint it on a line it did not talk about.
//   Legacy   — pre-anchor thread whose line still exists; painted where it
//              says, with no proof it is the same code.
enum class PullThreadPlacement { Current, Moved, Outdated, Legacy };

struct PullReviewThread {
    QString id;
    QString path;
    QString side;
    int lineStart = 0;
    int lineEnd = 0;
    bool resolved = false;
    bool hasSuggestion = false;
    QString suggestionState;
    QString appliedCommit;
    // Signed anchor from the opening event (empty for legacy threads).
    QString patchset;
    QString anchorLines;
    // Placement against the patch buildPullReviewSnapshot() was given.
    PullThreadPlacement placement = PullThreadPlacement::Legacy;
    int currentLineStart = 0; // 0 when outdated
    int currentLineEnd = 0;
    QList<PullEvent> events;

    bool outdated() const { return placement == PullThreadPlacement::Outdated; }
    bool moved() const { return placement == PullThreadPlacement::Moved; }
};

// Machine-authorship provenance for a PR, read from the `ForkMesh-Agent:
// <tool>/<model>` commit trailer that AgentRunner stamps on agent commits
// The trailer travels inside the signed commit series (commits.mbox
// is folded into the pull signature), so this attribution is verifiable and
// cross-node — no local AgentSession required.
struct PullAgentProvenance {
    bool isAgent = false;
    QString tool;   // e.g. "claude-code"
    QString model;  // e.g. "claude-opus-4-8"
    QString value;  // raw trailer value "<tool>/<model>"
};

PullAgentProvenance pullAgentProvenance(const PullRequest &pr);
PullAgentProvenance pullAgentProvenanceIn(const QString &commits);

QHash<QString, bool> pullFileAuthorship(const PullRequest &pr);

struct PullReviewSnapshot {
    QString reviewSummary;
    int topLevelItems = 0;
    int totalThreads = 0;
    int unresolvedThreads = 0;
    int resolvedThreads = 0;
    int suggestions = 0;
    int outdatedThreads = 0;
    QList<PullReviewThread> threads;
    QHash<QString, PullReviewFileSummary> files;
};

// Folds the pull's event log into threads and places each one against
// pr.patch (the diff the caller is about to show) using pr.patchHeadOid as the
// current patchset.
PullReviewSnapshot buildPullReviewSnapshot(const PullRequest &pr);

// The patchset id new inline comments on `pr` anchor to: the head commit its
// patch was cut against, or empty when no client could name one.
QString pullPatchsetId(const PullRequest &pr);

// The text of `path`'s lines lineStart..lineEnd on `side` ("old"/"new") in a
// unified diff, prefix stripped and newline-joined. Bounded (see
// kPullAnchorMaxLines); empty when the range is not in the diff.
QString pullAnchorLines(const QString &patch, const QString &path,
                        const QString &side, int lineStart, int lineEnd);
constexpr int kPullAnchorMaxLines = 12;

// The anchor a new inline comment on `pr` should carry.
PullAnchor pullAnchorFor(const PullRequest &pr, const QString &path,
                         const QString &side, int lineStart, int lineEnd);

// Places one thread against `patch` cut at `currentPatchset`. Exposed for
// tests and for the web/desktop parity vectors; buildPullReviewSnapshot() calls
// it for every thread.
void placePullReviewThread(PullReviewThread &thread, const QString &patch,
                           const QString &currentPatchset);

// Short human description of a thread's placement for badges and tooltips
// ("Outdated", "Moved from line 12"), empty when it sits where it was written.
QString pullThreadPlacementLabel(const PullReviewThread &thread);
