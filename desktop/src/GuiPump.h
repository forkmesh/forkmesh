#pragma once

#include <QCoreApplication>
#include <QEventLoop>
#include <QList>
#include <QMetaObject>

#include <functional>
#include <utility>

// Re-entrancy state for the GUI-thread keep-alive pump.
// Every synchronous git read on the GUI thread goes through waitForGit(), which
// calls QCoreApplication::processEvents() between polls so the window keeps
// accepting input and painting instead of freezing (see MainWindowInternal.h).
// That pump also
// delivers *queued* work — the results of background threads and detached git
// processes — so a worker that happens to finish while the GUI thread is parked
// in a git wait lands its (often heavy) apply handler nested inside whatever
// render was already running.
// That nesting is how a 500ms pull-list rebuild ended up stacked on top of a
// commit reload on top of a spool sweep for a single 14.6s freeze in the stall
// log: none of the individual passes was pathological, they just ran inside one
// another. The counter below lets those landing points recognise "I am being
// delivered inside a pump" and re-post themselves for the next top-level turn of
// the event loop, which costs one extra event-loop hop and keeps each pass on
// its own stack.
// Lives in its own header (rather than MainWindowInternal.h) so MainWindow.h's
// runOffThread template can consult it without pulling in the whole internal UI
// layer.
namespace forkmesh::ui {

inline int g_keepAlivePumpDepth = 0;
inline QList<std::function<void()>> g_afterKeepAlivePump;
inline bool g_afterKeepAlivePumpPosted = false;

inline bool inKeepAlivePump() { return g_keepAlivePumpDepth > 0; }

// Shared by every GUI-thread Git wait. Keeping this named makes the contract
// testable: a nested wait may defer heavy queued apply callbacks, but it must
// never ask Qt to withhold input/window-system events.
inline QEventLoop::ProcessEventsFlags responsiveWaitEventFlags()
{
    return QEventLoop::AllEvents;
}

inline void postAfterKeepAlivePump();

inline void drainAfterKeepAlivePump()
{
    g_afterKeepAlivePumpPosted = false;
    // A queued drain can itself be delivered by the next nested event pump.
    // Leave the work parked; the outermost scope's destructor will post one
    // fresh drain after that pump has unwound.
    if (inKeepAlivePump() || g_afterKeepAlivePump.isEmpty())
        return;
    // Release one result per top-level turn. A pull load, branch scan and agent
    // diff often finish during the same Git wait; applying that whole burst in
    // one drain merely turns nested stalls into one long sequential stall.
    std::function<void()> work = std::move(g_afterKeepAlivePump.first());
    g_afterKeepAlivePump.removeFirst();
    if (work)
        work();
    if (!g_afterKeepAlivePump.isEmpty())
        postAfterKeepAlivePump();
}

inline void postAfterKeepAlivePump()
{
    if (g_afterKeepAlivePumpPosted || g_afterKeepAlivePump.isEmpty())
        return;
    QCoreApplication *app = QCoreApplication::instance();
    if (!app)
        return;
    g_afterKeepAlivePumpPosted = true;
    QMetaObject::invokeMethod(app, [] { drainAfterKeepAlivePump(); },
                              Qt::QueuedConnection);
}

// Park a heavy GUI apply callback until the nested event loop that encountered
// it has completely unwound.  Returning false means there was no pump and the
// caller should run the work immediately.
inline bool deferUntilAfterKeepAlivePump(std::function<void()> work)
{
    if (!inKeepAlivePump())
        return false;
    g_afterKeepAlivePump.append(std::move(work));
    return true;
}

// RAII depth marker for responsive nested waits. Exception-safe and re-entrant.
struct KeepAlivePumpScope {
    KeepAlivePumpScope() { ++g_keepAlivePumpDepth; }
    ~KeepAlivePumpScope()
    {
        if (--g_keepAlivePumpDepth == 0)
            postAfterKeepAlivePump();
    }
    KeepAlivePumpScope(const KeepAlivePumpScope &) = delete;
    KeepAlivePumpScope &operator=(const KeepAlivePumpScope &) = delete;
};

} // namespace forkmesh::ui
