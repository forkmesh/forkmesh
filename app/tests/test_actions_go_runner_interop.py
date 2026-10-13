"""The Python gateway and the Go mirror daemon's Actions runner, end to end.

Mirrors run the Go daemon (server/), which supervises this same Python
gateway. The two halves share one directory and no code, so each side's own
suite cannot show that they agree. This test runs both for real:

1. the Go runner publishes its first summary; the gateway must accept it
   (it refuses control requests until it can read a version 2 document);
2. the gateway couriers a router-signed dispatch request into
   actions-control/, exactly as it would for the Worker;
3. the Go runner verifies that Ed25519 capability itself, runs the workflow,
   and publishes the outcome;
4. the gateway serves that summary, and the Worker's validator accepts what
   the gateway served.

Wire contract: www/docs/operations/mirror-actions-control.md.
"""

import base64
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

import pytest
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
from cryptography.hazmat.primitives.serialization import Encoding, PublicFormat

import test_actions_control_gateway as gateway_side
import test_actions_control_worker as worker_side

gateway = gateway_side.gateway
base = gateway_side.base
ROOT = Path(__file__).resolve().parents[2]
NOW = gateway_side.NOW
NODE = gateway_side.NODE
REQUEST_ID = "interop_dispatch_0001"
STATUS_TARGET = gateway_side.TARGET.replace("actions-control", "actions-status")

pytestmark = pytest.mark.skipif(
    shutil.which("go") is None or shutil.which("git") is None,
    reason="needs the Go toolchain to run the mirror daemon's runner")


def b64url(raw):
    return base64.urlsafe_b64encode(raw).rstrip(b"=").decode("ascii")


class RouterVerifier:
    """The gateway's verifier seam, doing real Ed25519 for one router key.

    The suite's AcceptVerifier only recognises one fixed string; here the
    gateway and the Go runner must both verify the same genuine signature.
    """

    def __init__(self, public_key, *, accept_everything=False):
        self.public_key = public_key
        self.accept_everything = accept_everything

    def verify(self, message, signature):
        if self.accept_everything:
            return True
        try:
            raw = base64.urlsafe_b64decode(signature + "=" * (-len(signature) % 4))
            self.public_key.verify(raw, message.encode("utf-8"))
            return True
        except Exception:
            return False


def repin(config_path, bare):
    """Re-pin the served refs, as the Go daemon does after every sync.

    The prepare phase pushes a workflow commit; a gateway still holding the
    old digest would refuse the repository as tampered with.
    """
    value = json.loads(config_path.read_text(encoding="utf-8"))
    value["repositories"][0]["integrity"]["expectedRefsSha256"] = (
        gateway.refs_sha256(bare))
    config_path.write_text(json.dumps(value), encoding="utf-8")


def run_go_phase(phase, handoff, bare, router_public):
    env = {
        **os.environ,
        "FORKMESH_ACTIONS_INTEROP_DIR": str(handoff),
        "FORKMESH_ACTIONS_INTEROP_PHASE": phase,
        "FORKMESH_ACTIONS_INTEROP_NODE": NODE,
        "FORKMESH_ACTIONS_INTEROP_ROUTER": router_public,
        "FORKMESH_ACTIONS_INTEROP_BARE": str(bare),
        "FORKMESH_ACTIONS_INTEROP_NOW": str(NOW),
    }
    result = subprocess.run(
        ["go", "test", "-count=1", "-run",
         "^TestActionsInteropWithThePythonGateway$", "."],
        cwd=ROOT / "server", env=env, capture_output=True, text=True,
        timeout=600)
    assert result.returncode == 0, result.stdout[-4000:] + result.stderr[-2000:]
    # A skip would mean the environment never reached the Go test.
    assert "no tests to run" not in result.stdout


