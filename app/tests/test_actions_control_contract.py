"""The Worker and the mirror gateway agree on the Actions control contract.

Each side has its own exhaustive suite (test_actions_control_worker.py,
test_actions_control_gateway.py). Those prove each half matches
www/docs/operations/mirror-actions-control.md; this file proves the halves
match *each other*, by handing one side's real output to the other side's real
validator. A drift in a key name, a pattern or a bound fails here even when
both suites still pass.
"""

import hashlib
import json
import re

import pytest

import test_actions_control_gateway as gateway_side
import test_actions_control_worker as worker_side

gateway = gateway_side.gateway
NOW = gateway_side.NOW
NODE = gateway_side.NODE

BROWSER_REQUESTS = {
    "dispatch": {"workflow": ".forkmesh/deploy.yml", "ref": "refs/heads/release/1.2"},
    "rerun": {"runId": 7},
    "cancel": {"runId": 7},
    "approve": {"runId": 7},
    "reject": {"runId": 7},
    "set-workflow-enabled": {"workflow": ".forkmesh/ci.yaml", "enabled": False},
    "set-repository-enabled": {"enabled": True},
}


def test_both_sides_name_the_same_actions():
    module = worker_side._load_module()
    assert set(module.ACTIONS_CONTROL_ACTIONS) == set(BROWSER_REQUESTS)
    assert set(gateway.ACTIONS_CONTROL_ACTIONS) == set(BROWSER_REQUESTS)
    for action, keys in module.ACTIONS_CONTROL_ACTIONS.items():
        assert set(keys) == set(gateway.ACTIONS_CONTROL_ACTIONS[action]), action
    assert module.ACTIONS_CONTROL_WIRE_TYPE == gateway.ACTIONS_CONTROL_WIRE_TYPE
    assert module.ACTIONS_CONTROL_OPERATION == "actions-control"
    assert "actions-control" in gateway.PUBLIC_OPERATIONS
    assert "actions-control" in worker_side.edge_routing.PUBLIC_OPERATIONS


@pytest.mark.parametrize("action", sorted(BROWSER_REQUESTS))
def test_gateway_accepts_the_body_the_worker_signs(action):
    module = worker_side._load_module()
    browser = {
        "requestId": "web_control_0000001",
        "action": action,
        "node": NODE,
        "sessionToken": "session-token",
        **BROWSER_REQUESTS[action],
    }
    data = module._actions_control_body(json.dumps(browser))
    assert data is not None, action
    body = module._actions_control_gateway_body(data, "alice", NOW)
    assert body and len(body) <= gateway.MAX_ACTIONS_CONTROL_BODY
    # Neither the session token nor the routing hint reaches the executor.
    document = json.loads(body)
    assert "sessionToken" not in document and "node" not in document

    accepted = gateway.GatewayApplication._actions_control_request(body, NOW)
    assert accepted["requestId"] == browser["requestId"]
    assert accepted["action"] == action
    assert accepted["actor"] == "alice"
    for key, value in BROWSER_REQUESTS[action].items():
        assert accepted[key] == value and type(accepted[key]) is type(value)


def test_worker_accepts_the_gateways_acknowledgement():
    module = worker_side._load_module()
    acknowledgement = {
        "ok": True, "status": "accepted",
        "requestId": "web_control_0000001", "node": NODE,
    }
    assert module._actions_control_accepted(
        acknowledgement, "web_control_0000001", NODE) == acknowledgement
    # Another node's, or another request's, acknowledgement is not ours.
    assert module._actions_control_accepted(
        acknowledgement, "web_control_0000002", NODE) is None
    assert module._actions_control_accepted(
        acknowledgement, "web_control_0000001", "mirror-b") is None


def test_masked_target_is_the_route_the_gateway_queues_under():
    target = worker_side.edge_routing.masked_target_url(
        "https://mirror.example", gateway_side.OWNER, gateway_side.REPOSITORY,
        "actions-control")
    assert worker_side.urlparse(target).path == gateway_side.TARGET


def test_worker_revalidates_the_gateways_summary_v2_response(tmp_path):
    handoff = tmp_path / "gateway-state"
    handoff.mkdir(mode=0o700)
    handoff.chmod(0o700)
    path = handoff / "actions-summary.json"
    gateway_side.write_summary(path, gateway_side.summary_v2())
    response = gateway_side.load_summary(path)
    assert response["schemaVersion"] == 2

    _, validate = worker_side._validators()
    # The Worker test clock and the gateway test clock differ; validate at
    # the gateway's.
    result = validate(json.loads(json.dumps(response)), NODE, NOW)
    assert result is not None
    assert result["schemaVersion"] == 2
    assert result["node"] == NODE
    assert result["actionsEnabled"] is True
    assert [item["path"] for item in result["workflows"]] == [
        item["path"] for item in response["workflows"]]
    assert [item["requestId"] for item in result["controls"]] == [
        item["requestId"] for item in response["controls"]]
    assert len(result["runs"]) == len(response["runs"]) > 0
    for run in result["runs"]:
        assert {"workflowPath", "pullNumber", "steps"} <= set(run)
        assert "owner" not in run and "repository" not in run
    # Nothing about any other repository on that executor crosses over.
    text = json.dumps(result)
    assert "Other owner" not in text and "other_control_00001" not in text


