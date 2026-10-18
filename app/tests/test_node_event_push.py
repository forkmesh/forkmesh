#!/usr/bin/env python3
"""ForkMeshNodes — the per-owner node event push channel.

notify_repo_host fans a payload-free {"type":"event","topic"} frame out to the
owner's connected desktop/headless nodes the moment a web submission lands;
each node runs the matching direct Git or signed inbox reconciliation. Relay
event pushes stay payload free: they never carry repository bytes or inbox
payloads. The latest commit fingerprint is retained for reconnect replay, and
a notify failure never fails the write that triggered it. Replies to signed
node writes return only to the socket that originated them.

Node -> relay carries keepalives plus the key-signed writes named in
NODE_FRAME_ACCEPTS — the batched agent-status report and periodic account
heartbeat a desktop would otherwise POST while already holding this socket.
Those are transport only: each frame carries the same proof its HTTPS route
demands and goes to the same handler, so holding the socket grants nothing.
"""

import ast
import asyncio
import json
import re
import tomllib
from pathlib import Path
from types import SimpleNamespace
from urllib.parse import parse_qs, quote, unquote, urlparse
import sys
from _maintenance_stubs import with_maintenance_off


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
CATALOG = ROOT / "src" / "catalog.py"
CATALOG_ROUTES = ROOT / "src" / "catalog_routes.py"
WRANGLER = ROOT / "wrangler.toml"
ENTRY_TEXT = ((
    # entry.py + its lazily-split domain modules
    (ENTRY.parent / "node_events_runtime.py").read_text(encoding="utf-8")
    + "\n\n\n"
    + ENTRY.read_text(encoding="utf-8")
    + "\n\n\n"
) + "\n"
              + CATALOG.read_text(encoding="utf-8") + "\n"
              + CATALOG_ROUTES.read_text(encoding="utf-8"))

NAMES = {
    "notify_repo_host",
    "notify_repo_mirrors",
    "_load_node_commit_events",
    "_stored_commit_generation",
    "remember_node_commit_event",
    "replay_node_commit_events",
    "_node_events_authorization",
    "_node_events_watch_object",
    "_node_events_object",
    "_repo_watch_do_name",
    "notify_repo_watchers",
    "_node_events_object_holds_node",
    "node_events_handler",
    "_node_events_do_name",
    "ForkMeshNodes",
    "safe_segment",
    "clean_string",
    "_ws_attachment",
    "_ws_attr",
    "durable_object_traffic_note",
    "durable_object_traffic_flush",
    "js_nullish",
}
CONSTANTS = {
    "DURABLE_OBJECT_BINDING_RE",
    "DURABLE_OBJECT_TRAFFIC_FLUSH_MS",
    "DURABLE_OBJECT_TRAFFIC_FLUSH_BYTES",
    "DURABLE_OBJECT_TRAFFIC_MAX",
    "NODE_EVENT_MAX_SOCKETS",
    "NODE_WATCH_MAX_SOCKETS",
    "NODE_SOCKET_STALE_MS",
    "NODE_EVENT_MSG_WINDOW_MS",
    "NODE_EVENT_MSG_MAX_PER_WINDOW",
    "NODE_EVENT_MAX_FRAME_BYTES",
    "NODE_FRAME_ACCEPTS",
    "NODE_FRAME_RELAYS",
    "NODE_CONTROL_MSG_WINDOW_MS",
    "NODE_CONTROL_MSG_MAX_PER_WINDOW",
    "NODE_COMMIT_REPLAY_STORAGE_KEY",
    "NODE_COMMIT_REPLAY_MAX",
    "CATALOG_MAX_RECORDS_PER_OWNER",
    "MAX_NODE_NAME",
    # The gate the DO's synthetic request has to satisfy for its proof to be
    # checked at all; pulled from entry.py so the two cannot drift apart.
    "ORG_TASK_AGENT_STATUS_BATCH_RE",
    "HOST_COUNT_TIMEOUT_MS",
    "EXPECTED_DEGRADED_HEADERS",
    "EXPECTED_DEGRADED_HEADER",
    # safe_segment's dependencies from catalog.py
    "MAX_REPO_SEGMENT",
    "ROOM_NAME_RE",
}

NOW = 1_000_000_000


def _load(extra_globals):
    tree = ast.parse(ENTRY_TEXT, filename=str(ENTRY))
    selected = []
    found = set()
    for node in tree.body:
        if (isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef,
                              ast.ClassDef))
                and node.name in NAMES):
            selected.append(node)
            found.add(node.name)
        elif isinstance(node, ast.Assign) and any(
                isinstance(target, ast.Name) and target.id in CONSTANTS
                for target in node.targets):
            selected.append(node)
    assert found == NAMES, "missing: %s" % sorted(NAMES - found)
    module = ast.fix_missing_locations(
        ast.Module(body=selected, type_ignores=[]))
    namespace = dict(extra_globals)
    exec(compile(module, str(ENTRY), "exec"), with_maintenance_off(namespace))
    return namespace


def _source_of(name):
    for node in ast.parse(ENTRY_TEXT, filename=str(ENTRY)).body:
        if (isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef,
                              ast.ClassDef)) and node.name == name):
            return ast.unparse(node)
    raise AssertionError("%s not found in entry.py" % name)


class _Headers:
    def __init__(self, values=None):
        self.values = {str(k).lower(): v for k, v in (values or {}).items()}

    def get(self, name):
        return self.values.get(str(name).lower())


class _Request:
    def __init__(self, url, headers=None):
        self.url = url
        self.headers = _Headers(headers)


class _Sock:
    def __init__(self, attachment=None, fail_send=False):
        self.attachment = attachment
        self.fail_send = fail_send
        self.sent = []
        self.closed = None

    def serializeAttachment(self, value):
        self.attachment = value

    def deserializeAttachment(self):
        return self.attachment

    def send(self, frame):
        if self.fail_send:
            raise RuntimeError("socket gone")
        self.sent.append(frame)

    def close(self, code=1000, reason=""):
        self.closed = (code, reason)


class _Ctx:
    def __init__(self):
        self.tagged = []  # (tags, socket)
        self.storage = _Storage()

    def acceptWebSocket(self, server, tags):
        self.tagged.append((list(tags), server))

    def getWebSockets(self, tag=None):
        return [sock for tags, sock in self.tagged
                if tag is None or tag in tags]


class _Storage:
    def __init__(self):
        self.values = {}

    async def get(self, key):
        return self.values.get(key)

    async def put(self, key, value):
        self.values[key] = value


def _to_js(value, **_kwargs):
    if isinstance(value, dict):
        return SimpleNamespace(**value)
    return value


class _JsResponse:
    @staticmethod
    def new(_body, opts):
        return {"status": getattr(opts, "status", None),
                "webSocket": getattr(opts, "webSocket", None)}


class _WebSocketPair:
    @staticmethod
    def new():
        pair = SimpleNamespace(client=_Sock(), server=_Sock())

        class _Values:
            def object_values(self):
                return pair.client, pair.server

        return _Values()


class DurableObject:
    def __init__(self, ctx=None, env=None):
        self.ctx = ctx
        self.env = env


def _json_response(data, status=200, cache_seconds=None, cache_control=None,
                   extra_headers=None):
    return {"status": status, "data": data,
            "headers": dict(extra_headers or {})}


def _agent_live_relay_module():
    source = str(Path(__file__).resolve().parents[1] / "src")
    if source not in sys.path:
        sys.path.insert(0, source)
    import agent_live_relay
    return agent_live_relay


def _base_globals(traffic_writes=None):
    socket_ids = iter(range(1, 100))

    async def d1_run(_env, sql, *args):
        if traffic_writes is not None:
            traffic_writes.append((sql, args))
        return None

    return {
        "d1_run": d1_run,
        "asyncio": asyncio,
        "json": json,
        "_agent_live_relay": _agent_live_relay_module(),
        "re": re,
        "parse_qs": parse_qs,
        "quote": quote,
        "unquote": unquote,
        "urlparse": urlparse,
        "Date": SimpleNamespace(now=staticmethod(lambda: NOW)),
        "_cron_subrequest_charge": lambda _count=1: None,
        "to_js": _to_js,
        "json_response": _json_response,
        "new_socket_id": lambda: "sock-%d" % next(socket_ids),
        "JsResponse": _JsResponse,
        "WebSocketPair": _WebSocketPair,
        "DurableObject": DurableObject,
    }


