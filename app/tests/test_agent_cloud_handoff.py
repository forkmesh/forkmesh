"""The desktop's "Move to cloud" handoff endpoint.

The desktop has no browser session, so this route is authorized by the node's
own signing key.  What it accepts (and what it refuses to accept) decides
whether a session that stopped running on one desk can start again on another.
"""

import ast
import asyncio
import re
from pathlib import Path
from types import SimpleNamespace
from urllib.parse import parse_qs, urlparse


HANDLERS = Path(__file__).resolve().parents[1] / "src" / "route_handlers.py"

SIGNING_KEY = "device-key-of-the-desk-giving-up-the-session"


class Recorder:
    def __init__(self, other_nodes=1, batch_raises=False, targets=()):
        self.targets = list(targets)
        self.target_contexts = []
        self.batches = []
        self.audits = []
        self.notified = []
        self.account_events = []
        self.other_nodes = other_nodes
        self.batch_raises = batch_raises

    async def run_batch(self, _env, statements):
        if self.batch_raises:
            raise RuntimeError("d1 unavailable")
        self.batches.append(statements)

    async def first(self, _env, sql, *args):
        assert "FROM account_devices" in sql
        assert args[1] == SIGNING_KEY
        return {"n": self.other_nodes}

    async def audit(self, _env, actor, action, kind, target, outcome, detail):
        self.audits.append((actor, action, target, outcome, detail))

    async def owner_targets(self, _env, context):
        self.target_contexts.append(context)
        return self.targets

    async def notify(self, _env, owner, repo, channel):
        self.notified.append((owner, repo, channel))

    async def notify_account(self, _env, owner, topic):
        self.account_events.append((owner, topic))

    def rows(self):
        """Every (sql, params) pair the one queued batch wrote."""
        assert len(self.batches) == 1
        return self.batches[0]

    def payload(self, index):
        """The one encrypted dict this batch row carries."""
        _sql, params = self.rows()[index]
        payloads = [item for item in params if isinstance(item, dict)]
        assert len(payloads) == 1
        return payloads[0]


def _load(recorder, signing_key=SIGNING_KEY, keys=None, owned=(),
          org_owned=()):
    tree = ast.parse(HANDLERS.read_text(encoding="utf-8"), filename=str(HANDLERS))
    wanted = {
        "agent_handoff_handler",
        "_agent_handoff_signer",
        "_agent_handoff_model",
        "_agent_handoff_mirror_target",
        "_ring_cloud_agent_readers",
    }
    selected = [
        node for node in tree.body
        if isinstance(node, (ast.AsyncFunctionDef, ast.FunctionDef))
        and node.name in wanted
    ]
    assert {node.name for node in selected} == wanted
    module = ast.fix_missing_locations(
        ast.Module(body=selected, type_ignores=[]))

    class Clock:
        @staticmethod
        def now():
            return 2_100_000_000_000

    async def noop(*_args, **_kwargs):
        return None

    def response(data, status=200, **_kwargs):
        return {"status": status, "data": data}

    namespace = {
        "re": re,
        "Date": Clock,
        "MAX_NODE_NAME": 64,
        "MAX_REPO_SEGMENT": 100,
        "ORG_AGENT_PROVIDERS": ("claude-code", "codex"),
        "ORG_AGENT_MODEL_ALIASES": {
            "claude-code": {
                "opus": "claude-opus-5-5", "opus-5": "claude-opus-5",
                "haiku": "claude-haiku-4-5",
            },
            "codex": {"sol": "gpt-5.6-sol"},
        },
        "ORG_AGENT_MAX_PROMPT": 8000,
        "BRANCH_OP_NAME_PATTERN": r"[A-Za-z0-9][A-Za-z0-9._/-]{0,199}",
        "_AGENT_HANDOFF_MAX_BRANCH": 200,
        "_AGENT_HANDOFF_MAX_TITLE": 160,
        "ensure_schema": noop,
        "method_name": lambda request: request.method,
        "_authorized_owner_signing_key": lambda _env, _request, name: (
            asyncio.sleep(0, result=(
                signing_key if keys is None else keys.get(name, "")))),
        "_account_owns_node": lambda _env, actor, node: asyncio.sleep(
            0, result=(actor, node) in owned),
        "_org_owner_of_linked_repo": lambda _env, node, repo, actor: (
            asyncio.sleep(0, result=(actor, node, repo) in org_owned)),
        "parse_qs": parse_qs,
        "urlparse": urlparse,
        "clean_string": lambda value, maximum: str(
            "" if value is None else value)[:maximum],
        "valid_node_name": lambda name: bool(
            re.fullmatch(r"[a-z0-9][a-z0-9-]{0,38}", str(name or ""))),
        "safe_segment": lambda value: str(value or ""),
        "bounded_json_request": lambda request: asyncio.sleep(
            0, result=request.body),
        "blind_index": lambda _env, value: asyncio.sleep(
            0, result="bi:" + value),
        "encrypt_row": lambda _env, value: asyncio.sleep(
            0, result=value),
        "_ap_uuid": lambda: "session-uuid",
        "_org_agent_security_envelope": lambda context, owner_scope: (
            {"state": "not_required", "model": "", "reason": ""},
            {
                "required": False,
                "waiver": "repository_owner",
                "waivedFor": context["actor"],
                "tools": False,
                "failClosed": True,
            },
            "queued",
        ),
        "_org_agent_error": lambda code, message, status, **kwargs: {
            "status": status,
            "data": {"ok": False, "error": code, "message": message, **kwargs},
        },
        "_contribution_run_batch": recorder.run_batch,
        "_audit_sensitive_action": recorder.audit,
        "notify_repo_host": recorder.notify,
        "notify_account_event": recorder.notify_account,
        "d1_first": recorder.first,
        "_org_agent_owner_targets": recorder.owner_targets,
        "_profile_catalog_owner_names": lambda _env, name: asyncio.sleep(
            0, result=[name, "mirror9", "workshop"]),
        "json_response": response,
    }
    exec(compile(module, str(HANDLERS), "exec"), namespace)
    return namespace["agent_handoff_handler"], namespace["_agent_handoff_model"]


