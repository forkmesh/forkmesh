"""A real Git client pushing through the gateway under an operator policy."""

import json
from http.server import ThreadingHTTPServer
import threading

import pytest

import test_mirror_gateway as base
from test_mirror_gateway import gateway, run


def write_policy(path, **overrides):
    value = {"schemaVersion": 1, "profile": "home", **overrides}
    path.write_text(json.dumps(value), encoding="utf-8")
    path.chmod(0o600)
    return path


def make_policy_application(tmp_path, *, owners=("mobile-owner", "banned")):
    config_path, root = base.write_writable_config(tmp_path, owners=owners)
    state = tmp_path / "policy-state"
    state.mkdir(mode=0o700)
    policy = write_policy(
        tmp_path / "operator-policy.json",
        limits={"maxBlobBytes": 1024},
        content={"blockedExtensions": [".exe"]},
        repositories={"block": ["banned"]},
    )
    value = json.loads(config_path.read_text(encoding="utf-8"))
    value["operatorPolicy"] = {
        "policyPath": str(policy), "stateDirectory": str(state)}
    config_path.write_text(json.dumps(value), encoding="utf-8")
    app = gateway.GatewayApplication(
        gateway.load_config(config_path),
        verifier=base.AcceptVerifier(),
        health_signer=base.HealthSigner(),
        clock_ms=lambda: base.NOW,
        log=lambda _event: None,
    )
    return app, root, state, policy, config_path


@pytest.fixture
def served(tmp_path):
    app, root, state, policy, config_path = make_policy_application(tmp_path)
    server = ThreadingHTTPServer(("127.0.0.1", 0), base._PushGitHandler)
    server.RequestHandlerClass.application = app
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        yield {
            "app": app, "root": root, "state": state, "policy": policy,
            "config": config_path, "tmp": tmp_path,
            "origin": "http://127.0.0.1:%d" % server.server_address[1],
        }
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=5)
        app.close()


def _rev(git_dir, ref="refs/heads/main"):
    return run(["git", "--git-dir", str(git_dir), "rev-parse", ref],
               git_dir.parent)


def test_policy_rejects_disallowed_push_and_keeps_repository_intact(served):
    source = base.make_push_client(
        served["tmp"], served["origin"] + "/mobile-owner/app")
    first = base._git_push(source, "origin", "main")
    assert first.returncode == 0, first.stderr
    git_dir = served["root"] / "mobile-owner" / "app.git"
    accepted = _rev(git_dir)

    # Repository-planted hooks still never run beside the policy hook.
    marker = served["tmp"] / "planted-hook-ran"
    for hook in ("pre-receive", "update", "post-receive"):
        planted = git_dir / "hooks" / hook
        planted.write_text("#!/bin/sh\ntouch %s\n" % marker, encoding="utf-8")
        planted.chmod(0o755)

    (source / "installer.exe").write_bytes(b"MZ" * 4)
    (source / "big.bin").write_bytes(b"x" * 4096)
    run(["git", "add", "."], source)
    run(["git", "commit", "-m", "refused content"], source)
    refused = run(["git", "rev-parse", "HEAD"], source)
    pushed = base._git_push(source, "origin", "main")

    assert pushed.returncode != 0
    assert "blocked-extension" in pushed.stderr
    assert "blob-size" in pushed.stderr
    assert _rev(git_dir) == accepted
    assert not marker.exists()
    assert subprocess_ok(
        ["git", "--git-dir", str(git_dir), "fsck", "--strict"])
    assert not subprocess_ok(
        ["git", "--git-dir", str(git_dir), "cat-file", "-e", refused])
    summary = gateway.node_operator_policy.status_summary(served["state"])
    assert summary["rejectedPushes"] == 1
    assert summary["audit"]["ok"] is True

    # The permitted history keeps flowing.
    run(["git", "reset", "--hard", accepted], source)
    (source / "notes.md").write_text("fine\n", encoding="utf-8")
    run(["git", "add", "."], source)
    run(["git", "commit", "-m", "permitted"], source)
    assert base._git_push(source, "origin", "main").returncode == 0
    assert _rev(git_dir) == run(["git", "rev-parse", "HEAD"], source)


def subprocess_ok(command):
    import subprocess

    return subprocess.run(
        command, capture_output=True, check=False).returncode == 0


def test_blocked_owner_is_never_created(served):
    source = base.make_push_client(
        served["tmp"], served["origin"] + "/banned/app")
    pushed = base._git_push(source, "origin", "main")
    assert pushed.returncode != 0
    assert not (served["root"] / "banned" / "app.git").exists()