def _nodes_binding(idFromName_calls, fetch_calls, fetch_error=None,
                   fetch_result=None):
    class _Stub:
        async def fetch(self, url):
            fetch_calls.append(url)
            if fetch_error is not None:
                raise fetch_error
            return fetch_result if fetch_result is not None else {"ok": True}

    binding = SimpleNamespace()
    binding.idFromName = lambda name: (idFromName_calls.append(name)
                                       or "id:" + name)
    binding.get = lambda _id: _Stub()
    return binding


# --- notify_repo_host --------------------------------------------------------

def test_notify_pushes_one_payload_free_frame_to_the_owner_channel():
    ids, fetches = [], []
    env = SimpleNamespace(FORKMESH_NODES=_nodes_binding(ids, fetches))
    ns = _load(_base_globals())
    asyncio.run(ns["notify_repo_host"](env, "Alice", "widgets", "issues"))
    assert ids == ["nodes:alice"]
    assert len(fetches) == 1
    url = urlparse(fetches[0])
    assert url.path == "/api/nodes/notify"
    query = parse_qs(url.query)
    assert query["topic"] == ["issues"]
    assert query["repo"] == ["Alice/widgets"]


def test_commit_notify_carries_only_a_valid_target_refs_digest():
    ids, fetches = [], []
    env = SimpleNamespace(FORKMESH_NODES=_nodes_binding(ids, fetches))
    ns = _load(_base_globals())
    state = "a" * 64
    asyncio.run(ns["notify_repo_host"](
        env, "Alice", "widgets", "commits", state.upper(),
        generation=1234))
    query = parse_qs(urlparse(fetches[-1]).query)
    assert query["state"] == [state]
    assert query["generation"] == ["1234"]

    asyncio.run(ns["notify_repo_host"](
        env, "Alice", "widgets", "commits", "not-a-digest"))
    assert "state" not in parse_qs(urlparse(fetches[-1]).query)


def test_notify_failure_never_escapes_and_bad_segments_are_dropped():
    ids, fetches = [], []
    env = SimpleNamespace(FORKMESH_NODES=_nodes_binding(
        ids, fetches, fetch_error=RuntimeError("DO unavailable")))
    ns = _load(_base_globals())
    # A dead Durable Object must not fail the issue/PR write that notified.
    asyncio.run(ns["notify_repo_host"](env, "alice", "widgets", "issues"))
    assert ids == ["nodes:alice"]
    # Invalid segments never even select a Durable Object.
    ids.clear()
    asyncio.run(ns["notify_repo_host"](env, "", "widgets", "issues"))
    asyncio.run(ns["notify_repo_host"](env, "alice", "../etc", "issues"))
    assert ids == []


def test_mirror_notifications_reach_machine_and_linked_owner_channels():
    notified = []

    async def context(_env, owner, repo):
        assert (owner, repo) == ("source", "widgets")
        return {"nodes": {"mirror2", "mirror3"}}

    async def d1_all(_env, sql, *args):
        assert "LEFT JOIN users" in sql
        assert set(args) == {"mirror2", "mirror3"}
        return [
            {"node_name": "mirror2", "owner": "jett"},
            {"node_name": "mirror3", "owner": ""},
        ]

    async def notify(
            _env, owner, repo, topic, event_owner="", generation=0):
        notified.append((owner, repo, topic, event_owner))

    ns = _load({
        **_base_globals(),
        "_https_mirror_public_context": context,
        "d1_all": d1_all,
        "notify_repo_host": notify,
        "valid_node_name": lambda value: bool(
            re.fullmatch(r"[a-z][a-z0-9-]{0,62}", str(value or ""))),
    })
    # Replace the loaded notifier so notify_repo_mirrors observes the spy.
    ns["notify_repo_mirrors"].__globals__["notify_repo_host"] = notify
    asyncio.run(ns["notify_repo_mirrors"](
        object(), "source", "widgets", "issues"))
    assert notified == [
        ("jett", "widgets", "issues", "source"),
        ("mirror2", "widgets", "issues", "source"),
        ("mirror3", "widgets", "issues", "source"),
    ]


def test_commit_notifications_wake_lagging_group_members_with_target_state():
    notified = []
    state = "b" * 64

    async def context(_env, _owner, _repo):
        # mirror3 is outside the serving allowlist because it is behind. It
        # must still receive the event that tells it which generation to seek.
        return {
            "nodes": {"mirror2"},
            "groupNodes": {"mirror2", "mirror3"},
        }

    async def d1_all(_env, _sql, *_args):
        return []

    async def notify(
            _env, owner, repo, topic, target_state="", event_owner="",
            generation=0):
        notified.append(
            (owner, repo, topic, target_state, event_owner, generation))

    ns = _load({
        **_base_globals(),
        "_https_mirror_public_context": context,
        "d1_all": d1_all,
        "notify_repo_host": notify,
        "valid_node_name": lambda value: bool(
            re.fullmatch(r"[a-z][a-z0-9-]{0,62}", str(value or ""))),
    })
    ns["notify_repo_mirrors"].__globals__["notify_repo_host"] = notify
    asyncio.run(ns["notify_repo_mirrors"](
        object(), "source", "widgets", "commits", state))
    assert notified == [
        ("mirror2", "widgets", "commits", state, "source", 0),
        ("mirror3", "widgets", "commits", state, "source", 0),
    ]


def _core_wake_targets(source_owner, group, cores):
    notified = []

    async def context(_env, _owner, _repo):
        return {"nodes": set(group), "groupNodes": set(group)}

    async def d1_all(_env, _sql, *_args):
        return []

    async def notify(
            _env, owner, repo, topic, target_state="", event_owner="",
            generation=0):
        notified.append(owner)

    ns = _load({
        **_base_globals(),
        "_https_mirror_public_context": context,
        "d1_all": d1_all,
        "notify_repo_host": notify,
        "hosted_core_nodes": lambda _env: list(cores),
        "valid_node_name": lambda value: bool(
            re.fullmatch(r"[a-z][a-z0-9-]{0,62}", str(value or ""))),
    })
    ns["notify_repo_mirrors"].__globals__["notify_repo_host"] = notify
    asyncio.run(ns["notify_repo_mirrors"](
        object(), source_owner, "forkmesh", "commits", "d" * 64, 7))
    return notified


def test_a_merge_on_any_core_wakes_every_core_including_the_source_core():
    cores = ("fmnext-n1", "fmnext-n2", "fmnext-n3")
    assert _core_wake_targets(
        "fmnext-n1", {"fmnext-n2", "fmnext-n3"}, cores) == list(cores)
    assert _core_wake_targets("fmnext-n1", set(), cores) == list(cores)


def test_a_non_core_source_is_still_not_woken_and_adds_no_cores():
    cores = ("fmnext-n1", "fmnext-n2", "fmnext-n3")
    assert _core_wake_targets(
        "source", {"source", "mirror2"}, cores) == ["mirror2"]
    assert _core_wake_targets(
        "fmnext-n1", {"fmnext-n1", "mirror2"}, ()) == ["mirror2"]


def _admit_core_push_state_with(record, hosted=None):
    tree = ast.parse(ENTRY.read_text(encoding="utf-8"), filename=str(ENTRY))
    node = next(
        item for item in tree.body
        if isinstance(item, ast.AsyncFunctionDef)
        and item.name == "_admit_core_push_state")
    wakes = []

    async def context(_env, owner, repo):
        return {"owner": owner, "repo": repo, "repoBi": "bi"}

    async def schedule(_env, owner, repo, topic, state="", generation=0):
        wakes.append((owner, repo, topic, state, generation))
        return True

    async def notify(*_args, **_kwargs):
        raise AssertionError("the doorbell accepted the wake")

    namespace = {
        "_https_mirror_public_context": context,
        "_record_core_push_state": record,
        "schedule_repo_mirror_wake": schedule,
        "notify_repo_mirrors": notify,
        "Date": SimpleNamespace(now=lambda: NOW),
    }
    if hosted is not None:
        namespace["record_hosted_push_state"] = hosted
    exec(compile(ast.fix_missing_locations(
        ast.Module(body=[node], type_ignores=[])), str(ENTRY), "exec"),
        namespace)
    state = asyncio.run(namespace["_admit_core_push_state"](
        object(), "fmnext-n1", "forkmesh", {"node": "fmnext-n2"}))
    return state, wakes


