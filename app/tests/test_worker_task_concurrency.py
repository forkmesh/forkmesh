#!/usr/bin/env python3
"""No concurrent asyncio tasks anywhere in the Worker's Python.

The Python Workers runtime (observed on compatibility_date 2026-08-03, Pyodide
0.28.2; not yet shown fixed on the Python 3.14 runtime) can step one
PyodideTask while another task's step is on the stack. The loop then raises

    RuntimeError: Cannot enter into task <PyodideTask pending ...>
                  while another task <PyodideTask pending ...> is running

the offending task is left pending forever, and from that moment EVERY request
the isolate receives dies the same way: a permanently wedged isolate answering
Cloudflare 1101 until workerd recycles it. Live tails during the 2026-08-05/06
(issue #555) and 2026-08-07 outages showed 30-85% of relay traffic failing this
way, spread across whatever routes happened to be busy — /api/repo/*/*/pending,
git info/refs, the world-general room WebSocket, even /health.

So the Worker's Python must never spawn or interleave a second task:

  * ``asyncio.gather(...)``            -> await the pieces in sequence
  * ``asyncio.wait_for(js_promise)``   -> ``js_fetch_with_timeout``, or a
                                          native ``AbortSignal.timeout`` on
                                          the fetch init
  * ``asyncio.create_task`` /
    ``asyncio.ensure_future``          -> just await it

The sequential form costs a few milliseconds of added latency per request; the
concurrent form costs every request that isolate would have served.
"""
import ast
from pathlib import Path

SRC = Path(__file__).resolve().parents[1] / "src"

# Every construct that puts a second PyodideTask in flight.
BANNED_CALLS = {
    "gather",
    "wait_for",
    "create_task",
    "ensure_future",
    "wait",
    "as_completed",
    "shield",
    "run_coroutine_threadsafe",
}


def _asyncio_calls(path):
    """(line, attribute) for every ``asyncio.<name>(...)`` call in a module."""
    tree = ast.parse(path.read_text(encoding="utf-8"), filename=str(path))
    found = []
    for node in ast.walk(tree):
        if not isinstance(node, ast.Call):
            continue
        func = node.func
        if not isinstance(func, ast.Attribute):
            continue
        value = func.value
        if isinstance(value, ast.Name) and value.id == "asyncio":
            found.append((node.lineno, func.attr))
    return found


def test_no_concurrent_asyncio_tasks_in_worker_python():
    offenders = []
    for path in sorted(SRC.glob("*.py")):
        for lineno, attr in _asyncio_calls(path):
            if attr in BANNED_CALLS:
                offenders.append("%s:%d asyncio.%s" % (path.name, lineno, attr))
    assert not offenders, (
        "These spawn or interleave a second Pyodide task, which wedges the "
        "isolate for every later request (see this module's docstring). Await "
        "sequentially, or bound a fetch with js_fetch_with_timeout instead:\n"
        + "\n".join(offenders)
    )


def test_entry_documents_the_rule_for_the_next_author():
    entry = (SRC / "entry.py").read_text(encoding="utf-8")
    header = entry[:entry.index('asyncio = _LazyModule("asyncio")')]
    for marker in (
        "No concurrent asyncio tasks on a request path",
        "Cannot enter into task",
        "js_fetch_with_timeout",
        "test_worker_task_concurrency.py",
    ):
        assert marker in header, marker


def test_timeouts_on_the_mirror_paths_use_the_native_abort_signal():
    entry = (SRC / "entry.py").read_text(encoding="utf-8")
    # js_fetch_with_timeout is the only bounded-fetch helper: it attaches a
    # Workers-native AbortSignal instead of cancelling a Python await.
    assert "JsAbortSignal.timeout(" in entry
    for name in (
        "_https_mirror_private_proxy",
        "_https_mirror_merge_proxy",
        "_https_mirror_actions_proxy",
    ):
        start = entry.index("async def " + name)
        end = entry.index("\nasync def ", start + 1)
        body = entry[start:end]
        assert "js_fetch_with_timeout(" in body, name


def test_node_event_pushes_are_awaited_directly():
    entry = (SRC / "entry.py").read_text(encoding="utf-8")
    for name in ("notify_repo_host", "notify_account_event"):
        start = entry.index("async def " + name)
        end = entry.index("\nasync def ", start + 1)
        body = entry[start:end]
        assert "await node_object.fetch(" in body, name
        assert "asyncio" not in body.replace(
            "asyncio.wait_for: cancelling", "").replace(
            "asyncio.wait_for.", ""), name


def test_leaderboard_sources_are_read_one_at_a_time():
    entry = (SRC / "entry.py").read_text(encoding="utf-8")
    start = entry.index("async def leaderboards_overview")
    end = entry.index("\nasync def ", start + 1)
    body = entry[start:end]
    # Late-bound thunks: a source that is never reached must not be left as an
    # orphan coroutine, and each is awaited in its own turn.
    assert '("users", lambda: _account_users_directory(env, None))' in body
    assert "result = await source()" in body
    # Per-source isolation survives the rewrite: one broken board must not 500
    # the hub (adhoc #225).
    assert "degraded.append(name)" in body


def test_live_host_probes_are_serialized_under_a_wall_clock_budget():
    entry = (SRC / "entry.py").read_text(encoding="utf-8")
    assert "HYDRATE_PROBE_BUDGET_MS" in entry
    start = entry.index("async def hydrate_repo_group_live_hosts")
    end = entry.index("\nasync def ", start + 1)
    body = entry[start:end]
    # Sequential probes make the count cap useless as a latency bound, so the
    # pass must stop on its own clock and let the memo carry the rest.
    assert "hosts = await repo_live_host_count(" in body
    assert "probe_deadline" in body
