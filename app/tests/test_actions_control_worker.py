"""Mirror Actions control route and summary v2 revalidation in the Worker.

The wire contract is www/docs/operations/mirror-actions-control.md.
"""

import ast
import asyncio
import copy
import functools
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import sys
from urllib.parse import urlparse

import pytest


ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "app" / "src"
ENTRY = SRC / "entry.py"
ENTRY_TEXT = (
    # entry.py + its lazily-split domain modules
    (ENTRY.parent / "federation.py").read_text(encoding="utf-8")
    + "\n\n\n"
    + (ENTRY.parent / "ssh_gateway_routes.py").read_text(encoding="utf-8")
    + "\n\n\n"
    + (ENTRY.parent / "repo_sync.py").read_text(encoding="utf-8")
    + "\n\n\n"
    + (ENTRY.parent / "agent_routes.py").read_text(encoding="utf-8")
    + "\n\n\n"
    + ENTRY.read_text(encoding="utf-8")
    + "\n\n\n"
)
APP_RUNTIME = (SRC / "app_runtime.py").read_text(encoding="utf-8")
MODULE_TEXT = (SRC / "repository_actions.py").read_text(encoding="utf-8")
FOOTPRINT = (
    ROOT / "world" / "tools" / "build_worker_footprint.py"
).read_text(encoding="utf-8")
sys.path.insert(0, str(SRC))

import edge_routing  # noqa: E402
import urls  # noqa: E402


NOW = 1_784_920_000_000
COMMIT = "a" * 40
REQUEST_ID = "req_0123456789abcdef"
WORKFLOW = ".forkmesh/deploy.yml"
ACTIONS_CONSTANTS = {
    "HTTPS_MIRROR_ACTIONS_LEASE_MAX_MS": 15 * 60 * 1000,
    "HTTPS_MIRROR_ACTIONS_CLOCK_SKEW_MS": 60 * 1000,
    "HTTPS_MIRROR_ACTIONS_RUN_LIMIT": 20,
    "HTTPS_MIRROR_ACTIONS_LOG_TAIL_MAX_BYTES": 16 * 1024,
}


class _ContextUnavailable(Exception):
    pass


class _Date:
    @staticmethod
    def now():
        return NOW


class _Headers(dict):
    def get(self, key, default=None):
        return super().get(str(key).lower(), default)


class _Request:
    def __init__(self, body, method="POST", headers=None):
        self.method = method
        self.body = body
        self.headers = _Headers(headers or {})

    async def text(self):
        return self.body


class _Upstream:
    def __init__(self, status, text):
        self.status = status
        self._text = text
        self.headers = _Headers()

    async def text(self):
        return self._text


class _Uint8Array:
    @staticmethod
    def new(value):
        return value