def test_a_core_push_wakes_the_cores_even_when_its_state_is_not_admitted():
    async def admitted(_env, _context, _endpoint, _alias):
        return "e" * 64

    async def unverified(_env, _context, _endpoint, _alias):
        return ""

    async def failing(_env, _context, _endpoint, _alias):
        raise RuntimeError("d1 down")

    assert _admit_core_push_state_with(admitted) == (
        "e" * 64, [("fmnext-n1", "forkmesh", "commits", "e" * 64, NOW)])
    for record in (unverified, failing):
        assert _admit_core_push_state_with(record) == (
            "", [("fmnext-n1", "forkmesh", "commits", "", 0)])


def test_an_admitted_core_push_moves_the_hosted_catalog_head():
    pinned = []

    async def admitted(_env, _context, _endpoint, _alias):
        return "e" * 64

    async def unverified(_env, _context, _endpoint, _alias):
        return ""

    async def hosted(_env, owner, repo, endpoint, state="", notify=True):
        pinned.append((owner, repo, endpoint["node"], state, notify))
        return state

    assert _admit_core_push_state_with(admitted, hosted)[0] == "e" * 64
    assert pinned == [("fmnext-n1", "forkmesh", "fmnext-n2", "e" * 64, False)]
    pinned.clear()
    assert _admit_core_push_state_with(unverified, hosted)[0] == ""
    assert pinned == []

    async def broken(*_args, **_kwargs):
        raise RuntimeError("catalog unavailable")

    assert _admit_core_push_state_with(admitted, broken) == (
        "e" * 64, [("fmnext-n1", "forkmesh", "commits", "e" * 64, NOW)])


def test_merge_paths_wake_the_cores_whether_or_not_the_state_was_admitted():
    agent_branches = (ENTRY.parent / "agent_branches.py").read_text(
        encoding="utf-8")
    route_handlers = (ENTRY.parent / "route_handlers.py").read_text(
        encoding="utf-8")
    assert "    if admitted:\n        generation" not in agent_branches
    assert "generation = int(Date.now()) if admitted else 0" in agent_branches
    assert "if admitted_state:\n            generation" not in route_handlers
    assert ("generation = int(Date.now()) if admitted_state else 0"
            in route_handlers)


def test_mirror_event_is_addressed_to_target_but_names_source_repository():
    ids, fetches = [], []
    env = SimpleNamespace(FORKMESH_NODES=_nodes_binding(ids, fetches))
    ns = _load(_base_globals())
    asyncio.run(ns["notify_repo_host"](
        env, "jett", "widgets", "commits", "c" * 64,
        event_owner="source"))
    assert ids == ["nodes:jett"]
    query = parse_qs(urlparse(fetches[0]).query)
    assert query["repo"] == ["source/widgets"]
    assert query["state"] == ["c" * 64]


def test_write_paths_stay_wired_to_the_event_push():
    # The push is only instant if the submission handlers keep calling it.
    entry_only = (
        ENTRY.read_text(encoding="utf-8")
        + "\n\n\n"
        + (ENTRY.parent / "agent_routes.py").read_text(encoding="utf-8")
    )
    calls = entry_only.count("notify_repo_host(env, owner, repo,")
    assert calls >= 6, "web submission paths no longer notify the owner node"
    for topic in ("issues", "pulls", "discussions", "agents", "about"):
        assert 'notify_repo_host(env, owner, repo, "%s")' % topic in entry_only
    # Freshly pushed source refs have no per-owner submission handler; they
    # reach the fleet through the mirror fan-out on the catalog publish path.
    # The full refs digest is the trigger so branch and tag pushes wake every
    # mirror even when the advertised default head stays put.
    catalog_routes = CATALOG_ROUTES.read_text(encoding="utf-8")
    assert "scheduled = await schedule_repo_mirror_wake(" in catalog_routes
    assert 'env, owner, record["name"], "commits", state_hash,' in \
        catalog_routes
    assert "catalog_mirror_update_needed(" in catalog_routes
    assert "current_state != str(prior_state_hash" in catalog_routes


def test_issue_mirror_wake_is_durable_but_off_the_acceptance_path():
    entry = ENTRY.read_text(encoding="utf-8")
    node_runtime = (
        ENTRY.parent / "node_events_runtime.py"
    ).read_text(encoding="utf-8")
    issue_start = entry.index("async def issues_handler")
    issue_end = entry.index("\nasync def ", issue_start + 1)
    issue_handler = entry[issue_start:issue_end]
    scheduler_start = node_runtime.index(
        "async def schedule_repo_mirror_wake")
    scheduler = node_runtime[scheduler_start:]
    # The source still gets the immediate nudge, but the potentially large
    # mirror group is only represented by one persistent DO doorbell request.
    assert 'await notify_repo_host(env, owner, repo, "issues")' in issue_handler
    assert 'await schedule_repo_mirror_wake(env, owner, repo, "issues")' in issue_handler
    assert 'await notify_repo_mirrors(env, owner, repo, "issues")' not in issue_handler
    assert "MIRROR_WAKE_PATH" in scheduler
    assert "MIRROR_WAKE_NAME" in scheduler
    assert "doorbell.fetch(request)" in scheduler
    assert "async def _mirror_wake_alarm" in entry
    assert "await notify_repo_mirrors(" in entry


def test_event_pushes_carry_no_repo_data_and_writes_grant_no_authority():
    # This transport never reads/decrypts repository data or forwards a public
    # request body. Node writes are authorized by their proof, never by the
    # fact that they arrived on a connected socket.
    for name in ("ForkMeshNodes", "notify_repo_host", "node_events_handler"):
        source = _source_of(name)
        assert "d1_" not in source
        assert "decrypt" not in source
        assert "request.bytes" not in source
        assert "request.text" not in source
        assert "request.json" not in source
    nodes = _source_of("ForkMeshNodes")
    assert "'type': 'event'" in nodes
    # The write path re-signs nothing and invents no identity: it forwards the
    # node's own node/ts/sig triple to the shared task handler, which runs the
    # batch proof check and the per-task ownership gates exactly as on HTTPS.
    for token in ("frame.get('node')", "frame.get('ts')", "frame.get('sig')"):
        assert token in nodes
    assert "organization_tasks_handler" in nodes
    assert "'/api/tasks/agent-status'" in nodes
    for token in ("frame.get('nodeName')", "_account_heartbeat"):
        assert token in nodes
    assert "api/accounts/heartbeat" in nodes


# --- /api/nodes/events routing ----------------------------------------------

def _events_request(upgrade="websocket"):
    headers = {"upgrade": upgrade} if upgrade else {}
    return _Request(
        "https://forkmesh.com/api/nodes/events?owner=Alice&ts=1&sig=s",
        headers)


def test_events_route_requires_websocket_upgrade():
    ns = _load({**_base_globals(),
                "_authorize_owner": None})  # must not be reached
    result = asyncio.run(ns["node_events_handler"](
        SimpleNamespace(), _events_request(upgrade=None)))
    assert result["status"] == 426


def test_events_route_rejects_unsigned_upgrades_before_do_selection():
    ids, fetches = [], []
    env = SimpleNamespace(FORKMESH_NODES=_nodes_binding(ids, fetches))

    async def deny(_env, _request, _owner):
        return False

    ns = _load({**_base_globals(), "_authorize_owner": deny})
    result = asyncio.run(ns["node_events_handler"](env, _events_request()))
    assert result["status"] == 401
    assert ids == [] and fetches == []


def test_events_route_forwards_authorized_upgrades_to_the_owner_do():
    ids, fetches = [], []
    env = SimpleNamespace(FORKMESH_NODES=_nodes_binding(
        ids, fetches, fetch_result={"status": 101}))
    seen = []

    async def allow(_env, _request, owner):
        seen.append(owner)
        return True

    async def durable_object_request(request):
        return request

    ns = _load({**_base_globals(),
                "_authorize_owner": allow,
                "durable_object_request": durable_object_request})
    result = asyncio.run(ns["node_events_handler"](env, _events_request()))
    assert result == {"status": 101}
    assert seen == ["Alice"]
    assert ids == ["nodes:alice"]
    assert len(fetches) == 1


def test_events_route_retries_then_answers_a_retryable_503():
    ids, fetches, logged = [], [], []
    env = SimpleNamespace(FORKMESH_NODES=_nodes_binding(
        ids, fetches, fetch_error=RuntimeError("aborted")))

    async def allow(_env, _request, _owner):
        return True

    async def durable_object_request(request):
        return request

    async def log_durable_object_abort(_env, _request, path, error):
        logged.append((path, str(error)))

    ns = _load({**_base_globals(),
                "_authorize_owner": allow,
                "durable_object_request": durable_object_request,
                "log_durable_object_abort": log_durable_object_abort})
    result = asyncio.run(ns["node_events_handler"](env, _events_request()))
    assert result["status"] == 503
    assert len(fetches) == 2  # one retry against a fresh stub
    assert logged == [("/api/nodes/events", "aborted")]
    assert result["headers"] == ns["EXPECTED_DEGRADED_HEADERS"]


