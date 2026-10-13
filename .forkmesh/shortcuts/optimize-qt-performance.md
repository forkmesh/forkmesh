# name: Optimize Qt desktop performance
# description: Profile the Qt 6 Widgets client, cut one measured hot path, and prove the win against the performance budgets.
# agent: claude-code
# model: claude-opus-5

Make the ForkMesh desktop client measurably faster in one focused, reviewable
pass. The client is Qt 6 Widgets under `desktop/` (Widgets, Network, Svg,
Concurrent, optional DBus and Multimedia). There is no QML or Qt Quick in this
repository, so the targets are widgets, item models, delegates, painting, and
the event loop — not bindings or the scene graph.

Optimize how often something happens before optimizing how fast each occurrence
is. Cutting 50,000 `data()` calls or repaints to 5,000 beats making each one 20%
faster, and it is usually the cheaper diff.

1. Read what the app already recorded before you profile anything yourself.
   `~/projects/forkmesh/logs/diagnostics/stalls.log` holds one record per GUI freeze the
   `StallWatchdog` caught, with a sampled backtrace and the `BlockingCallScope`
   breadcrumb naming the blocking call. `~/projects/forkmesh/logs/diagnostics/actions.jsonl`
   is `ActionTelemetry`: every action carries an `Execution` value, and
   `UiBlocking` means that work ran synchronously on the GUI thread. The app log
   (`network_log.txt` in the app-data folder; Settings ▸ Data lists the path)
   carries the `[startup +NNNNms]` markers `desktop/src/StartupTrace.h` emits.
   `desktop/tests/test_perf_budgets.cpp` documents the budgets already agreed
   and what each one does and does not cover. Cite the evidence you found.

2. Establish a reproducible baseline on an optimized binary before editing.
   Configure with `-DCMAKE_BUILD_TYPE=Release`; a Debug measurement is not
   evidence and will send you after the wrong function. Use `tools/measure_startup.py`
   for startup, the `forkmesh-perf-tests` target for the agreed budgets, and a
   CPU profiler such as `perf` for anything else. These budgets are wall-clock
   and load-sensitive, so record the machine load with every number and compare
   only measurements taken under the same load; a timing without its load is
   meaningless. Name the workload and repeat it enough to separate signal from noise.

3. Pick the single highest-cost target the measurements actually support, and
   state the baseline number you intend to move. Do not optimize by intuition,
   and do not touch Qt's implicitly shared classes on the assumption that a copy
   is expensive — measure that assumption first.

4. Fix the cost where it belongs:
   - Keep the GUI thread free. Filesystem, git, network, database, and image
     work belong on a worker (`QThreadPool`, `QtConcurrent`) or an
     asynchronous API. Never pump the event loop to wait, and take anything held
     across a wait by value.
   - Cut notification churn. Emit one `dataChanged()` over the correct range
     instead of hundreds of per-cell signals, and reserve `beginResetModel()`
     for a genuine full reset. Coalesce repeated signals, queued
     `invokeMethod()` calls, and posted events rather than letting thousands of
     cheap ones queue up.
   - Keep `data()`, `rowCount()`, `flags()`, proxy filtering and sorting, and
     delegate `paint()` cheap. Views call them far more often than the code
     reads like they do. No queries, no expensive formatting, no allocation of
     fonts, icons, or regexes per call — hoist and cache those.
   - Repaint only the region that changed, cache expensive static rendering, and
     keep layout and measurement work out of `paintEvent()`.
   - Reduce allocations and copies: pass large objects by `const &`, move
     instead of copy, `reserve()` when the final size is known, and stop
     rebuilding the same temporary `QString`, `QImage`, `QIcon`, or
     `QRegularExpression` in a loop.
   - Replace short-interval `QTimer` polling with event-driven updates, and do
     not refresh the UI more often than a user can perceive.

5. Prove the result. Re-measure the same workload on the same machine under
   comparable load and report before and after. Run the relevant checks —
   `forkmesh-perf-tests`, and `forkmesh-window-tests` for anything touching the
   UI — and add or tighten a budget or regression test so the win cannot
   silently reverse. Confirm behavior is unchanged; a faster app that shows less
   or does less is a behavior change, not an optimization, and needs to be
   called out and decided by the team rather than slipped in.

6. Never make a number look better by widening a performance budget or the
   watchdog threshold, silencing a log, disabling telemetry, skipping a test, or
   dropping work users depend on. If the honest result is that the budget is
   wrong, say so and explain why instead of editing it quietly. If the
   measurements do not support a change, report that and open an issue with the
   evidence rather than committing an unproven one.

Keep the diff narrow and behavior-preserving, inspect it for unrelated changes,
and leave a review-ready branch. Never open a pull request and never merge your own work.

Finish with a concise optimization report: the evidence that chose the target,
the baseline and the workload and machine load behind it, what changed and why,
the after measurement from the same conditions, the verification commands and
their outcomes, any budget or test added, and the next most expensive target the
measurements point at.