def _load_module():
    spec = importlib.util.spec_from_file_location(
        "repository_actions_under_test", SRC / "repository_actions.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


@functools.lru_cache(maxsize=1)
def _entry_validator_code():
    tree = ast.parse(ENTRY_TEXT, filename=str(ENTRY))
    node = next(
        item for item in tree.body
        if isinstance(item, ast.FunctionDef)
        and item.name == "_https_mirror_actions_result"
    )
    return compile(
        ast.fix_missing_locations(ast.Module(body=[node], type_ignores=[])),
        str(ENTRY),
        "exec",
    )


def _entry_validator(delegate):
    """entry.py's own validator, with its version 2 hook wired to a module."""
    namespace = {**ACTIONS_CONSTANTS, "re": re, "_repository_actions": delegate}
    exec(_entry_validator_code(), namespace)
    return namespace["_https_mirror_actions_result"]


def _validators():
    module = _load_module()
    validate = _entry_validator(module)
    module._bind_runtime({
        **ACTIONS_CONSTANTS, "_https_mirror_actions_result": validate})
    return module, validate


def _run(**changes):
    run = {
        "id": 7,
        "workflow": "Deploy",
        "commit": COMMIT,
        "ref": "refs/heads/main",
        "status": "success",
        "createdAt": NOW - 5000,
        "startedAt": NOW - 4000,
        "finishedAt": NOW - 2000,
        "logTail": "token=[REDACTED]\nAll steps completed.",
    }
    run.update(changes)
    return run


def _v1(**changes):
    value = {
        "ok": True,
        "node": "mirror2",
        "updatedAt": NOW - 1000,
        "expiresAt": NOW + 1000,
        "runs": [_run()],
    }
    value.update(changes)
    return value


def _v2(**changes):
    value = _v1(
        schemaVersion=2,
        actionsEnabled=True,
        workflows=[{
            "path": WORKFLOW,
            "name": "Deploy",
            "events": ["push", "workflow_dispatch"],
            "manual": True,
            "enabled": True,
            "runsOn": ["linux"],
            "valid": True,
        }],
        controls=[
            {
                "requestId": REQUEST_ID,
                "action": "dispatch",
                "status": "applied",
                "reason": "",
                "runId": 7,
                "finishedAt": NOW - 500,
            },
            {
                "requestId": "req_fedcba9876543210",
                "action": "cancel",
                "status": "rejected",
                "reason": "not_cancellable",
                "runId": 0,
                "finishedAt": NOW - 900,
            },
        ],
        runs=[_run(
            workflowPath=WORKFLOW,
            pullNumber=12,
            steps=[
                {"name": "Checkout", "status": "success"},
                {"name": "", "status": "skipped"},
            ],
        )],
    )
    value.update(changes)
    return value


def _body(action="cancel", **changes):
    extra = {
        "dispatch": {"workflow": WORKFLOW, "ref": "refs/heads/main"},
        "rerun": {"runId": 7},
        "cancel": {"runId": 7},
        "approve": {"runId": 7},
        "reject": {"runId": 7},
        "set-workflow-enabled": {"workflow": WORKFLOW, "enabled": False},
        "set-repository-enabled": {"enabled": True},
    }[action]
    value = {
        "requestId": REQUEST_ID, "action": action, "node": "mirror2", **extra}
    value.update(changes)
    return value


def _check(module, value):
    return module._actions_control_body(json.dumps(value))


# ---------------------------------------------------------------------------
# Control body validator.


@pytest.mark.parametrize("action", [
    "dispatch", "rerun", "cancel", "approve", "reject",
    "set-workflow-enabled", "set-repository-enabled",
])
def test_each_action_accepts_exactly_its_own_keys(action):
    module = _load_module()
    value = _body(action)
    assert _check(module, value) == value
    assert set(value) - {"requestId", "action", "node"} == (
        module.ACTIONS_CONTROL_ACTIONS[action])
    with_token = _check(module, {**value, "sessionToken": " tok "})
    assert with_token == {**value, "sessionToken": "tok"}
    # One unknown key, and every other action's key, is refused.
    assert _check(module, {**value, "extra": 1}) is None
    for key, sample in (
            ("workflow", WORKFLOW), ("ref", "refs/heads/main"),
            ("runId", 7), ("enabled", True)):
        if key not in value:
            assert _check(module, {**value, key: sample}) is None
    # The Worker adds these; a browser may not supply them.
    for key, sample in (
            ("schemaVersion", 1), ("type", "forkmesh.actions-control-v1"),
            ("actor", "alice"), ("issuedAt", NOW)):
        assert _check(module, {**value, key: sample}) is None
    for key in value:
        missing = {k: v for k, v in value.items() if k != key}
        assert _check(module, missing) is None


@pytest.mark.parametrize("field,bad", [
    ("requestId", "short"),
    ("requestId", "x" * 65),
    ("requestId", "has space 0123456789"),
    ("requestId", 1234567890123456),
    ("requestId", None),
    ("action", "delete"),
    ("action", "Cancel"),
    ("action", 1),
    ("action", ["cancel"]),
    ("node", "Mirror2"),
    ("node", "mirror_2"),
    ("node", "-mirror"),
    ("node", ""),
    ("node", 2),
    ("runId", 0),
    ("runId", -1),
    ("runId", 2 ** 53),
    ("runId", True),
    ("runId", 7.0),
    ("runId", "7"),
    ("runId", None),
    ("sessionToken", ""),
    ("sessionToken", "   "),
    ("sessionToken", 5),
    ("sessionToken", "t" * 513),
])
def test_common_and_run_id_patterns_are_strict(field, bad):
    module = _load_module()
    assert _check(module, {**_body("cancel"), field: bad}) is None


def test_run_id_accepts_the_largest_exact_integer():
    module = _load_module()
    assert _check(module, _body("rerun", runId=2 ** 53 - 1))["runId"] == (
        2 ** 53 - 1)


@pytest.mark.parametrize("field,bad", [
    ("workflow", "deploy.yml"),
    ("workflow", ".forkmesh/deploy.json"),
    ("workflow", ".forkmesh/sub/deploy.yml"),
    ("workflow", ".forkmesh/" + "x" * 97 + ".yml"),
    ("workflow", ".forkmesh/deploy.yml\n"),
    ("workflow", ""),
    ("workflow", 1),
    ("ref", "main"),
    ("ref", "refs/tags/v1"),
    ("ref", "refs/heads/"),
    ("ref", "refs/heads/a..b"),
    ("ref", "refs/heads/a//b"),
    ("ref", "refs/heads/a/"),
    ("ref", "refs/heads/a.lock"),
    ("ref", "refs/heads/a b"),
    ("ref", "refs/heads/a\n"),
    ("ref", "refs/heads/" + "x" * 201),
    ("ref", None),
])
def test_dispatch_patterns_are_strict(field, bad):
    module = _load_module()
    assert _check(module, _body("dispatch", **{field: bad})) is None


def test_dispatch_accepts_yaml_suffix_and_nested_branch():
    module = _load_module()
    value = _body(
        "dispatch", workflow=".forkmesh/ci.yaml",
        ref="refs/heads/feature/x-1.2")
    assert _check(module, value) == value


@pytest.mark.parametrize("action", [
    "set-workflow-enabled", "set-repository-enabled"])
@pytest.mark.parametrize("bad", [0, 1, "true", None, [True]])
def test_enabled_must_be_a_real_boolean(action, bad):
    module = _load_module()
    assert _check(module, _body(action, enabled=bad)) is None
    for good in (True, False):
        assert _check(module, _body(action, enabled=good))["enabled"] is good


def test_body_must_be_one_small_duplicate_free_json_object():
    module = _load_module()
    check = module._actions_control_body
    good = json.dumps(_body())
    assert check(good) is not None
    assert module.ACTIONS_CONTROL_BODY_MAX_BYTES == 4 * 1024
    padded = good[:-1] + "," + " " * (4 * 1024) + '"x":1}'
    assert check(padded) is None
    assert check(good + " " * (4 * 1024 + 1 - len(good))) is None
    assert check(good + " " * (4 * 1024 - len(good))) is not None
    for raw in ("", "[]", "null", "7", '"cancel"', "{", b"{}", None):
        assert check(raw) is None
    assert check(good[:-1] + ',"runId":8}') is None


def test_gateway_body_is_canonical_and_never_forwards_node_or_token():
    module = _load_module()
    data = _check(module, _body("dispatch", sessionToken="tok"))
    raw = module._actions_control_gateway_body(data, "alice", NOW)
    document = json.loads(raw)
    assert document == {
        "schemaVersion": 1,
        "type": "forkmesh.actions-control-v1",
        "requestId": REQUEST_ID,
        "action": "dispatch",
        "actor": "alice",
        "issuedAt": NOW,
        "workflow": WORKFLOW,
        "ref": "refs/heads/main",
    }
    assert raw == json.dumps(
        document, sort_keys=True, separators=(",", ":")).encode("utf-8")
    for actor in ("", "Alice", "-alice", "a" * 65, "al ice"):
        assert module._actions_control_gateway_body(data, actor, NOW) == b""


def test_only_the_exact_acknowledgement_is_accepted():
    module = _load_module()
    accepted = {
        "ok": True, "status": "accepted",
        "requestId": REQUEST_ID, "node": "mirror2"}
    check = module._actions_control_accepted
    assert check(dict(accepted), REQUEST_ID, "mirror2") == accepted
    for changes in (
            {"ok": 1}, {"status": "queued"}, {"requestId": REQUEST_ID + "x"},
            {"node": "mirror3"}, {"extra": 1}):
        assert check({**accepted, **changes}, REQUEST_ID, "mirror2") is None
    assert check(
        {k: v for k, v in accepted.items() if k != "node"},
        REQUEST_ID, "mirror2") is None
    assert check([accepted], REQUEST_ID, "mirror2") is None


# ---------------------------------------------------------------------------
# Summary v2 revalidation.


def test_version_2_answer_is_revalidated_and_passed_through():
    _, validate = _validators()
    value = _v2()
    checked = validate(copy.deepcopy(value), "mirror2", NOW)
    assert checked == value
    assert set(checked) == {
        "ok", "schemaVersion", "node", "updatedAt", "expiresAt",
        "actionsEnabled", "workflows", "controls", "runs"}
    assert set(checked["runs"][0]) == set(_run()) | {
        "workflowPath", "pullNumber", "steps"}
    empty = _v2(actionsEnabled=False, workflows=[], controls=[], runs=[])
    assert validate(copy.deepcopy(empty), "mirror2", NOW) == empty
    not_pull = _v2()
    not_pull["runs"][0].update(workflowPath="", pullNumber=0, steps=[])
    assert validate(copy.deepcopy(not_pull), "mirror2", NOW) == not_pull


def test_version_1_answer_is_still_exact():
    _, validate = _validators()
    value = _v1()
    assert validate(copy.deepcopy(value), "mirror2", NOW) == value
    # A version 2 key on a version 1 answer is still an unknown key.
    for key, sample in (
            ("actionsEnabled", True), ("workflows", []), ("controls", [])):
        assert validate(_v1(**{key: sample}), "mirror2", NOW) is None
    with_path = _v1()
    with_path["runs"][0]["workflowPath"] = WORKFLOW
    assert validate(with_path, "mirror2", NOW) is None
    for version in (1, 3, "2", 2.0, True, None):
        assert validate(_v1(schemaVersion=version), "mirror2", NOW) is None
        assert validate(_v2(schemaVersion=version), "mirror2", NOW) is None


def _mutate(path, new):
    value = _v2()
    target = value
    for key in path[:-1]:
        target = target[key]
    if new is _DROP:
        del target[path[-1]]
    else:
        target[path[-1]] = new
    return value


_DROP = object()
LONG_LIST = ["e%d" % index for index in range(9)]


@pytest.mark.parametrize("path,new", [
    # Top level.
    (("actionsEnabled",), 1),
    (("actionsEnabled",), _DROP),
    (("workflows",), _DROP),
    (("controls",), _DROP),
    (("workflows",), {}),
    (("controls",), None),
    (("extra",), 1),
    (("node",), "mirror3"),
    (("ok",), False),
    (("expiresAt",), NOW - 1),
    (("updatedAt",), True),
    (("runs",), [_run()]),
    # Runs: the version 1 rules still apply underneath.
    (("runs", 0, "status"), "done"),
    (("runs", 0, "commit"), "zz"),
    (("runs", 0, "logTail"), "x" * (16 * 1024 + 1)),
    (("runs", 0, "extra"), 1),
    (("runs", 0, "workflowPath"), _DROP),
    (("runs", 0, "workflowPath"), "deploy.yml"),
    (("runs", 0, "workflowPath"), None),
    (("runs", 0, "pullNumber"), -1),
    (("runs", 0, "pullNumber"), True),
    (("runs", 0, "pullNumber"), "12"),
    (("runs", 0, "pullNumber"), 1.5),
    (("runs", 0, "pullNumber"), 2 ** 53),
    (("runs", 0, "steps"), None),
    (("runs", 0, "steps"), [{"name": "s", "status": "success"}] * 65),
    (("runs", 0, "steps", 0), ["Checkout", "success"]),
    (("runs", 0, "steps", 0, "name"), "x" * 121),
    (("runs", 0, "steps", 0, "name"), "two\nlines"),
    (("runs", 0, "steps", 0, "name"), "nul\x00"),
    (("runs", 0, "steps", 0, "name"), 5),
    (("runs", 0, "steps", 0, "status"), "queued"),
    (("runs", 0, "steps", 0, "status"), None),
    (("runs", 0, "steps", 0, "command"), "make deploy"),
    (("runs", 0, "steps", 0, "status"), _DROP),
    # Workflows.
    (("workflows",), [_v2()["workflows"][0]] * 2),
    (("workflows", 0), "deploy"),
    (("workflows", 0, "path"), "/etc/passwd"),
    (("workflows", 0, "path"), ".forkmesh/../x.yml"),
    (("workflows", 0, "name"), "x" * 161),
    (("workflows", 0, "name"), "a\rb"),
    (("workflows", 0, "name"), None),
    (("workflows", 0, "events"), LONG_LIST),
    (("workflows", 0, "events"), ["x" * 41]),
    (("workflows", 0, "events"), [""]),
    (("workflows", 0, "events"), ["push\n"]),
    (("workflows", 0, "events"), [1]),
    (("workflows", 0, "events"), "push"),
    (("workflows", 0, "runsOn"), LONG_LIST),
    (("workflows", 0, "runsOn"), ["x" * 65]),
    (("workflows", 0, "runsOn"), [None]),
    (("workflows", 0, "manual"), 1),
    (("workflows", 0, "enabled"), "true"),
    (("workflows", 0, "valid"), None),
    (("workflows", 0, "valid"), _DROP),
    (("workflows", 0, "content"), "on: push"),
    # Controls.
    (("controls", 0), None),
    (("controls", 0, "requestId"), "short"),
    (("controls", 0, "requestId"), 5),
    (("controls", 0, "action"), "delete"),
    (("controls", 0, "action"), None),
    (("controls", 0, "status"), "accepted"),
    (("controls", 0, "reason"), "run_not_found"),
    (("controls", 0, "reason"), None),
    (("controls", 1, "reason"), ""),
    (("controls", 1, "reason"), "because"),
    (("controls", 1, "reason"), ["run_not_found"]),
    (("controls", 0, "runId"), -1),
    (("controls", 0, "runId"), True),
    (("controls", 0, "runId"), 2 ** 53),
    (("controls", 0, "finishedAt"), -1),
    (("controls", 0, "finishedAt"), NOW + 61 * 1000),
    (("controls", 0, "finishedAt"), "0"),
    (("controls", 0, "owner"), "forkmesh"),
    (("controls", 0, "repository"), "forkmesh"),
    (("controls", 0, "finishedAt"), _DROP),
])
def test_version_2_rejects_each_malformed_field(path, new):
    _, validate = _validators()
    assert validate(_mutate(path, new), "mirror2", NOW) is None


def test_version_2_list_bounds_are_exact():
    module, validate = _validators()
    workflow = _v2()["workflows"][0]
    control = _v2()["controls"][0]

    def workflows(count):
        return [
            {**workflow, "path": ".forkmesh/w%d.yml" % index}
            for index in range(count)]

    assert validate(_v2(workflows=workflows(32)), "mirror2", NOW)
    assert validate(_v2(workflows=workflows(33)), "mirror2", NOW) is None
    assert validate(_v2(controls=[control] * 32), "mirror2", NOW)
    assert validate(_v2(controls=[control] * 33), "mirror2", NOW) is None
    steps = [{"name": "s", "status": "pending"}] * 64
    value = _v2()
    value["runs"][0]["steps"] = steps
    assert validate(value, "mirror2", NOW)
    for reason in module.ACTIONS_CONTROL_REJECTION_REASONS:
        value = _v2()
        value["controls"][1]["reason"] = reason
        assert validate(value, "mirror2", NOW)
    for status in module.ACTIONS_STEP_STATUSES:
        value = _v2()
        value["runs"][0]["steps"][0]["status"] = status
        assert validate(value, "mirror2", NOW)
    assert module.ACTIONS_STEP_STATUSES == {
        "pending", "running", "success", "failed", "skipped", "cancelled"}
    assert validate(_v2(runs=[_v2()["runs"][0]] * 21), "mirror2", NOW) is None


# ---------------------------------------------------------------------------
# Route, operation and lazy-module wiring.


def test_route_regex_matches_only_the_control_path():
    match = urls.REPO_ACTION_CONTROL_RE.fullmatch(
        "/api/repo/forkmesh/forkmesh/actions/control")
    assert match and match.groups() == ("forkmesh", "forkmesh")
    for path in (
            "/api/repo/forkmesh/forkmesh/actions/control/",
            "/api/repo/forkmesh/forkmesh/actions/control/x",
            "/api/repo/forkmesh/forkmesh/actions/runs",
            "/api/repo/forkmesh/actions/control",
            "/api/repo/forkmesh/forkmesh/actions-control"):
        assert not urls.REPO_ACTION_CONTROL_RE.match(path)
    assert not urls.REPO_ACTION_RUNS_RE.match(
        "/api/repo/forkmesh/forkmesh/actions/control")


def test_operation_is_registered_for_signed_post_routing():
    assert "actions-control" in edge_routing.PUBLIC_OPERATIONS
    assert "actions-control" not in edge_routing.MERGE_EXECUTOR_OPERATIONS
    target = edge_routing.masked_target_url(
        "https://mirror.example.test", "forkmesh", "forkmesh",
        "actions-control")
    assert target == (
        "https://mirror.example.test"
        "/v1/repositories/forkmesh/forkmesh/actions-control")
    # No query key survives for this operation's empty allowlist callers.
    assert "?" not in target
    message = edge_routing.request_message(
        "mirror2", "POST", urlparse(target).path, "0" * 64,
        "A" * 24, NOW)
    assert message.split("\n")[2:4] == [
        "POST", "/v1/repositories/forkmesh/forkmesh/actions-control"]
    # A proof advertising the operation still digests; before registration an
    # unknown name voided the node's whole signed proof.
    assert edge_routing.operations_sha256(
        ["actions-control", "actions-status"])


def test_handler_lives_in_the_lazy_module_not_in_entry():
    assert "def repository_actions_control_handler" not in ENTRY_TEXT
    assert "def _actions_control_body" not in ENTRY_TEXT
    assert "def _https_mirror_actions_control_proxy" not in ENTRY_TEXT
    assert "def _https_mirror_actions_result_v2" not in ENTRY_TEXT
    assert "forkmesh.actions-control-v1" not in ENTRY_TEXT
    assert (
        '_repository_actions = _LazyModule("repository_actions")'
        in ENTRY_TEXT)
    assert (
        "repository_actions_control_handler = _repository_actions.export(\n"
        '    "repository_actions_control_handler")' in ENTRY_TEXT)
    assert "_repository_actions._https_mirror_actions_result_v2(" in ENTRY_TEXT
    assert '"REPO_ACTION_CONTROL_RE"' in ENTRY_TEXT
    assert "REPO_ACTION_CONTROL_RE.match(url.path)" in APP_RUNTIME
    assert (
        "return await repository_actions_control_handler(\n"
        "                self.env, request, owner, repo)" in APP_RUNTIME)
    assert '"repository_actions.py",' in FOOTPRINT
    # Nothing in the module is imported by name at entry.py global scope.
    assert not re.search(
        r"^(?:import|from) repository_actions\b", ENTRY_TEXT, re.M)


def test_handler_source_keeps_session_authorization_and_audit():
    handler = MODULE_TEXT[
        MODULE_TEXT.index("async def repository_actions_control_handler"):]
    for required in (
        'method_name(request) != "POST"',
        "_https_mirror_public_context",
        "_account_session_record(env, request, data)",
        "_account_owns_node",
        "_org_write_allowed",
        "_audit_sensitive_action",
        '"repository.actions_control"',
        '"denied", {"action": data["action"]}',
        "no-store",
        "EXPECTED_DEGRADED_HEADERS",
        'operation=ACTIONS_CONTROL_OPERATION',
        'candidate["node"] == data["node"]',
    ):
        assert required in handler or required in MODULE_TEXT, required
    assert "_account_session_record(env, request, data)" in handler
    assert '"repository.actions_control"' in handler
    proxy = MODULE_TEXT[
        MODULE_TEXT.index("async def _https_mirror_actions_control_proxy"):
        MODULE_TEXT.index("def _actions_node_unavailable")]
    for required in (
        "_https_mirror_repository_proof(",
        "ACTIONS_CONTROL_OPERATION",
        "https_routing.request_message(",
        '"POST"',
        "ed25519_sign(",
        "X-ForkMesh-Signature",
        "X-ForkMesh-Body-Sha256",
        '"redirect": "manual"',
        "!= 202",
    ):
        assert required in proxy, required
    # No concurrent tasks on a request path (see entry.py's rule).
    for banned in ("asyncio", "gather(", "create_task(", "wait_for("):
        assert banned not in MODULE_TEXT.replace("asyncio.wait_for", "")
    status = ENTRY_TEXT[
        ENTRY_TEXT.index("async def repository_actions_status_handler"):
        ENTRY_TEXT.index("_repository_actions = _LazyModule(")]
    assert 'result["controlAvailable"] = await ' in status
    assert 'env, candidate, context, "actions-control")' in status


# ---------------------------------------------------------------------------
# Handler behaviour against a fake runtime.


def _harness(
        actor="alice", owner="alice", context="ok", owns_node=False,
        org_write=False, candidates=("mirror1", "mirror2"), proof=True,
        upstream_status=202, upstream=None, signature="sig"):
    module = _load_module()
    calls = {"audit": [], "fetch": [], "candidates": [], "proof": []}

    def json_response(payload, status=200, cache_control="", **kwargs):
        return {
            "status": status, "payload": payload,
            "cacheControl": cache_control,
            "headers": kwargs.get("extra_headers")}

    async def public_context(env, owner_name, repo):
        if context == "unavailable":
            raise _ContextUnavailable()
        if context == "missing":
            return None
        return {"owner": owner, "repo": repo}

    async def session_record(env, request, data=None):
        calls["sessionData"] = data
        return ("bi", {"name": actor}) if actor else ("", None)

    async def owns(env, name, node):
        return owns_node

    async def org_write_allowed(env, node, repo, name):
        return org_write

    async def audit(env, name, action, target_type, target, outcome, details):
        calls["audit"].append((name, action, target_type, target, outcome,
                               details))

    async def mirror_candidates(
            env, ctx, region, sticky="", avoid_node="", operation=""):
        calls["candidates"].append((sticky, operation))
        return [
            {"node": node, "baseUrl": "https://%s.example.test" % node}
            for node in candidates]

    async def repository_proof(env, endpoint, ctx, operation, **kwargs):
        calls["proof"].append((endpoint["node"], operation))
        return proof

    async def sign(public_key, seed, message):
        calls["signed"] = message.decode("utf-8")
        return signature

    async def fetch(target, options, timeout):
        calls["fetch"].append((target, options))
        body = upstream if upstream is not None else json.dumps({
            "ok": True, "status": "accepted",
            "requestId": REQUEST_ID, "node": "mirror2"})
        return _Upstream(upstream_status, body)

    module._bind_runtime({
        "Date": _Date,
        "urlparse": urlparse,
        "https_routing": edge_routing,
        "json_response": json_response,
        "method_name": lambda request: request.method,
        "clean_string": lambda value, limit: str(value or "")[:limit],
        "MAX_NODE_NAME": 64,
        "EXPECTED_DEGRADED_HEADERS": {"X-ForkMesh-Expected-Degraded": "1"},
        "HTTPS_MIRROR_FETCH_TIMEOUT_SECONDS": 8,
        "_HttpsMirrorContextUnavailable": _ContextUnavailable,
        "_https_mirror_public_context": public_context,
        "_account_session_record": session_record,
        "_account_owns_node": owns,
        "_org_write_allowed": org_write_allowed,
        "_audit_sensitive_action": audit,
        "_https_mirror_candidates": mirror_candidates,
        "_https_mirror_repository_proof": repository_proof,
        "_https_mirror_router_public_key": lambda env: "pub",
        "_https_mirror_router_seed": lambda env: "seed",
        "world_request_country": lambda request: "US",
        "ed25519_sign": sign,
        "js_fetch_with_timeout": fetch,
        "Uint8Array": _Uint8Array,
        "_to_js": lambda value: value,
        "_b64url_encode": lambda value: "R" * 24,
        "_random_bytes": lambda count: b"\x00" * count,
    })
    return module, calls


def _post(module, body=None, **request):
    raw = body if isinstance(body, str) else json.dumps(
        _body() if body is None else body)
    return asyncio.run(module.repository_actions_control_handler(
        None, _Request(raw, **request), "alice", "project"))


def test_accepted_request_is_signed_for_the_named_node_and_audited():
    module, calls = _harness()
    response = _post(module, _body("cancel", sessionToken="tok"))
    assert response == {
        "status": 202,
        "payload": {
            "ok": True, "status": "accepted",
            "requestId": REQUEST_ID, "node": "mirror2"},
        "cacheControl": "no-store",
        "headers": None,
    }
    assert calls["sessionData"]["sessionToken"] == "tok"
    assert calls["candidates"] == [("mirror2", "actions-control")]
    assert calls["proof"] == [("mirror2", "actions-control")]
    (target, options), = calls["fetch"]
    assert target == (
        "https://mirror2.example.test"
        "/v1/repositories/alice/project/actions-control")
    assert options["method"] == "POST" and options["redirect"] == "manual"
    sent = options["body"]
    assert json.loads(sent) == {
        "schemaVersion": 1, "type": "forkmesh.actions-control-v1",
        "requestId": REQUEST_ID, "action": "cancel", "actor": "alice",
        "issuedAt": NOW, "runId": 7}
    assert sent == json.dumps(
        json.loads(sent), sort_keys=True, separators=(",", ":")).encode()
    headers = options["headers"]
    assert headers["X-ForkMesh-Node"] == "mirror2"
    assert headers["X-ForkMesh-Body-Sha256"] == hashlib.sha256(
        sent).hexdigest()
    assert headers["X-ForkMesh-Issued-At"] == str(NOW)
    assert headers["X-ForkMesh-Signature"] == "sig"
    assert calls["signed"] == edge_routing.request_message(
        "mirror2", "POST", "/v1/repositories/alice/project/actions-control",
        headers["X-ForkMesh-Body-Sha256"], "R" * 24, NOW)
    assert calls["audit"] == [(
        "alice", "repository.actions_control", "repository", "alice/project",
        "success", {"action": "cancel", "node": "mirror2", "runId": 7})]


def test_audit_names_the_workflow_for_workflow_actions():
    module, calls = _harness()
    assert _post(module, _body("dispatch"))["status"] == 202
    assert calls["audit"][0][5] == {
        "action": "dispatch", "node": "mirror2", "workflow": WORKFLOW}
    module, calls = _harness()
    assert _post(module, _body("set-repository-enabled"))["status"] == 202
    assert calls["audit"][0][5] == {
        "action": "set-repository-enabled", "node": "mirror2"}


def test_request_ladder_before_any_node_is_contacted():
    module, calls = _harness()
    assert _post(module, method="GET")["status"] == 405
    module, calls = _harness(context="unavailable")
    response = _post(module)
    assert (response["status"], response["payload"]) == (
        503, {"error": "mirror_unavailable"})
    assert response["headers"]
    module, calls = _harness(context="missing")
    assert _post(module)["payload"] == {"error": "not_found"}
    module, calls = _harness()
    response = _post(module, headers={"content-length": "4097"})
    assert (response["status"], response["payload"]) == (
        413, {"error": "request_too_large"})
    response = _post(module, json.dumps(_body()) + " " * 4096)
    assert response["status"] == 413
    response = _post(module, _body(runId=True))
    assert (response["status"], response["payload"]) == (
        400, {"error": "invalid_request"})
    assert "sessionData" not in calls
    module, calls = _harness(actor="")
    response = _post(module)
    assert (response["status"], response["payload"]) == (
        401, {"error": "invalid_session"})
    assert calls["audit"] == [] and calls["fetch"] == []


def test_only_owner_node_owner_or_org_writer_may_control():
    module, calls = _harness(actor="mallory")
    response = _post(module)
    assert (response["status"], response["payload"]) == (
        403, {"error": "forbidden"})
    assert response["cacheControl"] == "no-store"
    assert calls["audit"] == [(
        "mallory", "repository.actions_control", "repository",
        "alice/project", "denied", {"action": "cancel"})]
    assert calls["candidates"] == [] and calls["fetch"] == []
    for grant in ({"owns_node": True}, {"org_write": True}):
        module, calls = _harness(actor="bob", **grant)
        assert _post(module)["status"] == 202
        assert calls["audit"][0][0] == "bob"
        assert json.loads(calls["fetch"][0][1]["body"])["actor"] == "bob"


@pytest.mark.parametrize("changes", [
    {"candidates": ("mirror1", "mirror3")},
    {"candidates": ()},
    {"proof": False},
    {"signature": ""},
    {"upstream_status": 200},
    {"upstream_status": 409},
    {"upstream_status": 500},
    {"upstream": "not json"},
    {"upstream": json.dumps({
        "ok": True, "status": "accepted",
        "requestId": REQUEST_ID, "node": "mirror1"})},
    {"upstream": json.dumps({
        "ok": True, "status": "accepted", "requestId": REQUEST_ID,
        "node": "mirror2", "path": "/var/lib"})},
    {"upstream": "x" * (4 * 1024 + 1)},
])
def test_any_failure_to_reach_or_accept_is_one_expected_degraded_503(changes):
    module, calls = _harness(**changes)
    response = _post(module)
    assert response == {
        "status": 503,
        "payload": {"error": "actions_node_unavailable"},
        "cacheControl": "no-store",
        "headers": {"X-ForkMesh-Expected-Degraded": "1"},
    }
    # Never a second node, and nothing is audited as done.
    assert len(calls["fetch"]) <= 1
    assert all(
        "mirror2" in target for target, _ in calls["fetch"])
    assert calls["audit"] == []
