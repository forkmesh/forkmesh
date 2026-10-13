#pragma once

#include "ActionFile.h"
#include "ActionStore.h"
#include "IssueStore.h"

#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>

enum class RepoSecuritySeverity {
    Pass = 0,
    Info,
    Warning,
    High,
    Critical,
};

struct RepoSecuritySignal {
    QString key;
    QString title;
    RepoSecuritySeverity severity = RepoSecuritySeverity::Info;
    QString summary;
    QString detail;
    QString actionLabel;
    QString actionTarget;
    QStringList items;
};

struct RepoSecurityFinding {
    QString id;
    QString category;
    RepoSecuritySeverity severity = RepoSecuritySeverity::Info;
    QString title;
    QString detail;
    QString path;
    int line = 0;
    QString recommendedAction;
};

struct RepoSecurityInput {
    QString owner;
    QString name;
    QString localPath;
    QString mirrorPath;
    QString ref;
    bool publishToNetwork = false;
    bool isPrivate = false;
    bool previewOnly = false;
    bool actionsEnabled = true;
    bool integrityWarning = false;
    QList<Issue> issues;
    QList<ActionWorkflow> workflows;
    QList<ActionRun> actionRuns;
};

struct RepoSecuritySnapshot {
    QString repoKey;
    QString ref;
    qint64 generatedAtMs = 0;
    QList<RepoSecuritySignal> signalList;
    QList<RepoSecurityFinding> findings;
    QHash<QString, int> dependencyCounts;
};

// Result of rescanning a single dependency manifest (the dependency card's
// per-row "Run scan" button), as opposed to RepoSecurity::scan()'s full
// repo-wide pass. Kept separate so a single-manifest rescan stays cheap: it
// reads only that one file instead of every tracked file in the repo.
struct RepoSecurityManifestScan {
    int dependencyCount = 0;
    QList<RepoSecurityFinding> findings;
};

class RepoSecurity
{
public:
    static RepoSecuritySnapshot scan(const RepoSecurityInput &input);
    static RepoSecurityManifestScan scanManifest(const RepoSecurityInput &input,
                                                 const QString &manifestPath);
    static QString severityText(RepoSecuritySeverity severity);
    static RepoSecuritySeverity highestSeverity(const RepoSecuritySnapshot &snapshot);
    // Scan commits about to be pushed for secrets. upstreamRef is the remote
    // tracking ref (e.g. "refs/remotes/origin/main"); if empty, all tracked
    // text files in localPath are scanned instead.
    // To intentionally include a fake/example credential (e.g. a test
    // fixture), append a comment containing "forkmesh-secret-scan:ignore-line"
    // to that line; matches on that line are skipped. A trailing "\" joins
    // physical lines into one logical line, and the marker may sit anywhere in
    // that run — so a statement continued across several lines is suppressed
    // by a marker at the end of its last line.
    static QList<RepoSecurityFinding> findSecretsInPush(const QString &localPath,
                                                        const QString &upstreamRef);
    // The same high-confidence added-line scan, for an already-produced unified
    // diff. This keeps review surfaces from growing a second, looser set of
    // credential patterns just to grade the patch already on screen.
    static QList<RepoSecurityFinding> findSecretsInDiff(const QString &diff);
    // Appends the "forkmesh-secret-scan:ignore-line" marker (see above) to the
    // logical line a finding points at, so a reviewed/false-positive match no
    // longer blocks future scans. Picks comment tokens by file extension.
    // Returns false if the file or line couldn't be read/written, if the line
    // no longer holds a match (a stale finding must not rewrite unrelated
    // code), or if the file type has no comment syntax to annotate with. A
    // line that already carries the marker is left untouched and counts as
    // success.
    static bool markFindingSafe(const QString &localPath,
                                const RepoSecurityFinding &finding);
};

// Local quality evidence for the Quality tab: check-run health, code volume,
// documentation/tests presence, TODO markers, file-size health, commit
// activity and issue hygiene. Shares the signal/finding shapes (and the
// snapshot container) with RepoSecurity so both tabs render the same way.
class RepoQuality
{
public:
    static RepoSecuritySnapshot scan(const RepoSecurityInput &input);
};
