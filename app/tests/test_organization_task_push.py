#!/usr/bin/env python3
"""A private task board change wakes every member's client, not just the pings.

Private organization tasks are read with a signed GET /api/tasks. The desktop
pulls that catalog once per launch and then only when asked (there is no poll),
so without a push a task another member created is invisible until someone
clicks Refresh. `notify_organization_task_activity` already pings the
Engineering floor; these tests pin the second half of that fan-out — the
payload-free "tasks" topic sent to *every* member of the organization, which is
who can read the board.
"""

import ast
import asyncio
import importlib.util
import re
from pathlib import Path
from types import SimpleNamespace

# MainWindowRepos.cpp was split into feature files; read them in the original
# source order so section slices that span functions still line up.
MAIN_WINDOW_REPOS_SOURCES = (
    "MainWindowRepos.cpp",
    "MainWindowReposInternal.h",
    "MainWindowRepoSettings.cpp",
    "MainWindowRepoSync.cpp",
    "MainWindowMirrorPush.cpp",
    "MainWindowRepoFetch.cpp",
    "MainWindowAccounts.cpp",
)

ROOT = Path(__file__).resolve().parents[1]
ENTRY = ROOT / "src" / "entry.py"
ENTRY_TEXT = ENTRY.read_text(encoding="utf-8")

_catalog_spec = importlib.util.spec_from_file_location(
    "forkmesh_catalog_for_task_push", ROOT / "src" / "catalog.py")
catalog = importlib.util.module_from_spec(_catalog_spec)
_catalog_spec.loader.exec_module(catalog)

DESKTOP = ROOT.parent / "desktop" / "src"


def _function_node(name):
    for node in ast.walk(ast.parse(ENTRY_TEXT, filename=str(ENTRY))):
        if (isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
                and node.name == name):
            return node
    raise AssertionError(name + " not found in entry.py")


def _load_watchers(rows, pushes, calls):
    """`notify_organization_task_watchers` over a fake roster and DO channel."""

    async def d1_all(_env, sql, *params):
        calls.append((sql, params))
        return list(rows)

    async def notify_account_event(_env, owner, topic):
        pushes.append((owner, topic))

    namespace = {
        "d1_all": d1_all,
        "notify_account_event": notify_account_event,
        "clean_string": catalog.clean_string,
        "valid_node_name": _constant_function("valid_node_name"),
        "MAX_NODE_NAME": _constant("MAX_NODE_NAME"),
        "MAX_ORG_MEMBERS": _constant("MAX_ORG_MEMBERS"),
    }
    source = ast.unparse(_function_node("notify_organization_task_watchers"))
    exec(compile(source, str(ENTRY), "exec"), namespace)
    return namespace["notify_organization_task_watchers"]


def _constant(name):
    for node in ast.parse(ENTRY_TEXT, filename=str(ENTRY)).body:
        if (isinstance(node, ast.Assign)
                and any(getattr(t, "id", "") == name for t in node.targets)):
            return ast.literal_eval(node.value)
    raise AssertionError(name + " not found")


def _compiled_pattern(name):
    """The pattern behind `NAME = re.compile(r"...")` in entry.py."""
    for node in ast.parse(ENTRY_TEXT, filename=str(ENTRY)).body:
        if (isinstance(node, ast.Assign)
                and any(getattr(t, "id", "") == name for t in node.targets)
                and isinstance(node.value, ast.Call)):
            return re.compile(ast.literal_eval(node.value.args[0]))
    raise AssertionError(name + " not found")


def _constant_function(name):
    namespace = {"clean_string": catalog.clean_string,
                 "MAX_NODE_NAME": _constant("MAX_NODE_NAME"),
                 "NODE_NAME_RE": _compiled_pattern("NODE_NAME_RE")}
    exec(compile(ast.unparse(_function_node(name)), str(ENTRY), "exec"),
         namespace)
    return namespace[name]


def _run(coro):
    return asyncio.new_event_loop().run_until_complete(coro)


