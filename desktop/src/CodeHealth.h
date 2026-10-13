#pragma once

// Static + runtime health of this codebase, for the Component health page.
//
// Every rule here is a class of defect that has already frozen this app at
// least once (the stall-watchdog rounds): subprocess waits inside loops,
// per-widget setStyleSheet, QSettings rebuilt per iteration, regular
// expressions compiled per iteration, event-loop pumping inside a loop body.
// It is deliberately not a general-purpose linter — a finding here should
// always map to a real stall someone can go and remove.
//
// Pure Qt Core: no widgets, so the whole analysis is unit-testable and can run
// on a worker thread. The GUI lives in ComponentHealthBoard.

#include <QDateTime>
#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>

namespace forkmesh::codehealth {

// One suspicious pattern found inside a function.
struct Finding {
    QString kind; // stable id: "git-in-loop", "stylesheet-in-loop", ...
    QString why;  // one line a person can act on
    int line = 0; // 1-based, in the file
};

struct Function {
    QString name;  // "MainWindow::renderIssueThread"
    int line = 0;  // 1-based line of the definition
    int loc = 0;   // lines from the definition to its closing brace
    int loopDepth = 0; // deepest nested loop level in the body
    int stallMs = 0;   // milliseconds the stall log attributes to this name
    QVector<Finding> findings;
};

struct FileHealth {
    QString path;      // relative to the source root
    QString display;   // basename, what a tiny card shows
    QString component; // "UI · Issues", "Pulls", "Core", ...
    int loc = 0;       // physical lines
    int codeLoc = 0;   // lines with code once comments/literals are blanked
    int functionCount = 0;
    int findingCount = 0;
    int longestFunction = 0;
    int acknowledgedCount = 0; // findings cleared by a code-health: allow()
    int stallMs = 0;
    int health = 100; // 0-100, lower is worse
    qint64 modifiedMs = 0; // file mtime, for "recently changed" ordering
    QVector<Function> functions;
    QHash<QString, int> findingKinds;
};

struct ComponentRollup {
    QString component;
    int files = 0;
    int loc = 0;
    int functions = 0;
    int findings = 0;
    int stallMs = 0;
    int health = 100;
};

struct Report {
    QVector<FileHealth> files;          // worst health first
    QVector<ComponentRollup> components; // worst health first
    int totalLoc = 0;
    int totalFunctions = 0;
    int acknowledgedCount = 0;
    // Stall attribution. `unattributedMs` is the honest part: most stall
    // samples name only Qt/libc frames, so they belong to no component and
    // must not be spread over the code as if they did.
    int stallRecords = 0;
    qint64 stallTotalMs = 0;
    qint64 stallUnattributedMs = 0;
    // Records the watchdog itself judged machine-wide memory/io starvation.
    // Their frames show where the GUI thread waited on paging, so they are
    // blamed on no function either.
    qint64 stallStarvedMs = 0;
    QDateTime scannedAt;
    qint64 scanMs = 0;
    bool cancelled = false;
};

// Progress reporting for the worker. Return false to cancel the scan.
using ProgressFn =
    std::function<bool(int done, int total, const QString &currentFile)>;

// Blank comments and string/char literal bodies, preserving every line and
// column so reported line numbers stay exact. Exposed for tests.
QString stripNoise(const QString &text);

// Parse function definitions out of already-blanked source. Exposed for tests.
QVector<Function> scanFunctions(const QString &clean);

// Group a file into the area a person actually thinks in. Exposed for tests.
QString componentFor(const QString &relativePath);

// 0-100. Size alone is not unhealthy; unbounded work on the UI thread is.
int scoreFile(const FileHealth &file);

// A finding someone has checked and kept is acknowledged in the source, with
// the reason, in the comment block above the definition or inside its body:
//
//     // code-health: allow(blocking-wait) runs on the QtConcurrent worker
//
// It clears only the named kinds in that one function and requires a reason,
// so the exemption stays visible and reviewable next to the code. Exposed for
// tests: returns the kinds allowed by `rawLines[first, last)`.
QStringList acknowledgedKinds(const QStringList &rawLines, int first, int last);

// Walk `dirs` (relative to `root`) and build the report. `diagnosticsDir` is
// where stalls.log lives; pass an empty string to skip runtime attribution.
Report analyse(const QString &root, const QStringList &dirs,
               const QString &diagnosticsDir,
               const ProgressFn &progress = {});

} // namespace forkmesh::codehealth