# --- which object a streamed sign-in terminal joins -------------------------
#
# The two node implementations disagree about which per-owner object they sit
# on, and the terminal relay can only forward between sockets of ONE object. A
# desktop signs its drain token as the account and lands on nodes:<account>
# while publishing a machine name of its own; a managed mirror is its own node
# account and lands on nodes:<mirror>. So a browser that always joined its own
# account's object reached desktops and never mirrors -- which is exactly the
# "<node> did not answer" the sign-in dialog reported (issue #681).


def _ticket_request(query):
    return _Request(
        "https://forkmesh.com/api/nodes/events?" + query,
        {"upgrade": "websocket"})


def _probe_binding(names, agents):
    """FORKMESH_NODES whose objects answer the node-side socket snapshot."""
    class _Response:
        def __init__(self, payload):
            self.payload = payload

        async def json(self):
            return self.payload

    class _Object:
        def __init__(self, name):
            self.name = name

        async def fetch(self, _url):
            return _Response({"ok": True, "nodes": 9,
                              "agents": agents.get(self.name, 0)})

    binding = SimpleNamespace()
    binding.idFromName = lambda name: (names.append(name) or name)
    binding.get = lambda name: _Object(name)
    return binding


def _ticket_globals(owner, owned):
    def ticket_owner(_env, ticket):
        return owner if ticket == "good" else ""

    async def owns(_env, actor, node):
        return (actor, node) in owned

    return {**_base_globals(),
            "_account_event_ticket_owner": ticket_owner,
            "_account_owns_node": owns,
            "_authorize_owner": None}  # a ticket never signs an owner token


def test_terminal_browser_joins_the_object_its_machine_is_attached_to():
    names, agents = [], {"nodes:mirror35": 1}
    env = SimpleNamespace(FORKMESH_NODES=_probe_binding(names, agents))
    ns = _load(_ticket_globals("jett", {("jett", "mirror35")}))
    owner, rejection = asyncio.run(ns["_node_events_authorization"](
        env, _ticket_request("ticket=good&node=mirror35")))
    assert rejection is None
    # The managed mirror holds its socket under its own node account, so that
    # is the object this tab has to be accepted onto.
    assert owner == "mirror35"
    assert names == ["nodes:mirror35"]


def test_terminal_browser_stays_on_the_account_object_for_a_desktop_node():
    # A desktop's machine name is a label, not the identity its socket
    # authenticated as: nothing is attached to nodes:<machine>, and the node
    # is on the account's own object. Falling back is what keeps the desktop
    # sign-in working.
    names, agents = [], {}
    env = SimpleNamespace(FORKMESH_NODES=_probe_binding(names, agents))
    ns = _load(_ticket_globals("jett", {("jett", "workshop")}))
    owner, rejection = asyncio.run(ns["_node_events_authorization"](
        env, _ticket_request("ticket=good&node=workshop")))
    assert (owner, rejection) == ("jett", None)
    assert names == ["nodes:workshop"]


def test_terminal_browser_may_not_join_a_node_it_does_not_own():
    names, agents = [], {"nodes:mirror35": 1}
    env = SimpleNamespace(FORKMESH_NODES=_probe_binding(names, agents))
    ns = _load(_ticket_globals("mallory", set()))
    owner, rejection = asyncio.run(ns["_node_events_authorization"](
        env, _ticket_request("ticket=good&node=mirror35")))
    assert owner == ""
    assert rejection["status"] == 403
    # Refused before any object is selected, so an unowned name cannot even be
    # used to probe who is online.
    assert names == []


def test_the_account_channel_keeps_its_own_object_and_probes_nothing():
    names, agents = [], {"nodes:mirror35": 1}
    env = SimpleNamespace(FORKMESH_NODES=_probe_binding(names, agents))
    ns = _load(_ticket_globals("jett", {("jett", "mirror35")}))
    # No ?node=: every other consumer of this socket (unread counts, session
    # revocation) is unchanged and costs no extra subrequest.
    owner, rejection = asyncio.run(ns["_node_events_authorization"](
        env, _ticket_request("ticket=good")))
    assert (owner, rejection) == ("jett", None)
    # Naming yourself is the same thing as naming nothing.
    owner, rejection = asyncio.run(ns["_node_events_authorization"](
        env, _ticket_request("ticket=good&node=Jett")))
    assert (owner, rejection) == ("jett", None)
    assert names == []


def test_an_unreadable_probe_leaves_the_browser_on_the_account_object():
    class _Broken:
        async def fetch(self, _url):
            raise RuntimeError("DO unavailable")

    binding = SimpleNamespace()
    binding.idFromName = lambda name: name
    binding.get = lambda _name: _Broken()
    env = SimpleNamespace(FORKMESH_NODES=binding)
    ns = _load(_ticket_globals("jett", {("jett", "mirror35")}))
    owner, rejection = asyncio.run(ns["_node_events_authorization"](
        env, _ticket_request("ticket=good&node=mirror35")))
    assert (owner, rejection) == ("jett", None)


def test_a_signed_node_upgrade_ignores_a_node_parameter_entirely():
    names = []
    env = SimpleNamespace(FORKMESH_NODES=_probe_binding(names, {}))
    seen = []

    async def allow(_env, _request, owner):
        seen.append(owner)
        return True

    ns = _load({**_base_globals(), "_authorize_owner": allow})
    owner, rejection = asyncio.run(ns["_node_events_authorization"](
        env, _ticket_request("owner=Alice&ts=1&sig=s&node=mirror35")))
    # The node half proves an owner for itself; the redirect is a browser
    # affordance and must not move a machine off its own object.
    assert (owner, rejection) == ("Alice", None)
    assert seen == ["Alice"] and names == []


# --- ForkMeshNodes Durable Object -------------------------------------------

def _make_do(ns):
    do = ns["ForkMeshNodes"](_Ctx(), SimpleNamespace())
    return do


def test_the_object_snapshot_counts_the_node_side_apart_from_browsers():
    ns = _load(_base_globals())
    do = _make_do(ns)
    for attachment in (
        SimpleNamespace(id="node", last=NOW, role="node"),
        SimpleNamespace(id="old", last=NOW),  # pre-role socket reads as node
        SimpleNamespace(id="tab", last=NOW, role="browser"),
        SimpleNamespace(id="gone", last=NOW - ns["NODE_SOCKET_STALE_MS"] - 1),
    ):
        do.ctx.tagged.append((["node"], _Sock(attachment)))
    result = asyncio.run(do.fetch(_Request(
        "https://forkmesh.internal/api/nodes/state")))
    # `nodes` stays the role-blind total it has always been; `agents` is what
    # answers "is the machine itself here?" without a browser tab standing in
    # for it.
    assert result["data"] == {"ok": True, "nodes": 3, "agents": 2}


def test_do_notify_fans_out_to_live_sockets_and_reaps_stale_ones():
    ns = _load(_base_globals())
    do = _make_do(ns)
    live = _Sock(SimpleNamespace(id="a", last=NOW))
    dead_send = _Sock(SimpleNamespace(id="b", last=NOW), fail_send=True)
    stale = _Sock(SimpleNamespace(
        id="c", last=NOW - ns["NODE_SOCKET_STALE_MS"] - 1))
    for sock in (live, dead_send, stale):
        do.ctx.tagged.append((["node"], sock))
    result = asyncio.run(do.fetch(_Request(
        "https://forkmesh.internal/api/nodes/notify"
        "?topic=issues&repo=alice/widgets")))
    assert result["data"] == {"ok": True, "delivered": 1}
    frame = json.loads(live.sent[0])
    assert frame == {"type": "event", "topic": "issues",
                     "repo": "alice/widgets"}
    assert stale.closed == (1001, "stale")
    assert stale.sent == []