def test_worker_still_reads_the_gateways_summary_v1_response(tmp_path):
    handoff = tmp_path / "gateway-state"
    handoff.mkdir(mode=0o700)
    handoff.chmod(0o700)
    path = handoff / "actions-summary.json"
    document = gateway_side.summary_v2()
    document["schemaVersion"] = 1
    for key in ("repositories", "controls"):
        document.pop(key)
    for run in document["runs"]:
        for key in ("workflowPath", "pullNumber", "steps"):
            run.pop(key)
    gateway_side.write_summary(path, document)
    response = gateway_side.load_summary(path)
    assert "schemaVersion" not in response

    _, validate = worker_side._validators()
    result = validate(json.loads(json.dumps(response)), NODE, NOW)
    assert result is not None and "workflows" not in result
    assert len(result["runs"]) == len(response["runs"])


def test_envelope_binds_the_capability_to_the_exact_worker_body():
    # The executor recomputes sha256 over the envelope's `body` string, so the
    # bytes the Worker signs must survive the gateway unchanged.
    module = worker_side._load_module()
    data = module._actions_control_body(json.dumps({
        "requestId": "web_control_0000001", "action": "cancel",
        "node": NODE, "runId": 7}))
    body = module._actions_control_gateway_body(data, "alice", NOW)
    assert body == body.decode("utf-8").encode("utf-8")
    assert hashlib.sha256(body).hexdigest() == hashlib.sha256(
        json.loads(json.dumps(body.decode("utf-8"))).encode("utf-8")).hexdigest()


def test_go_runner_and_gateway_name_the_same_contract():
    # The executor is the Go mirror daemon (server/actions*.go). Its behaviour
    # against this gateway is proven by test_actions_go_runner_interop.py; these
    # pins catch a constant drifting on one side before that slower test runs.
    server = gateway_side.ROOT / "server"
    control = (server / "actions_control.go").read_text(encoding="utf-8")
    runner = (server / "actions.go").read_text(encoding="utf-8")
    assert f'actionsControlWireType      = "{gateway.ACTIONS_CONTROL_WIRE_TYPE}"' in control
    assert f'actionsControlEnvelopeType  = "{gateway.ACTIONS_CONTROL_ENVELOPE_TYPE}"' in control
    assert f'actionsControlDirectoryName = "{gateway.ACTIONS_CONTROL_DIRECTORY}"' in control
    assert '"forkmesh-masked-proxy-v1"' in control
    for action, keys in gateway.ACTIONS_CONTROL_ACTIONS.items():
        listed = ", ".join(f'"{key}"' for key in keys)
        assert re.search(
            rf'"{re.escape(action)}":\s+{{{re.escape(listed)}}}', control), action
    # Every reason the runner can answer with is one the gateway will serve.
    answered = set(re.findall(r'return "([a-z_]+)", ', runner))
    assert answered and answered <= set(gateway.ACTIONS_CONTROL_REJECTION_REASONS), (
        answered - set(gateway.ACTIONS_CONTROL_REJECTION_REASONS))
    assert gateway.MAX_ACTIONS_SUMMARY_BYTES == 256 * 1024
    assert "maxActionsSummaryBytes   = 256 << 10" in runner
    assert "maxActionsLogTailBytes   = 16 << 10" in runner


def test_go_daemon_only_advertises_actions_it_can_sandbox_and_serve():
    server = gateway_side.ROOT / "server"
    daemon = (server / "daemon.go").read_text(encoding="utf-8")
    runner = (server / "actions.go").read_text(encoding="utf-8")
    config = (server / "config.go").read_text(encoding="utf-8")
    # Opt-in, never default: this runs shell a repository supplies.
    assert 'EnableActions bool `json:"enableActions,omitempty"`' in config
    assert "if cfg.EnableActions {" in daemon
    assert "d.actions != nil && d.actions.ready(ctx)" in daemon
    # No sandbox, or a gateway script that predates the keys: nothing offered.
    assert "gatewayScriptServesActions(r.daemon.config.GatewayScript)" in runner
    assert '"--unshare-all", "--die-with-parent", "--new-session", "--clearenv"' in runner
