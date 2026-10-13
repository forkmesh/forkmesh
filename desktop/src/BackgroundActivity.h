#pragma once

#include "ActionTelemetry.h"
#include "LogSource.h"

#include <QAtomicInteger>
#include <QDateTime>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QString>
#include <QStringList>

#include <functional>
#include <utility>
#include <vector>

namespace forkmesh {

// Names the routine on whose behalf background work is being asked for — a
// timer tick ("home/repository stats timer (every 60s)"), a user action
// ("opening repo me/r"), an agent flow ("agent files diff poll · agent #42") —
// for as long as the scope lives on this thread. BackgroundActivity::begin() stamps
// the innermost one onto every ticket it opens, so the footer's outcome line
// can say *why* a `git status` ran and not only which line of code asked for
// it (adhoc #186). The stack is per thread: a chokepoint that hops threads
// (runGitDetached, runGitCaptureBackgrounded, runOffThread) reads current()
// before the hop and re-installs it on the far side. An empty name installs
// nothing, so re-installing an unknown routine never shadows a known one.
class BackgroundRoutine
{
public:
    explicit BackgroundRoutine(const QString &name)
        : m_pushed(!name.trimmed().isEmpty())
    {
        if (m_pushed)
            stack().push_back(name.simplified());
    }
    ~BackgroundRoutine()
    {
        if (m_pushed && !stack().empty())
            stack().pop_back();
    }
    BackgroundRoutine(const BackgroundRoutine &) = delete;
    BackgroundRoutine &operator=(const BackgroundRoutine &) = delete;

    // The innermost routine named on the calling thread, or empty.
    static QString current()
    {
        const std::vector<QString> &names = stack();
        return names.empty() ? QString() : names.back();
    }

    // `name` when nothing is named on this thread yet, else empty: for a
    // generic inner routine (a debounced refresh many flows share) that should
    // not shadow the more specific name an outer one already gave.
    static QString unlessNamed(const QString &name)
    {
        return current().isEmpty() ? name : QString();
    }

private:
    static std::vector<QString> &stack()
    {
        thread_local std::vector<QString> names;
        return names;
    }

    bool m_pushed = false;
};

// Everything a caller can say about one unit of background work when it opens
// a ticket. kind/detail/execution/source are what the bus always took; the
// rest is the attribution adhoc #186 asked for: which routine asked for the
// work, which checkout it ran in, and how long it waited for an admission slot
// before it could start.
struct BackgroundWork {
    QString kind;
    QString detail;
    ActionTelemetry::Execution execution = ActionTelemetry::Execution::Async;
    LogSource source;
    // Empty = whatever BackgroundRoutine is current on the thread begin() runs
    // on. Set it explicitly when the ticket opens on a different thread from
    // the one that asked for the work.
    QString routine;
    QString workingDirectory;
    qint64 queuedMs = -1; // < 0: never waited in a queue
};

// A ticket as the listener sees it: the work plus the bus's own bookkeeping.
struct BackgroundTicket : BackgroundWork {
    quint64 id = 0;
    qint64 startedAtMs = 0;
    // "succeeded", "failed (exit 128)", "timed out", "cancelled", … — set on
    // the retire notification only.
    QString outcome;
};

class BackgroundActivity
{
public:
    using Listener =
        std::function<void(const BackgroundTicket &ticket, bool started)>;

    static quint64 begin(BackgroundWork work)
    {
        if (work.routine.isEmpty())
            work.routine = BackgroundRoutine::current();
        const quint64 id = state().nextId.fetchAndAddOrdered(1);
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        BackgroundTicket ticket;
        static_cast<BackgroundWork &>(ticket) = std::move(work);
        ticket.id = id;
        ticket.startedAtMs = now;
        ActionTelemetry::started(id, ticket.kind, ticket.detail,
                                 ticket.execution, now,
                                 telemetryContext(ticket));
        {
            QMutexLocker lock(&state().mutex);
            state().tickets.insert(id, ticket);
            notifyLocked(ticket, true);
        }
        return id;
    }

    // `source` defaults to this call, which is what a one-off ticket wants. The
    // chokepoints every caller shares — waitForGit, runGitCapture — forward the
    // source their own caller captured instead, so the footer's outcome line
    // links to the code that asked for the work rather than to the chokepoint.
    static quint64 begin(
        const QString &kind, const QString &detail = QString(),
        ActionTelemetry::Execution execution = ActionTelemetry::Execution::Async,
        const LogSource &source = logSourceHere())
    {
        BackgroundWork work;
        work.kind = kind;
        work.detail = detail;
        work.execution = execution;
        work.source = source;
        return begin(std::move(work));
    }

    static void end(quint64 id,
                    const QString &outcome = QStringLiteral("completed"))
    {
        if (id == 0)
            return;
        BackgroundTicket ticket;
        bool found = false;
        {
            QMutexLocker lock(&state().mutex);
            const auto it = state().tickets.find(id);
            if (it != state().tickets.end()) {
                ticket = *it;
                state().tickets.erase(it);
                found = true;
                ticket.outcome = outcome;
                notifyLocked(ticket, false);
            }
        }
        if (found) {
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            ActionTelemetry::finished(id, ticket.kind, ticket.detail,
                                      ticket.execution, ticket.startedAtMs, now,
                                      outcome, telemetryContext(ticket));
        }
    }