def test_do_commit_frame_preserves_the_target_refs_digest():
    ns = _load(_base_globals())
    do = _make_do(ns)
    live = _Sock(SimpleNamespace(id="a", last=NOW))
    do.ctx.tagged.append((["node"], live))
    state = "c" * 64
    asyncio.run(do.fetch(_Request(
        "https://forkmesh.internal/api/nodes/notify"
        "?topic=commits&repo=alice/widgets&state=" + state
        + "&generation=1234")))
    assert json.loads(live.sent[0]) == {
        "type": "event", "topic": "commits",
        "repo": "alice/widgets", "state": state, "generation": 1234,
    }
    stored = json.loads(do.ctx.storage.values[
        ns["NODE_COMMIT_REPLAY_STORAGE_KEY"]])
    assert stored == {
        "alice/widgets": {
            "repo": "alice/widgets", "state": state, "generation": 1234,
        },
    }


def test_do_replays_the_latest_commit_generation_when_a_node_reconnects():
    ns = _load(_base_globals())
    do = _make_do(ns)
    live = _Sock(SimpleNamespace(id="existing", last=NOW))
    do.ctx.tagged.append((["node"], live))
    old_state, new_state = "a" * 64, "b" * 64
    for state, generation in ((old_state, 100), (new_state, 200),
                              (old_state, 150)):
        response = asyncio.run(do.fetch(_Request(
            "https://forkmesh.internal/api/nodes/notify"
            f"?topic=commits&repo=alice/widgets&state={state}"
            f"&generation={generation}")))
        assert response["status"] == 200

    result = asyncio.run(do.fetch(_Request(
        "https://forkmesh.com/api/nodes/events?owner=alice&ts=1&sig=s",
        {"upgrade": "websocket"})))
    assert result["status"] == 101
    assert [json.loads(frame)["generation"] for frame in live.sent] == [100, 200]
    accepted = do.ctx.tagged[-1][1]
    assert json.loads(accepted.sent[1]) == {
        "type": "event", "topic": "commits", "repo": "alice/widgets",
        "state": new_state, "generation": 200,
    }


def test_do_accepts_node_sockets_and_enforces_the_cap():
    ns = _load(_base_globals())
    do = _make_do(ns)
    result = asyncio.run(do.fetch(_Request(
        "https://forkmesh.com/api/nodes/events?owner=alice&ts=1&sig=s",
        {"upgrade": "websocket"})))
    assert result["status"] == 101
    assert result["webSocket"] is not None
    (tags, accepted), = do.ctx.tagged
    assert tags == ["node"]
    assert accepted.attachment.id and accepted.attachment.last == NOW
    # A signed upgrade is the node half of the account's stream. The role is
    # recorded on the attachment because it is what the terminal relay
    # addresses by, and it is echoed in the hello so a node can tell which
    # side the relay believes it to be.
    assert accepted.attachment.role == "node"
    assert json.loads(accepted.sent[0]) == {
        "type": "hello",
        "accepts": ["agent-status", "heartbeat", "endpoint"],
        "relays": ["terminal", "control"],
        "role": "node",
        "controlBudget": {
            "windowMs": ns["NODE_CONTROL_MSG_WINDOW_MS"],
            "max": ns["NODE_CONTROL_MSG_MAX_PER_WINDOW"],
        },
    }
    for _ in range(ns["NODE_EVENT_MAX_SOCKETS"]):
        do.ctx.tagged.append(
            (["node"], _Sock(SimpleNamespace(id="x", last=NOW))))
    full = asyncio.run(do.fetch(_Request(
        "https://forkmesh.com/api/nodes/events?owner=alice&ts=1&sig=s",
        {"upgrade": "websocket"})))
    assert full["status"] == 429


def test_do_accepts_a_ticketed_tab_as_the_browser_half_of_the_stream():
    # Same object, opposite role: how the account was proven is the only thing
    # that decides which side of a terminal stream a socket is on.
    ns = _load(_base_globals())
    do = _make_do(ns)
    result = asyncio.run(do.fetch(_Request(
        "https://forkmesh.com/api/nodes/events?ticket=t&node=mirror35",
        {"upgrade": "websocket"})))
    assert result["status"] == 101
    (_tags, accepted), = do.ctx.tagged
    assert accepted.attachment.role == "browser"
    assert json.loads(accepted.sent[0])["role"] == "browser"


def test_do_ping_refreshes_staleness_clock_and_answers_pong():
    ns = _load(_base_globals())
    do = _make_do(ns)
    ws = _Sock(SimpleNamespace(id="a", last=NOW - 60_000))
    asyncio.run(do.webSocketMessage(ws, json.dumps({"type": "ping"})))
    assert json.loads(ws.sent[0]) == {"type": "pong"}
    assert ws.attachment.last == NOW
    assert ws.closed is None
    # Non-text frames and oversized frames close the socket.
    binary = _Sock(SimpleNamespace(id="b", last=NOW))
    asyncio.run(do.webSocketMessage(binary, b"\x00"))
    assert binary.closed == (1003, "text frames only")
    huge = _Sock(SimpleNamespace(id="c", last=NOW))
    asyncio.run(do.webSocketMessage(
        huge, "x" * (ns["NODE_EVENT_MAX_FRAME_BYTES"] + 1)))
    assert huge.closed == (1009, "message too large")


def test_do_rate_limits_chatty_sockets_without_closing_them():
    ns = _load(_base_globals())
    do = _make_do(ns)
    ws = _Sock(SimpleNamespace(id="a", last=NOW))
    for _ in range(ns["NODE_EVENT_MSG_MAX_PER_WINDOW"] + 5):
        asyncio.run(do.webSocketMessage(ws, json.dumps({"type": "ping"})))
    assert len(ws.sent) == ns["NODE_EVENT_MSG_MAX_PER_WINDOW"]
    assert ws.closed is None


# --- the signed account heartbeat carried on the socket ---------------------

def _heartbeat_globals(calls, status=200, payload=None, raises=None):
    """Capture what the DO hands the existing account heartbeat handler."""

    class _Response:
        def __init__(self):
            self.status = status

        async def json(self):
            return payload if payload is not None else {
                "ok": True, "online": True}

    async def _account_heartbeat(_env, request):
        calls.append(request)
        if raises is not None:
            raise raises
        return _Response()

    class _JsRequest:
        @staticmethod
        def new(url, init):
            return SimpleNamespace(
                url=url,
                method=getattr(init, "method", None),
                body=getattr(init, "body", None),
                headers=getattr(init, "headers", None),
            )

    return {
        **_base_globals(),
        "_account_heartbeat": _account_heartbeat,
        "JsRequest": _JsRequest,
    }


def _heartbeat_frame(**overrides):
    frame = {
        "type": "heartbeat",
        "nodeName": "Alice",
        "solana": "PublicPayoutAddress",
        "ts": "1786072265687",
        "sig": "j4wIWDJLjNhC",
        "notificationPreferences": {"mention": True},
        "mirrorRequestsAck": ["request-1"],
    }
    frame.update(overrides)
    return json.dumps(frame)


def test_signed_heartbeat_frame_uses_the_same_handler_as_the_https_post():
    calls = []
    response = {
        "ok": True, "online": True, "isAdmin": True,
        "mirrorRequests": [{"id": "request-2", "owner": "bob",
                            "repo": "widgets"}],
    }
    ns = _load(_heartbeat_globals(calls, payload=response))
    do = _make_do(ns)
    ws = _Sock(SimpleNamespace(id="a", last=NOW))
    asyncio.run(do.webSocketMessage(ws, _heartbeat_frame()))

    (request,) = calls
    assert urlparse(request.url).path == "/api/accounts/heartbeat"
    assert request.method == "POST"
    body = json.loads(request.body)
    assert "type" not in body
    assert body == {
        "nodeName": "Alice",
        "solana": "PublicPayoutAddress",
        "ts": "1786072265687",
        "sig": "j4wIWDJLjNhC",
        "notificationPreferences": {"mention": True},
        "mirrorRequestsAck": ["request-1"],
    }
    assert json.loads(ws.sent[0]) == {
        "type": "heartbeat-result",
        "requestId": "1786072265687",
        "status": 200,
        "response": response,
    }


def test_heartbeat_frame_without_a_complete_proof_is_dropped():
    for missing in ("nodeName", "ts", "sig"):
        calls = []
        ns = _load(_heartbeat_globals(calls))
        do = _make_do(ns)
        ws = _Sock(SimpleNamespace(id="a", last=NOW))
        asyncio.run(do.webSocketMessage(
            ws, _heartbeat_frame(**{missing: ""})))
        assert calls == []
        assert ws.sent == []