def _body(**overrides):
    body = {
        "provider": "claude-code",
        "model": "claude-opus-5",
        "prompt": "Finish wiring the cloud handoff button.",
        "branch": "agent/adhoc-2115-move-to-cloud",
        "baseBranch": "main",
        "baseRef": "a1b2c3d4e5f6",
        "title": "Move an agent into the cloud",
        "issueNumber": 0,
        "createPr": True,
        "fromDevice": "workshop",
    }
    body.update(overrides)
    return body


def _request(method="POST", signer="jett", **overrides):
    return SimpleNamespace(
        method=method, body=_body(**overrides),
        url=("https://app.forkmesh.com/api/repo/jett/forkmesh/agents/412/"
             "handoff?owner=" + signer + "&ts=1&sig=s"))


def _post(recorder, signing_key=SIGNING_KEY, **overrides):
    handler, _model = _load(recorder, signing_key)
    return asyncio.run(handler(
        object(), _request(**overrides), "jett", "forkmesh", "412"))


def test_queues_a_continuation_job_naming_the_published_branch():
    recorder = Recorder()
    response = _post(recorder)
    assert response["status"] == 202
    assert response["data"]["ok"] is True
    assert response["data"]["sessionId"] == "session-uuid"
    assert response["data"]["queueState"] == "ready_for_claim"

    session_sql, _session_args = recorder.rows()[0]
    job_sql, _job_args = recorder.rows()[1]
    assert "INSERT INTO org_agent_sessions" in session_sql
    assert "INSERT INTO org_agent_jobs" in job_sql
    record = recorder.payload(0)
    job = recorder.payload(1)
    assert record["scope"] == "owner"
    assert record["handoff"]["branch"] == "agent/adhoc-2115-move-to-cloud"
    assert job["kind"] == "continue"
    assert job["branch"] == "agent/adhoc-2115-move-to-cloud"
    assert job["baseBranch"] == "main"
    assert job["baseRef"] == "a1b2c3d4e5f6"
    assert job["model"] == "claude-opus-5"
    assert job["prompt"] == "Finish wiring the cloud handoff button."
    # Owner-waived, and named as the owner on both sides so the receiving
    # desktop's own re-verification of the waiver can accept it.
    assert job["securityCheck"]["waiver"] == "repository_owner"
    assert job["securityCheck"]["waivedFor"] == "jett"
    assert job["requestedBy"] == "jett"
    assert recorder.notified == [("jett", "forkmesh", "org-agents")]