def test_every_member_of_the_org_is_woken_with_the_tasks_topic():
    pushes, calls = [], []
    watchers = _load_watchers(
        [{"name": "ada"}, {"name": "bo"}, {"name": "cy"}], pushes, calls)
    _run(watchers(SimpleNamespace(env=object()), "org:acme"))

    # Board visibility is org membership, so the fan-out is the org roster —
    # not the Engineering floor the activity pings go to.
    assert pushes == [("ada", "tasks"), ("bo", "tasks"), ("cy", "tasks")]
    sql, params = calls[0]
    assert "FROM org_members WHERE org_bi=?" in sql
    assert "role IN ('owner','admin','member')" in sql
    assert params[0] == "org:acme"
    # Bounded like every other roster read on a request path.
    assert params[-1] == _constant("MAX_ORG_MEMBERS")


def test_the_fanout_pushes_once_per_person_and_skips_unusable_names():
    pushes, calls = [], []
    watchers = _load_watchers(
        [{"name": "Ada"}, {"name": "ada"}, {"name": ""},
         {"name": "not a node name"}, {"name": None}, {}],
        pushes, calls)
    _run(watchers(SimpleNamespace(env=object()), "org:acme"))
    assert pushes == [("ada", "tasks")]


def test_an_empty_or_missing_roster_pushes_nothing():
    for rows in ([], None):
        pushes, calls = [], []
        watchers = _load_watchers(rows or [], pushes, calls)
        _run(watchers(SimpleNamespace(env=object()), ""))
        assert pushes == []


def test_task_activity_fans_the_topic_out_after_the_pings():
    notifier = ENTRY_TEXT.split(
        "async def notify_organization_task_activity", 1)[1].split(
            "async def notify_engineering_task_started", 1)[0]
    assert "await self.notify_organization_task_watchers(org_bi)" in notifier
    # The pings come first: a failure to wake a socket must not cost a member
    # the durable notification.
    assert notifier.index("enqueue_notification") < notifier.index(
        "notify_organization_task_watchers")


def test_the_desktop_answers_the_tasks_topic_with_a_board_read():
    repos = "".join(
        (DESKTOP / name).read_text(encoding="utf-8")
        for name in MAIN_WINDOW_REPOS_SOURCES
    )
    bridge = (DESKTOP / "MainWindowTaskBridge.cpp").read_text(encoding="utf-8")
    store = (DESKTOP / "OrganizationTaskStore.cpp").read_text(encoding="utf-8")
    handler = repos.split("&NodeEventSocket::eventReceived", 1)[1].split(
        "&NodeEventSocket::agentStatusResult", 1)[0]
    assert 'topic == QLatin1String("tasks")' in handler
    assert "onOrganizationTaskEvent();" in handler
    # The board is its own signed read, so the topic must not also drag the
    # consolidated /api/sync drain along behind it.
    tasks_branch = handler.split('topic == QLatin1String("tasks")', 1)[1].split(
        "return;", 1)[0]
    assert "scheduleRelaySync" not in tasks_branch

    wake = bridge.split("void MainWindow::onOrganizationTaskEvent()", 1)[1] \
        .split("\n}\n", 1)[0]
    assert "handleEvent();" in wake
    event = store.split("void OrganizationTaskStore::handleEvent()", 1)[1] \
        .split("\n}\n", 1)[0]
    # The store holds the board whether or not the Tasks page is built, so
    # the event re-reads it once and every listener renders the answer.
    assert "refresh();" in event
    # A push that lands while a read is in flight is remembered, because that
    # read may have been issued before the write it is about.
    assert "m_eventPending = true;" in event
    assert store.count("drainPendingEvent(); });") == 1


def test_a_reconnect_rereads_the_board_but_a_first_connect_does_not():
    repos = "".join(
        (DESKTOP / name).read_text(encoding="utf-8")
        for name in MAIN_WINDOW_REPOS_SOURCES
    )
    connected = repos.split("&NodeEventSocket::connectedChanged", 1)[1].split(
        "&NodeEventSocket::systemMessage", 1)[0]
    assert "if (m_nodeEventSocketDropped)" in connected
    assert connected.index("if (m_nodeEventSocketDropped)") < connected.index(
        "m_nodeEventSocketDropped = false;")
    assert "m_nodeEventSocketDropped = true;" in connected


if __name__ == "__main__":
    for name, value in sorted(dict(globals()).items()):
        if name.startswith("test_") and callable(value):
            value()
            print("ok", name)