def test_credit_notification_heartbeat_stays_on_http_transport():
    calls = []
    ns = _load(_heartbeat_globals(calls))
    do = _make_do(ns)
    ws = _Sock(SimpleNamespace(id="a", last=NOW))
    asyncio.run(do.webSocketMessage(
        ws, _heartbeat_frame(creditsRefilled="5h")))
    assert calls == []
    assert json.loads(ws.sent[0]) == {
        "type": "heartbeat-result",
        "requestId": "1786072265687",
        "status": 409,
        "response": {"error": "http_transport_required"},
    }


def test_refused_heartbeat_answers_with_http_equivalent_status():
    cases = (
        (_heartbeat_globals([], status=401,
                            payload={"error": "bad_signature"}),
         401, {"error": "bad_signature"}),
        (_heartbeat_globals([], raises=RuntimeError("handler exploded")),
         503, {"error": "unavailable"}),
    )
    for globals_, status, response in cases:
        ns = _load(globals_)
        do = _make_do(ns)
        ws = _Sock(SimpleNamespace(id="a", last=NOW))
        asyncio.run(do.webSocketMessage(ws, _heartbeat_frame()))
        assert json.loads(ws.sent[0]) == {
            "type": "heartbeat-result",
            "requestId": "1786072265687",
            "status": status,
            "response": response,
        }


def test_rate_limited_heartbeat_is_refused_without_running_its_handler():
    calls = []
    ns = _load(_heartbeat_globals(calls))
    do = _make_do(ns)
    ws = _Sock(SimpleNamespace(id="a", last=NOW))
    budget = ns["NODE_EVENT_MSG_MAX_PER_WINDOW"]
    for _ in range(budget + 1):
        asyncio.run(do.webSocketMessage(ws, _heartbeat_frame()))
    assert len(calls) == budget
    assert json.loads(ws.sent[-1]) == {
        "type": "heartbeat-result",
        "requestId": "1786072265687",
        "status": 429,
        "response": {"error": "rate_limited"},
    }


# --- the signed mirror lease carried on the same socket ---------------------

def _endpoint_globals(calls, status=201, payload=None, raises=None):
    class _Response:
        def __init__(self):
            self.status = status

        async def json(self):
            return payload if payload is not None else {
                "ok": True, "node": "mirror9",
                "baseUrl": "https://mirror9.example", "health": "active",
            }

    async def https_mirror_endpoint_handler(_env, request):
        calls.append(request)
        if raises is not None:
            raise raises
        return _Response()

    class _JsRequest:
        @staticmethod
        def new(url, init):
            return SimpleNamespace(
                url=url, method=getattr(init, "method", None),
                body=getattr(init, "body", None),
                headers=getattr(init, "headers", None),
            )

    return {
        **_base_globals(),
        "https_mirror_endpoint_handler": https_mirror_endpoint_handler,
        "JsRequest": _JsRequest,
    }


def _endpoint_frame(**overrides):
    frame = {
        "type": "endpoint", "node": "mirror9",
        "baseUrl": "https://mirror9.example",
        "publicKey": "public-key", "issuedAt": 1786072265687,
        "signature": "signature",
    }
    frame.update(overrides)
    return json.dumps(frame)


def test_signed_endpoint_lease_uses_the_existing_https_handler():
    calls = []
    ns = _load(_endpoint_globals(calls))
    do = _make_do(ns)
    ws = _Sock(SimpleNamespace(id="a", last=NOW))
    asyncio.run(do.webSocketMessage(ws, _endpoint_frame()))

    (request,) = calls
    assert urlparse(request.url).path == "/api/mirrors/https"
    assert request.method == "POST"
    assert json.loads(request.body) == {
        "node": "mirror9", "baseUrl": "https://mirror9.example",
        "publicKey": "public-key", "issuedAt": 1786072265687,
        "signature": "signature",
    }
    assert json.loads(ws.sent[0]) == {
        "type": "endpoint-result", "requestId": "1786072265687",
        "status": 201,
        "response": {
            "ok": True, "node": "mirror9",
            "baseUrl": "https://mirror9.example", "health": "active",
        },
    }


def test_refused_endpoint_lease_is_answered_without_an_http_retry_hint():
    ns = _load(_endpoint_globals(
        [], status=401, payload={"error": "unauthorized"}))
    do = _make_do(ns)
    ws = _Sock(SimpleNamespace(id="a", last=NOW))
    asyncio.run(do.webSocketMessage(ws, _endpoint_frame()))
    assert json.loads(ws.sent[0]) == {
        "type": "endpoint-result", "requestId": "1786072265687",
        "status": 401, "response": {"error": "unauthorized"},
    }


def test_rate_limited_endpoint_lease_is_refused_without_running_its_handler():
    calls = []
    ns = _load(_endpoint_globals(calls))
    do = _make_do(ns)
    ws = _Sock(SimpleNamespace(id="a", last=NOW))
    budget = ns["NODE_EVENT_MSG_MAX_PER_WINDOW"]
    for _ in range(budget + 1):
        asyncio.run(do.webSocketMessage(ws, _endpoint_frame()))
    assert len(calls) == budget
    assert json.loads(ws.sent[-1]) == {
        "type": "endpoint-result",
        "requestId": "1786072265687",
        "status": 429,
        "response": {"error": "rate_limited"},
    }


# --- the batched agent-status write carried on the socket --------------------

def _tasks_globals(calls, status=200, payload=None, raises=None):
    """Capture what the DO hands the shared task handler."""

    class _Response:
        def __init__(self):
            self.status = status

        async def json(self):
            return payload if payload is not None else {"ok": True}

    async def organization_tasks_handler(_env, request, path):
        calls.append((request, path))
        if raises is not None:
            raise raises
        return _Response()

    class _JsRequest:
        @staticmethod
        def new(url, init):
            return SimpleNamespace(
                url=url,
                method=getattr(init, "method", None),
                body=getattr(init, "body", None),
                headers=getattr(init, "headers", None),
            )

    return {
        **_base_globals(),
        "organization_tasks_handler": organization_tasks_handler,
        "JsRequest": _JsRequest,
    }


def _status_frame(**overrides):
    frame = {
        "type": "agent-status",
        "node": "Alice",
        "ts": "1786072265687",
        "sig": "j4wIWDJLjNhC",
        "statuses": [
            {"task": "a" * 32, "status": "running"},
            {"task": "b" * 32, "status": "success"},
        ],
    }
    frame.update(overrides)
    return json.dumps(frame)


def test_signed_status_frame_goes_to_the_same_handler_as_the_https_write():
    # The whole point of moving this write onto the socket: no request is
    # made, and no authority is invented. The node's own proof rides through
    # to the shared task handler, which re-checks it exactly as on HTTPS.
    calls = []
    ns = _load(_tasks_globals(calls, payload={
        "ok": True,
        "results": [{"task": "a" * 32, "ok": True, "error": ""},
                    {"task": "b" * 32, "ok": False, "error": "task_not_found"}],
    }))
    do = _make_do(ns)
    ws = _Sock(SimpleNamespace(id="a", last=NOW))
    asyncio.run(do.webSocketMessage(ws, _status_frame()))

    (request, path), = calls
    assert path == "/api/tasks/agent-status"
    assert request.method == "POST"
    url = urlparse(request.url)
    # The batch-proof gate in _org_task_signed_session matches on the path, so
    # a synthetic request the real regex misses would fall through to "no
    # session" and silently write nothing.
    assert ns["ORG_TASK_AGENT_STATUS_BATCH_RE"].match(url.path)
    # Lower-cased like the HTTPS gate does before verifying the signature.
    assert parse_qs(url.query) == {
        "node": ["alice"], "ts": ["1786072265687"], "sig": ["j4wIWDJLjNhC"]}
    assert json.loads(request.body) == {"statuses": [
        {"task": "a" * 32, "status": "running"},
        {"task": "b" * 32, "status": "success"},
    ]}
    # Per-entry outcomes go back so one deleted row cannot strand the rest.
    assert json.loads(ws.sent[0]) == {
        "type": "agent-status-result",
        "ok": True,
        "results": [{"task": "a" * 32, "ok": True, "error": ""},
                    {"task": "b" * 32, "ok": False, "error": "task_not_found"}],
    }