    static void setListener(Listener listener)
    {
        QMutexLocker lock(&state().mutex);
        state().listener = std::move(listener);
        if (!state().listener)
            return;
        for (auto it = state().tickets.cbegin();
             it != state().tickets.cend(); ++it)
            state().listener(it.value(), true);
    }

private:
    struct State {
        QMutex mutex;
        Listener listener;
        QHash<quint64, BackgroundTicket> tickets;
        QAtomicInteger<quint64> nextId = 1;
    };

    static State &state()
    {
        static State s;
        return s;
    }

    static ActionTelemetry::Context telemetryContext(const BackgroundTicket &ticket)
    {
        ActionTelemetry::Context context;
        context.sourceFile = ticket.source.file;
        context.sourceLine = ticket.source.line;
        context.sourceFunction = ticket.source.function;
        context.routine = ticket.routine;
        context.workingDirectory = ticket.workingDirectory;
        context.queuedMs = ticket.queuedMs;
        return context;
    }

    static void notifyLocked(const BackgroundTicket &ticket, bool started)
    {
        if (state().listener)
            state().listener(ticket, started);
    }
};

constexpr qint64 kBackgroundShowAfterMs = 200;

inline QString backgroundOkGlyph() { return QString::fromUtf8("\xE2\x9C\x93"); }
inline QString backgroundNotGlyph() { return QString::fromUtf8("\xE2\x9C\x95"); }

inline QString backgroundElapsedText(qint64 ms)
{
    const qint64 clamped = ms < 0 ? 0 : ms;
    if (clamped < 1000)
        return QStringLiteral("%1ms").arg(clamped);
    return QStringLiteral("%1s").arg(clamped / 1000.0, 0, 'f', 1);
}

// A note carries the whole command line — every pathspec of a `git diff main --
// <30 files>` included — because the point of the footer line is that you can
// paste it into a shell and reproduce the work. The cap is only a backstop
// against a pathological caller (adhoc #1620 saw a several-thousand-character
// entry), not the usual trim.
constexpr int kBackgroundDetailMaxChars = 2000;

inline QString backgroundDetailNote(const QString &detail)
{
    const QString note = detail.simplified();
    if (note.size() <= kBackgroundDetailMaxChars)
        return note;
    return note.left(kBackgroundDetailMaxChars - 1) + QString::fromUtf8("\xE2\x80\xA6");
}

// Why a retired ticket counts as a failure, or empty when it does not. A
// worker-process outcome reads "failed (exit 128)"; the line shows the reason
// alone ("exit 128") because it already says "failed".
inline QString backgroundFailureText(const QString &outcome)
{
    const QString text = outcome.trimmed();
    if (text.isEmpty() || text == QLatin1String("succeeded") ||
        text == QLatin1String("completed") ||
        text.startsWith(QLatin1String("answered")))
        return QString();
    if (text.startsWith(QLatin1String("failed (")) && text.endsWith(QLatin1Char(')')))
        return text.mid(8, text.size() - 9).trimmed();
    if (text == QLatin1String("failed"))
        return QString(); // nothing more to say than the word itself
    return text;
}

inline QString gitProbeAnswerOutcome(const QString &program,
                                     const QStringList &arguments,
                                     int exitCode)
{
    if (exitCode != 1)
        return QString();
    const QString base = program.section(QLatin1Char('/'), -1);
    if (base != QLatin1String("git") && base != QLatin1String("git.exe"))
        return QString();
    int at = 0;
    while (at < arguments.size()) {
        const QString &arg = arguments.at(at);
        if (arg == QLatin1String("-C") || arg == QLatin1String("-c") ||
            arg == QLatin1String("--git-dir") ||
            arg == QLatin1String("--work-tree")) {
            at += 2;
            continue;
        }
        if (arg.startsWith(QLatin1Char('-'))) {
            ++at;
            continue;
        }
        break;
    }
    if (at >= arguments.size())
        return QString();
    const QString command = arguments.at(at);
    const QStringList rest = arguments.mid(at + 1);
    const auto has = [&rest](const char *flag) {
        return rest.contains(QLatin1String(flag));
    };
    const bool probe =
        (command == QLatin1String("merge-base") && has("--is-ancestor")) ||
        (command == QLatin1String("rev-parse") && has("--verify") &&
         (has("--quiet") || has("-q"))) ||
        (command == QLatin1String("show-ref") &&
         (has("--verify") || has("--quiet") || has("-q") || has("--exists"))) ||
        (command == QLatin1String("merge-tree") && has("--write-tree")) ||
        (command == QLatin1String("diff") &&
         (has("--quiet") || has("--exit-code"))) ||
        (command == QLatin1String("symbolic-ref") &&
         (has("--quiet") || has("-q"))) ||
        command == QLatin1String("check-ignore") ||
        (command == QLatin1String("config") &&
         (has("--get") || has("--get-all") || has("--get-regexp")));
    return probe ? QStringLiteral("answered (exit 1)") : QString();
}

// What the outcome line says beyond the work itself (adhoc #186). Every field
// is optional: the line names only what it was told.
struct BackgroundOutcomeContext {
    // Every distinct routine the tally folded in, oldest first: "Git rail
    // status timer (every 10s)", "window activation refresh", …
    QStringList routines;
    QString function;        // the code that asked: "refreshRepoChangeBadge"
    QString repo;            // the checkout the work ran in: "me/r"
    QString agent;           // "#42 running" when that checkout is an agent's worktree
    QString path;            // the directory, shown only when no repo matched
    int failures = 0;        // runs that did not succeed
    QString lastFailure;     // "exit 128", "timed out", "crashed", …
    qint64 longestQueuedMs = -1; // longest wait for an admission slot
};

// A queue wait shorter than this is the normal hand-off to a worker and not
// worth a word on the line.
constexpr qint64 kBackgroundQueuedNoteMs = 100;

inline QString backgroundOutcomeLine(const QString &word, int runs,
                                     qint64 elapsedMs, const QString &detail,
                                     bool backgrounded,
                                     const BackgroundOutcomeContext &context)
{
    QString line = QStringLiteral("Background %1 %2")
                       .arg(backgrounded ? backgroundOkGlyph()
                                         : backgroundNotGlyph(),
                            word.isEmpty() ? QStringLiteral("work") : word);
    if (runs > 1)
        line += QStringLiteral(" %1%2").arg(QChar(0x00D7)).arg(runs);
    line += backgrounded ? QStringLiteral(" backgrounded")
                         : QStringLiteral(" not backgrounded");
    QString timing = (runs > 1 ? QStringLiteral("longest ") : QString()) +
                     backgroundElapsedText(elapsedMs);
    if (context.failures > 0) {
        timing += runs > 1 ? QStringLiteral(", %1 failed").arg(context.failures)
                           : QStringLiteral(", failed");
        if (!context.lastFailure.isEmpty())
            timing += QStringLiteral(": %1").arg(context.lastFailure);
    }
    line += QStringLiteral(" (%1)").arg(timing);
    const QString separator = QStringLiteral(" · ");
    if (!context.routines.isEmpty())
        line += separator + QStringLiteral("routine: ") +
                context.routines.join(QStringLiteral(", "));
    if (!context.function.isEmpty())
        line += separator + QStringLiteral("caller: ") + context.function;
    if (!context.repo.isEmpty())
        line += separator + QStringLiteral("repo: ") + context.repo;
    if (!context.agent.isEmpty())
        line += separator + QStringLiteral("agent: ") + context.agent;
    else if (context.repo.isEmpty() && !context.path.isEmpty())
        line += separator + QStringLiteral("dir: ") + context.path;
    if (context.longestQueuedMs >= kBackgroundQueuedNoteMs)
        line += separator +
                (runs > 1 ? QStringLiteral("longest queued: ")
                          : QStringLiteral("queued: ")) +
                backgroundElapsedText(context.longestQueuedMs);
    const QString note = backgroundDetailNote(detail);
    if (!note.isEmpty())
        line += QStringLiteral(" - ") + note;
    return line;
}

inline QString backgroundOutcomeLine(const QString &word, int runs,
                                     qint64 elapsedMs, const QString &detail,
                                     bool backgrounded)
{
    return backgroundOutcomeLine(word, runs, elapsedMs, detail, backgrounded,
                                 BackgroundOutcomeContext());
}

inline QString colorizeBackgroundMarker(const QString &escapedMessage)
{
    struct Mark {
        QString glyph;
        QLatin1String color;
    };
    const Mark marks[] = {
        {backgroundOkGlyph(), QLatin1String("#3fb950")},
        {backgroundNotGlyph(), QLatin1String("#f85149")},
    };
    QString html = escapedMessage;
    for (const Mark &mark : marks) {
        const int at = html.indexOf(mark.glyph);
        if (at < 0)
            continue;
        html.replace(at, mark.glyph.size(),
                     QStringLiteral("<span style='color:%1; font-weight:700'>%2"
                                    "</span>")
                         .arg(QString(mark.color), mark.glyph));
    }
    return html;
}

class BackgroundScope
{
public:
    explicit BackgroundScope(const QString &kind,
                             const QString &detail = QString(),
                             ActionTelemetry::Execution execution =
                                 ActionTelemetry::Execution::Async,
                             const LogSource &source = logSourceHere())
        : m_id(BackgroundActivity::begin(kind, detail, execution, source))
    {
    }
    explicit BackgroundScope(BackgroundWork work)
        : m_id(BackgroundActivity::begin(std::move(work)))
    {
    }
    ~BackgroundScope() { BackgroundActivity::end(m_id); }
    BackgroundScope(const BackgroundScope &) = delete;
    BackgroundScope &operator=(const BackgroundScope &) = delete;

private:
    quint64 m_id = 0;
};

} // namespace forkmesh