def test_the_job_carries_the_signing_key_of_the_desk_it_left():
    """Without it the origin claims its own handoff back and nothing moves."""
    recorder = Recorder()
    _post(recorder)
    assert recorder.payload(1)["fromKey"] == SIGNING_KEY
    assert recorder.payload(0)["handoff"]["fromKey"] == SIGNING_KEY


def test_the_transcript_is_never_copied_into_the_relay_readable_row():
    """It is owner-sealed on the snapshot transport; this row is not."""
    recorder = Recorder()
    _post(recorder, transcript="secret internal deliberation")
    serialized = repr(recorder.rows())
    assert "secret internal deliberation" not in serialized


def test_a_lone_node_is_told_nothing_can_pick_the_session_up_yet():
    recorder = Recorder(other_nodes=0)
    response = _post(recorder)
    assert response["data"]["queueState"] == "waiting_for_node"
    assert response["data"]["otherNodes"] == 0
    assert "No other node is enrolled" in response["data"]["message"]


def test_an_unsigned_request_is_rejected_before_anything_is_queued():
    recorder = Recorder()
    response = _post(recorder, signing_key="")
    assert response["status"] == 401
    assert response["data"]["error"] == "unauthorized"
    assert recorder.batches == []


def test_only_get_shaped_methods_are_refused():
    recorder = Recorder()
    handler, _model = _load(recorder)
    response = asyncio.run(handler(
        object(), _request(method="GET"), "jett", "forkmesh", "412"))
    assert response["status"] == 405
    assert recorder.batches == []


def test_a_branch_the_cloud_could_not_fetch_is_refused():
    recorder = Recorder()
    for branch in ("", "  ", "--upload-pack=evil", "a branch with spaces"):
        response = _post(recorder, branch=branch)
        assert response["status"] == 400
        assert response["data"]["error"] == "invalid_branch"
    assert recorder.batches == []


def test_an_api_only_provider_cannot_be_moved_into_the_cloud():
    recorder = Recorder()
    response = _post(recorder, provider="claude-api")
    assert response["status"] == 400
    assert response["data"]["error"] == "invalid_provider"
    assert recorder.batches == []


def test_a_handoff_without_a_task_is_refused():
    recorder = Recorder()
    response = _post(recorder, prompt="   ")
    assert response["status"] == 400
    assert response["data"]["error"] == "prompt_required"
    assert recorder.batches == []


def test_a_base_ref_that_is_not_a_commit_is_refused():
    recorder = Recorder()
    response = _post(recorder, baseRef="main; rm -rf /")
    assert response["status"] == 400
    assert response["data"]["error"] == "invalid_base_ref"
    assert recorder.batches == []


def test_a_queue_write_that_fails_reports_a_retryable_degradation():
    recorder = Recorder(batch_raises=True)
    response = _post(recorder)
    assert response["status"] == 503
    assert response["data"]["error"] == "queue_persistence_failed"
    assert response["data"]["retryable"] is True


def test_models_resolve_from_either_the_web_alias_or_the_desktops_own_id():
    _handler, resolve = _load(Recorder())
    assert resolve("claude-code", "opus") == "claude-opus-5-5"
    assert resolve("claude-code", "opus-5") == "claude-opus-5"
    assert resolve("claude-code", "claude-opus-5-5") == "claude-opus-5-5"
    assert resolve("claude-code", "claude-opus-5") == "claude-opus-5"
    assert resolve("codex", "gpt-5.6-sol") == "gpt-5.6-sol"
    # A model this provider does not offer falls back to the node's default
    # rather than failing the move; the receiving desktop re-checks the id.
    assert resolve("claude-code", "gpt-5.6-sol") == ""
    assert resolve("claude-code", "") == ""