def test_status_frame_without_a_complete_proof_is_dropped():
    # Being connected is not a credential: a frame missing any part of the
    # triple never reaches the task handler at all.
    for missing in ("node", "ts", "sig"):
        calls = []
        ns = _load(_tasks_globals(calls))
        do = _make_do(ns)
        ws = _Sock(SimpleNamespace(id="a", last=NOW))
        asyncio.run(do.webSocketMessage(ws, _status_frame(**{missing: ""})))
        assert calls == []
        assert ws.sent == []
    # So is a frame whose statuses are not a list.
    calls = []
    ns = _load(_tasks_globals(calls))
    do = _make_do(ns)
    ws = _Sock(SimpleNamespace(id="a", last=NOW))
    asyncio.run(do.webSocketMessage(ws, _status_frame(statuses="all")))
    assert calls == []
    assert ws.sent == []


def test_refused_status_write_still_answers_so_the_node_can_retry():
    # The node marks these states published optimistically. An unanswered
    # frame would leave runs showing a stale state on the board until
    # something else changed them, so a refusal must come back as one.
    for globals_ in (
        _tasks_globals([], status=401, payload={"error": "invalid_session"}),
        _tasks_globals([], raises=RuntimeError("handler exploded")),
    ):
        ns = _load(globals_)
        do = _make_do(ns)
        ws = _Sock(SimpleNamespace(id="a", last=NOW))
        asyncio.run(do.webSocketMessage(ws, _status_frame()))
        assert json.loads(ws.sent[0]) == {
            "type": "agent-status-result", "ok": False, "results": []}


def test_rate_limited_status_frames_do_no_work_but_are_still_refused():
    # A node that floods writes gets the same budget as one that floods pings.
    # Over budget the write never reaches the task handler — but it is still
    # answered, because silence would leave the node believing it landed.
    calls = []
    budget = _load(_base_globals())["NODE_EVENT_MSG_MAX_PER_WINDOW"]
    ns = _load(_tasks_globals(calls))
    do = _make_do(ns)
    ws = _Sock(SimpleNamespace(id="a", last=NOW))
    for _ in range(budget + 5):
        asyncio.run(do.webSocketMessage(ws, _status_frame()))
    assert len(calls) == budget
    assert len(ws.sent) == budget + 5
    assert json.loads(ws.sent[-1]) == {
        "type": "agent-status-result", "ok": False, "results": []}
    assert ws.closed is None


def test_rate_limited_keepalives_are_still_just_dropped():
    # The refusal is for writes only: a node must never see a verdict for a
    # frame it did not send, or it would retire a batch that is still pending.
    ns = _load(_base_globals())
    do = _make_do(ns)
    ws = _Sock(SimpleNamespace(id="a", last=NOW))
    for _ in range(ns["NODE_EVENT_MSG_MAX_PER_WINDOW"] + 5):
        asyncio.run(do.webSocketMessage(ws, json.dumps({"type": "ping"})))
    assert {json.loads(frame)["type"] for frame in ws.sent} == {"pong"}


def test_frame_ceiling_fits_a_full_fleet_report():
    # A desktop publishes one entry per live session, up to the task API's
    # MAX_AGENT_STATUS_BATCH of 200, and each entry carries the run's
    # provenance as well as its state. The socket's frame cap has to hold that
    # much or a fleet-sized node would silently fall back to HTTPS for the one
    # burst this whole change exists to keep off HTTPS.
    ns = _load(_base_globals())
    module = (ROOT.parent / "world" / "src" / "world_office_tasks.py").read_text(
        encoding="utf-8")
    assert "MAX_AGENT_STATUS_BATCH = 200" in module
    full = _status_frame(statuses=[
        {
            "task": "%032x" % index,
            "status": "running",
            "agent": {
                "provider": "claude-code", "startedBy": "forkbot-longish",
                "model": "claude-opus-5-20260101", "mode": "agent",
                "strength": "high", "sessionId": "%d" % index,
            },
        }
        for index in range(200)
    ])
    assert len(full.encode("utf-8")) < ns["NODE_EVENT_MAX_FRAME_BYTES"]
    # ...and the cap stays inside the two-byte extended frame length the Qt
    # client writes (NodeEventSocket::sendTextFrame).
    assert ns["NODE_EVENT_MAX_FRAME_BYTES"] < 65_536
    assert "constexpr int kMaxOutboundPayload = 60 * 1024;" in (
        ROOT.parent / "desktop" / "src" / "NodeEventSocket.cpp"
    ).read_text(encoding="utf-8")


def test_qt_heartbeat_prefers_the_socket_and_keeps_http_recovery():
    desktop = ROOT.parent / "desktop" / "src"
    setup = (desktop / "MainWindowSetup.cpp").read_text(encoding="utf-8")
    socket_cpp = (desktop / "NodeEventSocket.cpp").read_text(encoding="utf-8")
    socket_h = (desktop / "NodeEventSocket.h").read_text(encoding="utf-8")
    repos = "".join(
        (desktop / name).read_text(encoding="utf-8")
        for name in MAIN_WINDOW_REPOS_SOURCES
    )

    heartbeat = setup[
        setup.index("void MainWindow::sendNodeHeartbeat()"):
        setup.index("void MainWindow::showNodeClaimCode")
    ]
    frame_at = heartbeat.index('QStringLiteral("heartbeat")')
    fallback_at = heartbeat.index('accountsApiUrl("heartbeat")')
    assert frame_at < fallback_at
    assert "m_nodeEventSocket->sendSignedFrame(frame)" in heartbeat
    assert "creditsRefilled.isEmpty()" in heartbeat
    assert 'QLatin1String("http_transport_required")' in heartbeat
    assert "postNodeHeartbeatFallback();" in heartbeat
    assert "kNodeHeartbeatAckTimeoutMs" in heartbeat
    # Optional one-shot fields are retired only after a successful verdict;
    # a lost socket frame can therefore be replayed safely through HTTPS.
    assert "m_pendingMirrorRequestAcks.clear()" not in heartbeat
    assert "m_pendingMirrorRequestAcks.removeAll(id)" in heartbeat
    assert "finishNodeHeartbeatDelivery();" in heartbeat

    assert "void heartbeatResult(" in socket_h
    assert 'QLatin1String("heartbeat-result")' in socket_cpp
    assert "&NodeEventSocket::heartbeatResult" in repos
    assert "&MainWindow::onNodeHeartbeatFrame" in repos
    assert "retryNodeHeartbeatOverHttp();" in repos


def test_qt_commit_events_sync_the_named_repository_without_relay_polling():
    desktop = ROOT.parent / "desktop" / "src"
    repos = "".join(
        (desktop / name).read_text(encoding="utf-8")
        for name in MAIN_WINDOW_REPOS_SOURCES
    )
    socket_cpp = (desktop / "NodeEventSocket.cpp").read_text(encoding="utf-8")
    socket_h = (desktop / "NodeEventSocket.h").read_text(encoding="utf-8")

    assert "const QString &state" in socket_h
    assert 'frame.value(QStringLiteral("state")).toString()' in socket_cpp
    handler = repos[
        repos.index("&NodeEventSocket::eventReceived"):
        repos.index("&NodeEventSocket::agentStatusResult")
    ]
    commits = handler[handler.index('topic == QLatin1String("commits")'):]
    event_sync = repos[
        repos.index("void MainWindow::drainNodeCommitEvent"):
        repos.index("void MainWindow::stopNodeEventSocket")
    ]
    assert "eventRepo.trimmed().toLower()" in commits
    assert "m_pendingMirrorEventStates.insert" in commits
    assert "m_mirrorEventRetryScheduled" in event_sync
    assert "mirrorStateHash(candidate.mirrorPath) == state" in event_sync
    assert "syncRepository(index, /*quiet=*/true);" in event_sync
    assert commits.index("return;") < commits.rindex("scheduleRelaySync();")


# --- Deployment contract -----------------------------------------------------

def test_wrangler_registers_the_nodes_do_in_both_environments():
    config = tomllib.loads(WRANGLER.read_text(encoding="utf-8"))
    prod = {binding["name"]: binding["class_name"]
            for binding in config["durable_objects"]["bindings"]}
    dev = {binding["name"]: binding["class_name"]
           for binding in config["env"]["dev"]["durable_objects"]["bindings"]}
    assert prod["FORKMESH_NODES"] == "ForkMeshNodes"
    assert dev["FORKMESH_NODES"] == "ForkMeshNodes"
    migrated = [migration for migration in config["migrations"]
                if "ForkMeshNodes" in migration.get("new_sqlite_classes", [])]
    assert len(migrated) == 1
    # ForkMeshHost (the retired byte tunnel) must stay deleted; the event
    # channel is a different class and never re-registers it.
    assert not any("ForkMeshHost" in migration.get("new_sqlite_classes", [])
                   for migration in config["migrations"][9:])


