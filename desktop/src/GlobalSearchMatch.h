#pragma once

// Query matching for the search surfaces — the Ctrl+K overlay
// (MainWindowSearch.cpp), the header dropdown, and the full results page
// (MainWindowRepoDetail.cpp) — kept in its own GUI-free translation unit so
// the rules that decide what a search finds, and the parsers for the git
// output those rules run over, are unit-tested (tests/test_crypto.cpp)
// instead of only reachable through a live MainWindow. Sharing them is what
// keeps the three surfaces from quietly disagreeing about what an issue or
// thread contains.
// Everything here is pure: no Qt widgets, no processes, no disk.

#include "DiscussionStore.h"
#include "IssueStore.h"
#include "ProjectStore.h"
#include "PullStore.h"

#include <QString>
#include <QStringList>
#include <QVector>

namespace forkmesh::search {

struct Category {
    const char *kind;  // the GlobalSearchHit::kind this describes
    const char *title; // group header text
    const char *icon;  // octicon name
    const char *color; // icon tint
};

int categoryCount();
const Category &categoryAt(int index);
int categoryIndexOf(const QString &kind);

// One collaboration record matched by a repository-wide query. Keeping this
// projection GUI-free lets both search UIs use exactly the same title/body/
// comment rules instead of quietly disagreeing about what an issue or thread
// contains.
struct RecordHit {
    QString kind;    // issue | milestone | pull | discussion | project
    int number = 0;  // record number (milestones use path instead)
    QString path;    // milestone title
    QString primary; // row label
    QString detail;  // where the query matched
    QString status;
};

struct StatusBadge {
    QString label;
    QString icon;
    QString color;
};
StatusBadge statusBadge(const QString &kind, const QString &status);

struct HighlightRun {
    QString text;
    bool match = false;
};
QVector<HighlightRun> highlightRuns(const QString &text, const QString &query);

QVector<RecordHit> matchRepositoryRecords(
    const QString &query, const QList<Issue> &issues,
    const QList<IssueMilestone> &milestones,
    const QList<PullRequest> &pulls,
    const QList<Discussion> &discussions,
    const QList<Project> &projects, int limitPerKind = 40);

bool containsFold(const QString &haystack, const QString &needle);

QString snippetAround(const QString &haystack, const QString &needle);

bool matchTitleBody(const QString &query, const QString &title,
                    const QString &body, QString *where);

// Search a signed record's title/body/comment log. Returns true and fills
// *where with a context note the first time the query is found.
bool matchIssue(const Issue &issue, const QString &query, QString *where);
bool matchPull(const PullRequest &pull, const QString &query, QString *where);
bool matchDiscussion(const Discussion &discussion, const QString &query,
                     QString *where);
bool matchProject(const Project &project, const QString &query, QString *where);

struct WorktreeRecord {
    QString path;
    QString branch; // short name; empty when detached or bare
    QString head;   // commit the worktree is parked on
    bool detached = false;
};
QVector<WorktreeRecord> parseWorktreePorcelain(const QStringList &lines);

// The pull ledger lives on forkmesh/pulls and forkmesh/pull/<n> branches; they
// hold signed records, not anyone's work, so branch/worktree/commit search
// leaves them out.
bool isLedgerBranch(const QString &branch);

// Revision arguments for a `git log` that walks every local and remote-tracking
// branch (skipping the ledger branches) plus the browsed ref, with --source so
// %S names the branch each commit was reached through.
QStringList allBranchRevisionArgs(const QString &ref);

struct CodeRow {
    QString path;
    int line = 0;
    QString text;
    bool valid = false;
};
CodeRow parseGrepRow(const QString &row, const QString &ref);

} // namespace forkmesh::search
