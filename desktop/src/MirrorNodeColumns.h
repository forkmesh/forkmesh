#pragma once

#include <QtCore/qnamespace.h>

enum MirrorNodeColumn {
    MirrorNodeColNode = 0,
    MirrorNodeColOwner,
    MirrorNodeColRepos,
    MirrorNodeColCommit,
    MirrorNodeColMessage,
    MirrorNodeColAuthor,
    MirrorNodeColSynced,
    MirrorNodeColSize,
    MirrorNodeColGitDatabase,
    MirrorNodeColIssues,
    MirrorNodeColIssuesToSource,
    MirrorNodeColCommits,
    MirrorNodeColCommitsToSource,
    MirrorNodeColBranches,
    MirrorNodeColBranchesToSource,
    MirrorNodeColPulls,
    MirrorNodeColPullsToSource,
    MirrorNodeColDiscussions,
    MirrorNodeColDiscussionsToSource,
    MirrorNodeColHealth,
    MirrorNodeColCpu,
    MirrorNodeColRam,
    MirrorNodeColDisk,
    MirrorNodeColPlatform,
    MirrorNodeColVersion,
    MirrorNodeColId,
    MirrorNodeColRoute,
    MirrorNodeColClones,
    MirrorNodeColWebsite,
    MirrorNodeColArtifacts,
    MirrorNodeColReachability,
    MirrorNodeColRole,
    MirrorNodeColumnCount,
};

inline constexpr int kMirrorDiagnosticPromptRole = Qt::UserRole + 41;
inline constexpr int kMirrorNodeKeysRole = Qt::UserRole + 60;
inline constexpr int kMirrorNodeSelfRole = Qt::UserRole + 61;
inline constexpr int kNodeGroupRole = Qt::UserRole + 62;
inline constexpr int kNodeGroupChildRole = Qt::UserRole + 63;
inline constexpr int kMirrorNodeIdRole = Qt::UserRole + 64;
inline constexpr int kMirrorNodeOnlineRole = Qt::UserRole + 65;