def test_quarantine_hides_repository_on_this_node_only_and_is_reversible(
    served
):
    source = base.make_push_client(
        served["tmp"], served["origin"] + "/mobile-owner/app")
    assert base._git_push(source, "origin", "main").returncode == 0
    git_dir = served["root"] / "mobile-owner" / "app.git"
    tip = _rev(git_dir)
    app = served["app"]
    policy_module = gateway.node_operator_policy

    def info_refs():
        return app.dispatch(
            "GET",
            "/v1/repositories/mobile-owner/app/git-info-refs"
            "?service=git-upload-pack",
            base.push_capability_headers(), b"")

    def health_available():
        response = app.dispatch(
            "GET",
            "/health?nonce=%s&issuedAt=%d&owner=mobile-owner&repo=app"
            % ("N" * 24, base.NOW), {}, b"")
        return json.loads(response.body)["repositoryProof"]["available"]

    assert info_refs().status == 200
    assert health_available() is True

    policy_module.quarantine_repository(
        served["state"], owner="mobile-owner", name="app",
        reason="copyright notice", actor="operator")
    hidden = info_refs()
    assert hidden.status != 200
    assert health_available() is False
    (source / "more.md").write_text("more\n", encoding="utf-8")
    run(["git", "add", "."], source)
    run(["git", "commit", "-m", "while quarantined"], source)
    assert base._git_push(source, "origin", "main").returncode != 0
    assert _rev(git_dir) == tip

    policy_module.release_repository(
        served["state"], owner="mobile-owner", name="app",
        reason="counter-notice", actor="operator")
    assert info_refs().status == 200
    assert health_available() is True
    assert base._git_push(source, "origin", "main").returncode == 0


def test_private_replicas_follow_the_seeding_policy(tmp_path):
    store = tmp_path / "private-replicas"
    store.mkdir(mode=0o700)
    opaque_id = "a" * 64
    replica = store / (opaque_id + ".fm-private")
    replica.write_bytes(base.private_replica_bytes(opaque_id))
    replica.chmod(0o600)
    state = tmp_path / "policy-state"
    state.mkdir(mode=0o700)
    policy = write_policy(tmp_path / "operator-policy.json",
                          profile="commercial")
    config_path = base.write_config(tmp_path, None, repositories=[])
    value = json.loads(config_path.read_text(encoding="utf-8"))
    value["privateReplicaStore"] = str(store)
    value["operatorPolicy"] = {
        "policyPath": str(policy), "stateDirectory": str(state)}
    config_path.write_text(json.dumps(value), encoding="utf-8")
    app = gateway.GatewayApplication(
        gateway.load_config(config_path),
        verifier=base.AcceptVerifier(),
        health_signer=base.HealthSigner(),
        clock_ms=lambda: base.NOW,
        log=lambda _event: None,
    )
    target = "/v1/private-replicas/" + opaque_id
    try:
        served = app.dispatch("GET", target, base.capability_headers(
            target, request_id="policy_replica_01"), b"")
        assert served.status == 200
        # Dropping private seeding takes effect without a restart.
        write_policy(policy, profile="commercial",
                     seeding={"privateReplicas": False})
        refused = app.dispatch("GET", target, base.capability_headers(
            target, request_id="policy_replica_02"), b"")
        assert refused.status == 404
        # An unreadable policy serves nothing it cannot check.
        policy.write_text("{", encoding="utf-8")
        broken = app.dispatch("GET", target, base.capability_headers(
            target, request_id="policy_replica_03"), b"")
        assert broken.status == 404
    finally:
        app.close()


def test_invalid_or_missing_policy_fails_gateway_validation(tmp_path):
    _app, _root, _state, policy, config_path = make_policy_application(
        tmp_path)
    _app.close()
    policy.write_text('{"schemaVersion": 1, "profile": "cloud"}')
    with pytest.raises(gateway.GatewayError, match="operatorPolicy is invalid"):
        gateway.load_config(config_path)
    value = json.loads(config_path.read_text(encoding="utf-8"))
    value["operatorPolicy"] = {"policyPath": "relative.json",
                               "stateDirectory": "/tmp"}
    config_path.write_text(json.dumps(value), encoding="utf-8")
    with pytest.raises(gateway.GatewayError, match="absolute"):
        gateway.load_config(config_path)


def test_unreadable_policy_at_push_time_refuses(served):
    served["policy"].unlink()
    source = base.make_push_client(
        served["tmp"], served["origin"] + "/mobile-owner/app")
    pushed = base._git_push(source, "origin", "main")
    assert pushed.returncode != 0