def test_gateway_and_go_runner_complete_a_web_dispatch(tmp_path):
    config_path, bare, handoff, control = gateway_side.configure(tmp_path)
    router = Ed25519PrivateKey.generate()
    router_public = b64url(router.public_key().public_bytes(
        Encoding.Raw, PublicFormat.Raw))

    # 1. The Go runner's first summary is a document this gateway reads.
    run_go_phase("prepare", handoff, bare, router_public)
    repin(config_path, bare)
    summary_path = handoff / "actions-summary.json"
    assert oct(summary_path.stat().st_mode & 0o777) == "0o600"
    first = gateway_side.load_summary(summary_path)
    assert first["schemaVersion"] == 2 and first["node"] == NODE
    assert first["actionsEnabled"] is True and first["runs"] == []
    (workflow,) = first["workflows"]
    assert workflow == {
        "path": ".forkmesh/ci.yml", "name": "CI",
        "events": ["push", "workflow_dispatch"], "manual": True,
        "enabled": True, "runsOn": [], "valid": True,
    }

    # 2. The gateway couriers a dispatch the router really signed.
    events = []
    app = gateway.GatewayApplication(
        gateway.load_config(config_path),
        verifier=RouterVerifier(router.public_key()),
        health_signer=base.HealthSigner(),
        materializer=base.FakeArchiveMaterializer(bare),
        clock_ms=lambda: NOW,
        log=events.append,
    )
    try:
        body = gateway_side.encode(gateway_side.control_body(
            "dispatch", requestId=REQUEST_ID,
            workflow=".forkmesh/ci.yml", ref="refs/heads/main"))
        capability_id = "capability_interop_01"
        message = gateway.request_message(
            NODE, "POST", gateway_side.TARGET,
            hashlib.sha256(body).hexdigest(), capability_id, NOW)
        response, _ = gateway_side.post(app, body, headers={
            "X-ForkMesh-Request-Id": capability_id,
            "X-ForkMesh-Signature": b64url(router.sign(message.encode("utf-8"))),
        })
        assert response.status == 202, json.dumps(events)
        (envelope_path,) = control.glob("*.json")
        assert envelope_path.name == REQUEST_ID + ".json"

        # 3. The Go runner verifies the capability itself and runs it.
        run_go_phase("consume", handoff, bare, router_public)
        assert list(control.glob("*.json")) == [], "consumed exactly once"

        # 4. The gateway serves what the runner wrote...
        served = json.loads(app.dispatch(
            "GET", STATUS_TARGET,
            {
                "X-ForkMesh-Node": NODE,
                "X-ForkMesh-Request-Id": "capability_interop_02",
                "X-ForkMesh-Issued-At": str(NOW),
                "X-ForkMesh-Body-Sha256": hashlib.sha256(b"").hexdigest(),
                "X-ForkMesh-Signature": b64url(router.sign(
                    gateway.request_message(
                        NODE, "GET", STATUS_TARGET,
                        hashlib.sha256(b"").hexdigest(),
                        "capability_interop_02", NOW).encode("utf-8"))),
            }, b"").body)
    finally:
        app.close()

    (outcome,) = served["controls"]
    assert outcome["requestId"] == REQUEST_ID
    assert (outcome["action"], outcome["status"], outcome["reason"]) == (
        "dispatch", "applied", "")
    (run,) = served["runs"]
    assert run["id"] == outcome["runId"]
    assert run["status"] == "success" and run["workflow"] == "CI"
    assert run["workflowPath"] == ".forkmesh/ci.yml"
    assert run["ref"] == "refs/heads/main" and run["pullNumber"] == 0
    assert run["steps"] == [
        {"name": "Greet", "status": "success"},
        {"name": "Second", "status": "success"},
    ]
    assert "hello from alice/project at refs/heads/main" in run["logTail"]
    assert "owner" not in run and "repository" not in run

    # ...and the Worker accepts what the gateway served.
    _, validate = worker_side._validators()
    checked = validate(json.loads(json.dumps(served)), NODE, NOW)
    assert checked is not None
    assert checked["runs"][0]["steps"] == run["steps"]
    assert checked["controls"][0]["requestId"] == REQUEST_ID


def test_go_runner_refuses_a_request_the_router_did_not_sign(tmp_path):
    config_path, bare, handoff, control = gateway_side.configure(tmp_path)
    router = Ed25519PrivateKey.generate()
    router_public = b64url(router.public_key().public_bytes(
        Encoding.Raw, PublicFormat.Raw))
    run_go_phase("prepare", handoff, bare, router_public)
    repin(config_path, bare)
    app = gateway.GatewayApplication(
        gateway.load_config(config_path),
        # A gateway that was talked into it: the courier is not the authority.
        verifier=RouterVerifier(router.public_key(), accept_everything=True),
        health_signer=base.HealthSigner(),
        materializer=base.FakeArchiveMaterializer(bare),
        clock_ms=lambda: NOW,
        log=[].append,
    )
    try:
        body = gateway_side.encode(gateway_side.control_body(
            "dispatch", requestId=REQUEST_ID,
            workflow=".forkmesh/ci.yml", ref="refs/heads/main"))
        forged = Ed25519PrivateKey.generate()
        message = gateway.request_message(
            NODE, "POST", gateway_side.TARGET,
            hashlib.sha256(body).hexdigest(), "capability_interop_03", NOW)
        response, _ = gateway_side.post(app, body, headers={
            "X-ForkMesh-Request-Id": "capability_interop_03",
            "X-ForkMesh-Signature": b64url(forged.sign(message.encode("utf-8"))),
        })
        assert response.status == 202
    finally:
        app.close()
    run_go_phase("consume", handoff, bare, router_public)
    assert list(control.glob("*.json")) == []
    summary = gateway_side.load_summary(handoff / "actions-summary.json")
    # Nothing ran, and nothing is reported: an unverified file names no
    # request the executor is willing to answer.
    assert summary["runs"] == [] and summary["controls"] == []