def _target(node, runtime="headless", online=True, providers=("claude-code",)):
    return {
        "node": node,
        "providers": list(providers),
        "online": online,
        "runtimeMode": runtime,
    }


def test_the_session_continues_on_an_owned_mirror_node():
    """A mirror drains its own node name, so the job must be queued there."""
    recorder = Recorder(targets=[
        _target("workshop", runtime="desktop"),
        _target("mirror9"),
    ])
    response = _post(recorder)
    assert response["status"] == 202
    assert response["data"]["targetNode"] == "mirror9"
    assert response["data"]["queueState"] == "ready_for_claim"
    assert "mirror9 continues the session" in response["data"]["message"]
    session_sql, session_args = recorder.rows()[0]
    job_sql, job_args = recorder.rows()[1]
    assert session_args[3] == "mirror9"
    assert job_args[2] == "mirror9"
    assert recorder.payload(0)["targetNode"] == "mirror9"
    job = recorder.payload(1)
    assert job["repositoryOwner"] == "mirror9"
    assert job["organization"] == "jett"
    assert job["kind"] == "continue"
    assert recorder.notified == [("mirror9", "forkmesh", "org-agents")]
    # The mirror is told there is work; the account's own devices are told
    # there is now a run they are not the ones doing (adhoc #404).
    assert recorder.account_events == [("jett", "cloud-agents")]
    context = recorder.target_contexts[0]
    assert context["scope"] == "owner"
    assert context["nodeOwner"] == "jett"
    assert "mirror9" in context["ownedNodes"]


def test_a_mirror_without_the_runtime_or_the_leaving_device_is_skipped():
    recorder = Recorder(targets=[
        _target("workshop"),
        _target("codexonly", providers=("codex",)),
        _target("mirror9", online=False),
    ])
    response = _post(recorder)
    assert response["data"]["targetNode"] == "mirror9"
    assert response["data"]["queueState"] == "waiting_for_node"
    assert recorder.payload(1)["repositoryOwner"] == "mirror9"


def test_without_a_mirror_the_handoff_waits_for_another_desktop():
    recorder = Recorder(targets=[_target("laptop", runtime="desktop")])
    response = _post(recorder)
    assert "targetNode" not in response["data"]
    assert recorder.payload(1)["repositoryOwner"] == "jett"
    assert recorder.notified == [("jett", "forkmesh", "org-agents")]


def _post_as(recorder, node, signer, **load):
    handler, _model = _load(recorder, **load)
    return asyncio.run(handler(
        object(), _request(signer=signer), node, "forkmesh", "412"))


def test_the_account_owning_the_serving_node_may_move_its_session():
    recorder = Recorder()
    response = _post_as(
        recorder, "workshop", "jett",
        keys={"jett": SIGNING_KEY}, owned={("jett", "workshop")})
    assert response["status"] == 202
    session_sql, session_args = recorder.rows()[0]
    _job_sql, job_args = recorder.rows()[1]
    assert session_args[1] == "bi:jett"
    assert session_args[6] == "bi:jett"
    assert job_args[1] == "bi:jett"
    record = recorder.payload(0)
    assert record["org"] == "workshop"
    assert record["nodeOwner"] == "workshop"
    assert record["createdBy"] == "jett"
    assert recorder.payload(1)["fromKey"] == SIGNING_KEY


def test_an_organization_owner_of_the_linked_repo_may_move_its_session():
    recorder = Recorder()
    response = _post_as(
        recorder, "workshop", "jett",
        keys={"jett": SIGNING_KEY},
        org_owned={("jett", "workshop", "forkmesh")})
    assert response["status"] == 202


def test_a_signer_with_no_claim_on_the_serving_node_is_refused():
    recorder = Recorder()
    response = _post_as(
        recorder, "workshop", "mallory", keys={"mallory": SIGNING_KEY})
    assert response["status"] == 401
    assert recorder.batches == []
    recorder = Recorder()
    response = _post_as(
        recorder, "workshop", "jett",
        keys={"mallory": SIGNING_KEY}, owned={("jett", "workshop")})
    assert response["status"] == 401
    assert recorder.batches == []
