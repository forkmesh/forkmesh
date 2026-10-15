#pragma once

#include "AgentToolEfficiencyLedger.h"

#include <QByteArray>
#include <QHash>
#include <QJsonArray>
#include <QString>
#include <QVector>

#include <functional>

// The verb a shell command line is about ("git status", "python3 inline
// script", "pytest"), ignoring setup such as `cd dir &&` and `timeout 60`.
QString agentShellCommandShape(const QString &command);

// One aggregated improvement signal: every occurrence with the same kind,
// family, runtime tool, and signature folds into a single row so a runaway
// pattern reads as "x40" instead of forty lines.
struct AgentToolImprovement {
    QString key;
    QString kind;        // AgentToolImprovementLog::Kind*
    QString family;
    QString runtimeName;
    QString signature;   // extra grouping (normalized error text), may be empty
    QString topic;       // what the signature means to a reader ("sed · missing path")
    QString detail;      // newest example: path, pattern, command, or error
    QString suggestion;  // what to change in the runtime, prompt, or host
    qint64 occurrences = 0;
    qint64 tokensAtStake = 0; // estimated context the pattern cost
    qint64 firstMs = 0;
    qint64 lastMs = 0;
};

// Realtime log of potential tool-use improvements, derived from the calls
// the efficiency ledger observes. Every occurrence is appended to a local
// JSONL file the moment it is detected, so the log can be tailed, handed to
// an agent, or fed into a teammate; the in-memory view aggregates it for the
// board and for the markdown digest the operator copies back into the
// system. Like the ledger it never reads file contents: only sizes, tool
// arguments, and the leading lines of error results.
class AgentToolImprovementLog
{
public:
    static inline const QString KindRepeatedCall = QStringLiteral("repeated-call");
    static inline const QString KindWholeFileRead = QStringLiteral("whole-file-read");
    static inline const QString KindWholeFileEdit = QStringLiteral("whole-file-edit");
    static inline const QString KindOversizedResult = QStringLiteral("oversized-result");
    static inline const QString KindToolError = QStringLiteral("tool-error");
    static inline const QString KindSlowCall = QStringLiteral("slow-call");
    static inline const QString KindCalledWhileOff = QStringLiteral("called-while-off");
    static inline const QString KindUncatalogued = QStringLiteral("uncatalogued-tool");
    static inline const QString KindAbandonedCall = QStringLiteral("abandoned-call");

    static constexpr int kMaxSignals = 200;
    static constexpr int kMaxSeenPerSession = 256;
    static constexpr qint64 kRepeatWindowMs = 10 * 60 * 1000;
    static constexpr qint64 kOversizedResultTokens = 8000;
    static constexpr qint64 kWholeFileMinTokens = 3000;
    static constexpr qint64 kWholeEditMinTokens = 1000;
    static constexpr qint64 kSlowCallMs = 60 * 1000;
    static constexpr qint64 kRotateBytes = 2 * 1024 * 1024;
    // Occurrences this much older than the newest one are dropped on load,
    // so a signal fixed long ago stops resurfacing in the board and digest.
    static constexpr qint64 kMaxAgeMs = 7 * 24 * 60 * 60 * 1000LL;

    explicit AgentToolImprovementLog(QString logPath = QString());

    // Inspects one finished (or abandoned) call. Returns how many
    // occurrences were recorded; each one is appended to the log file.
    int observe(const AgentToolCallObservation &call);
    // The turn ended: files may change before the next one, so a later
    // identical read is no longer a repeat.
    void forgetSession(int sessionId);

    // Most recently seen first.
    QVector<AgentToolImprovement> improvements() const;
    int signalCount() const { return int(m_signals.size()); }
    qint64 occurrenceCount() const { return m_occurrences; }

    // Markdown ready to paste into an agent prompt or an issue: signals
    // ranked by context at stake, each with its newest example and the
    // suggested change. fullLogPath overrides the log file the digest
    // points at (the archive, once the signals are handed off).
    QString digestMarkdown(int limit = 25,
                           const QString &fullLogPath = QString()) const;

    // Rebuilds the aggregate view from the JSONL file; malformed lines are
    // skipped so a torn final write never loses the rest of the log.
    bool load(QString *error = nullptr);
    // Forgets every signal and deletes the log file.
    void clear();
    // The signals were handed to an agent to fix: moves the log to
    // sentLogPath() (replacing the previous hand-off) and starts afresh, so
    // the next digest carries only what happened since.
    bool archiveSent();
    QString logPath() const { return m_logPath; }
    QString sentLogPath() const;
    void loadSnapshot(const QJsonArray &signalRows, qint64 occurrences);

    static QString kindLabel(const QString &kind);
    static bool kindIsFailure(const QString &kind);

    // Invoked after every change so the UI can debounce a refresh.
    std::function<void()> changed;
    std::function<void(int sessionId, const QString &kind, qint64 tokens)> occurred;

private:
    struct Occurrence {
        QString kind;
        QString family;
        QString runtimeName;
        QString signature;
        QString topic;
        QString detail;
        QString suggestion;
        QString provider;
        QString model;
        int sessionId = 0;
        qint64 tokens = 0;
        qint64 ms = 0;
    };

    void record(const Occurrence &occurrence, bool append);
    void appendLine(const Occurrence &occurrence);
    bool noteRepeat(const AgentToolCallObservation &call);

    QString m_logPath;
    QVector<AgentToolImprovement> m_signals;
    QHash<QString, int> m_index;
    // sessionId -> fingerprint of a successful read-only call -> finished ms
    QHash<int, QHash<QByteArray, qint64>> m_seen;
    qint64 m_occurrences = 0;
};