# --- the owner control relay (phone <-> desktop, sealed end to end) ---------

def _control_globals():
    import sys
    src = str(ROOT / "src")
    if src not in sys.path:
        sys.path.insert(0, src)
    import node_control
    import node_terminal
    globals_ = _base_globals()
    globals_["normalize_control_frame"] = node_control.normalize_control_frame
    globals_["terminal_rate_state"] = node_terminal.terminal_rate_state
    return globals_


def _control_frame(**overrides):
    frame = {
        "type": "control", "action": "data", "session": "a" * 24,
        "node": "desk", "seq": 3, "data": "c2VhbGVk",
    }
    frame.update(overrides)
    return frame


def test_do_hands_a_phone_control_frame_to_the_node_side_only():
    ns = _load(_control_globals())
    do = _make_do(ns)
    phone = _Sock(SimpleNamespace(id="phone", last=NOW, role="browser"))
    other_tab = _Sock(SimpleNamespace(id="tab", last=NOW, role="browser"))
    desktop = _Sock(SimpleNamespace(id="desk", last=NOW, role="node"))
    for sock in (phone, other_tab, desktop):
        do.ctx.tagged.append((["node"], sock))
    asyncio.run(do.webSocketMessage(
        phone, json.dumps(_control_frame(extra="dropped"))))
    assert [json.loads(frame) for frame in desktop.sent] == [{
        "type": "control", "action": "data", "session": "a" * 24,
        "node": "desk", "from": "browser", "seq": 3, "data": "c2VhbGVk",
        "more": False,
    }]
    assert phone.sent == [] and other_tab.sent == []
    # The desktop's answer goes back to the browser half and never to nodes.
    asyncio.run(do.webSocketMessage(desktop, json.dumps(_control_frame())))
    assert json.loads(phone.sent[0])["from"] == "node"
    assert json.loads(other_tab.sent[0])["from"] == "node"
    assert len(desktop.sent) == 1


def test_do_meters_control_frames_on_their_own_window():
    ns = _load(_control_globals())
    do = _make_do(ns)
    phone = _Sock(SimpleNamespace(id="phone", last=NOW, role="browser"))
    desktop = _Sock(SimpleNamespace(id="desk", last=NOW, role="node"))
    do.ctx.tagged.extend([(["node"], phone), (["node"], desktop)])
    limit = ns["NODE_CONTROL_MSG_MAX_PER_WINDOW"]
    for _ in range(limit + 7):
        asyncio.run(do.webSocketMessage(phone, json.dumps(_control_frame())))
    assert len(desktop.sent) == limit
    assert phone.closed is None
    # The event budget is untouched: a keepalive still gets its pong.
    asyncio.run(do.webSocketMessage(phone, json.dumps({"type": "ping"})))
    assert json.loads(phone.sent[-1]) == {"type": "pong"}


def test_do_drops_a_malformed_control_frame():
    ns = _load(_control_globals())
    do = _make_do(ns)
    phone = _Sock(SimpleNamespace(id="phone", last=NOW, role="browser"))
    desktop = _Sock(SimpleNamespace(id="desk", last=NOW, role="node"))
    do.ctx.tagged.extend([(["node"], phone), (["node"], desktop)])
    for bad in (_control_frame(session="short"),
                _control_frame(action="error"),  # a phone cannot refuse
                _control_frame(data="not base64!")):
        asyncio.run(do.webSocketMessage(phone, json.dumps(bad)))
    assert desktop.sent == []


def _watch_request(target="forkmesh/forkmesh", upgrade="websocket"):
    headers = {"upgrade": upgrade} if upgrade else {}
    return _Request(
        "https://forkmesh.com/api/nodes/events?watch=" + quote(target, safe=""),
        headers)


def _watch_globals(public=True):
    async def context(_env, owner, repo):
        return {"owner": owner, "repo": repo} if public else None

    async def org_repo_node(_env, owner, repo):
        return "forkmesh" if (owner, repo) == ("acme", "forkmesh") else ""

    async def durable_object_request(request):
        return request

    async def deny(*_args):
        raise AssertionError("a watch socket never presents an owner token")

    return {
        **_base_globals(),
        "_https_mirror_public_context": context,
        "_org_repo_node": org_repo_node,
        "durable_object_request": durable_object_request,
        "_authorize_owner": deny,
        "valid_node_name": lambda value: bool(
            re.fullmatch(r"[a-z][a-z0-9-]{0,62}", str(value or ""))),
    }


def test_public_upstream_watch_joins_the_repository_object_unsigned():
    ids, fetches = [], []
    env = SimpleNamespace(FORKMESH_NODES=_nodes_binding(
        ids, fetches, fetch_result={"status": 101}))
    ns = _load(_watch_globals())
    result = asyncio.run(ns["node_events_handler"](
        env, _watch_request("ForkMesh/ForkMesh")))
    assert result == {"status": 101}
    assert ids == ["watch:forkmesh/forkmesh"]
    ids.clear()
    asyncio.run(ns["node_events_handler"](env, _watch_request("acme/forkmesh")))
    assert ids == ["watch:forkmesh/forkmesh"]


def test_upstream_watch_refuses_private_malformed_and_plain_requests():
    ids, fetches = [], []
    env = SimpleNamespace(FORKMESH_NODES=_nodes_binding(ids, fetches))
    hidden = _load(_watch_globals(public=False))
    assert asyncio.run(hidden["node_events_handler"](
        env, _watch_request()))["status"] == 404
    ns = _load(_watch_globals())
    for target in ("forkmesh", "/forkmesh", "-x/forkmesh", "a/b/c"):
        assert asyncio.run(ns["node_events_handler"](
            env, _watch_request(target)))["status"] in (400, 404)
    assert asyncio.run(ns["node_events_handler"](
        env, _watch_request(upgrade=None)))["status"] == 426
    assert ids == [] and fetches == []


def test_watch_sockets_get_no_write_capabilities_and_only_pong():
    ns = _load(_base_globals())
    do = _make_do(ns)
    result = asyncio.run(do.fetch(_Request(
        "https://forkmesh.com/api/nodes/events?watch=forkmesh%2Fforkmesh",
        {"upgrade": "websocket"})))
    assert result["status"] == 101
    (_tags, accepted), = do.ctx.tagged
    assert accepted.attachment.role == "watch"
    hello = json.loads(accepted.sent[0])
    assert hello["accepts"] == [] and hello["relays"] == []
    assert hello["role"] == "watch"
    for frame in ({"type": "heartbeat", "ts": "1"},
                  {"type": "endpoint", "issuedAt": "1"},
                  {"type": "terminal", "action": "input"},
                  {"type": "control"}):
        accepted.sent.clear()
        asyncio.run(do.webSocketMessage(accepted, json.dumps(frame)))
        assert accepted.sent == []
    asyncio.run(do.webSocketMessage(accepted, json.dumps({"type": "ping"})))
    assert json.loads(accepted.sent[-1]) == {"type": "pong"}
    for _ in range(ns["NODE_EVENT_MAX_SOCKETS"]):
        do.ctx.tagged.append(
            (["node"], _Sock(SimpleNamespace(id="w", last=NOW, role="watch"))))
    more = asyncio.run(do.fetch(_Request(
        "https://forkmesh.com/api/nodes/events?watch=forkmesh%2Fforkmesh",
        {"upgrade": "websocket"})))
    assert more["status"] == 101


def test_commit_notifications_also_wake_upstream_watchers():
    ids, fetches = [], []
    env = SimpleNamespace(FORKMESH_NODES=_nodes_binding(ids, fetches))

    async def context(_env, owner, repo):
        return {"nodes": set()}

    ns = _load({
        **_base_globals(),
        "_https_mirror_public_context": context,
        "valid_node_name": lambda value: bool(
            re.fullmatch(r"[a-z][a-z0-9-]{0,62}", str(value or ""))),
    })
    state = "c" * 64
    asyncio.run(ns["notify_repo_mirrors"](
        env, "ForkMesh", "ForkMesh", "commits", state, 77))
    assert ids == ["watch:forkmesh/forkmesh"]
    query = parse_qs(urlparse(fetches[0]).query)
    assert query == {"topic": ["commits"], "repo": ["forkmesh/forkmesh"],
                     "state": [state], "generation": ["77"]}
    ids.clear()
    asyncio.run(ns["notify_repo_mirrors"](
        env, "forkmesh", "forkmesh", "issues"))
    assert ids == []
