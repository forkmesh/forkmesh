#pragma once

#include <QString>

#include <memory>

namespace forkmesh {

// Where an action record came from and why, beyond the work itself. All
// optional: a record carries only the fields that were known. `sourceFile` and
// `sourceFunction` are the string literals a LogSource holds. (A namespace-level
// struct, not a nested one: GCC refuses a nested class with default member
// initializers as a default argument of the enclosing class's methods.)
struct ActionContext {
    const char *sourceFile = nullptr;
    int sourceLine = 0;
    const char *sourceFunction = nullptr;
    QString routine;          // the timer tick / action / agent flow that asked
    QString workingDirectory; // the checkout the work ran in
    qint64 queuedMs = -1;     // wait for an admission slot before starting
};

class ActionTelemetry
{
public:
    enum class Execution {
        Async,      // event-driven network/process work
        Worker,     // blocking work running on a worker thread
        UiDeferred, // bounded GUI-only application, queued between event turns
        UiBlocking, // synchronous work still running on the GUI thread
    };

    using Context = ActionContext;

    static void initialize(const QString &path);
    static void started(quint64 id, const QString &kind, const QString &detail,
                        Execution execution, qint64 startedAtMs,
                        const Context &context = Context());
    static void finished(quint64 id, const QString &kind, const QString &detail,
                         Execution execution, qint64 startedAtMs,
                         qint64 finishedAtMs, const QString &outcome,
                         const Context &context = Context());
    static void instant(const QString &kind, const QString &detail,
                        const QString &outcome = QStringLiteral("observed"));
    static void shutdown();

private:
    struct Writer;
    static std::shared_ptr<Writer> writer();
};

} // namespace forkmesh
