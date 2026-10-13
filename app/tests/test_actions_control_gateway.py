"""Mirror gateway half of Actions control and the version 2 summary.

Wire contract: www/docs/operations/mirror-actions-control.md.
"""

import hashlib
import itertools
import json
import os
from pathlib import Path
import stat

import pytest

import test_mirror_gateway as base
from test_mirror_gateway import gateway
from test_headless_mirror_refresh import (  # noqa: F401
    installation,
    refresh_tool,
)


ROOT = Path(__file__).resolve().parents[2]
NOW = base.NOW
NODE = "mirror-a"
OWNER = "alice"
REPOSITORY = "project"
TARGET = f"/v1/repositories/{OWNER}/{REPOSITORY}/actions-control"
COMMIT = "a" * 40
REQUEST_ID = "web_control_0000001"
_capability_ids = itertools.count(1)


# --------------------------------------------------------------------------
# Fixtures and builders.
# --------------------------------------------------------------------------


def summary_v1(**changes):
    value = {
        "schemaVersion": 1,
        "type": "forkmesh.mirror-actions-summary",
        "node": NODE,
        "updatedAt": NOW - 1000,
        "expiresAt": NOW + 9 * 60 * 1000,
        "runs": [
            {
                "id": 7,
                "owner": OWNER,
                "repository": REPOSITORY,
                "workflow": "Deploy",
                "commit": COMMIT,
                "ref": "refs/heads/main",
                "status": "success",
                "createdAt": NOW - 5000,
                "startedAt": NOW - 4000,
                "finishedAt": NOW - 2000,
                "logTail": "All steps completed.",
            },
            {
                "id": 8,
                "owner": "other",
                "repository": REPOSITORY,
                "workflow": "Other owner",
                "commit": "b" * 40,
                "ref": "refs/heads/main",
                "status": "queued",
                "createdAt": NOW - 1000,
                "startedAt": 0,
                "finishedAt": 0,
                "logTail": "",
            },
        ],
    }
    value.update(changes)
    return value


def workflow_entry(**changes):
    value = {
        "path": ".forkmesh/deploy.yml",
        "name": "Deploy",
        "events": ["push", "workflow_dispatch"],
        "manual": True,
        "enabled": True,
        "runsOn": ["linux", "x64"],
        "valid": True,
    }
    value.update(changes)
    return value


def control_entry(**changes):
    value = {
        "owner": OWNER,
        "repository": REPOSITORY,
        "requestId": REQUEST_ID,
        "action": "dispatch",
        "status": "applied",
        "reason": "",
        "runId": 7,
        "finishedAt": NOW - 1500,
    }
    value.update(changes)
    return value


def summary_v2(**changes):
    value = summary_v1(schemaVersion=2)
    value["runs"][0].update({
        "workflowPath": ".forkmesh/deploy.yml",
        "pullNumber": 12,
        "steps": [
            {"name": "Checkout", "status": "success"},
            {"name": "", "status": "skipped"},
        ],
    })
    value["runs"][1].update({
        "workflowPath": "",
        "pullNumber": 0,
        "steps": [],
    })
    value["repositories"] = [
        {
            "owner": OWNER,
            "repository": REPOSITORY,
            "actionsEnabled": True,
            "workflows": [
                workflow_entry(),
                workflow_entry(
                    path=".forkmesh/broken.yaml",
                    name="",
                    events=[],
                    manual=False,
                    runsOn=[],
                    valid=False,
                ),
            ],
        },
        {
            "owner": "other",
            "repository": REPOSITORY,
            "actionsEnabled": False,
            "workflows": [workflow_entry(name="Other owner")],
        },
    ]
    value["controls"] = [
        control_entry(),
        control_entry(
            owner="other", requestId="other_control_00001", runId=8),
        control_entry(
            action="cancel",
            status="rejected",
            reason="not_cancellable",
            finishedAt=NOW - 3000,
        ),
    ]
    value.update(changes)
    return value


def write_summary(path, value):
    path.write_text(
        json.dumps(value, sort_keys=True, separators=(",", ":")),
        encoding="utf-8",
    )
    path.chmod(0o600)


def load_summary(path, *, owner=OWNER, repository=REPOSITORY):
    return gateway._load_actions_summary(
        path, node=NODE, owner=owner, repository=repository, now_ms=NOW)


def control_body(action="dispatch", /, **changes):
    value = {
        "schemaVersion": 1,
        "type": "forkmesh.actions-control-v1",
        "requestId": REQUEST_ID,
        "action": action,
        "actor": "alice",
        "issuedAt": NOW - 500,
    }
    value.update({
        "dispatch": {
            "workflow": ".forkmesh/deploy.yml", "ref": "refs/heads/main"},
        "rerun": {"runId": 7},
        "cancel": {"runId": 7},
        "approve": {"runId": 7},
        "reject": {"runId": 7},
        "set-workflow-enabled": {
            "workflow": ".forkmesh/deploy.yml", "enabled": False},
        "set-repository-enabled": {"enabled": True},
    }[action])
    value.update(changes)
    return value


def encode(value):
    return json.dumps(value, separators=(",", ":")).encode("utf-8")


def configure(tmp_path, *, operations=None, extra=None):
    """Write a gateway configuration with the Actions handoff beside it."""

    bare, _commit = base.make_bare_repository(tmp_path)
    (tmp_path / "releases").mkdir(exist_ok=True)
    handoff = tmp_path / "gateway-state"
    handoff.mkdir(mode=0o700)
    handoff.chmod(0o700)
    control = handoff / "actions-control"
    control.mkdir(mode=0o700)
    control.chmod(0o700)
    config_path = base.write_config(tmp_path, bare)
    value = json.loads(config_path.read_text(encoding="utf-8"))
    value["actionsSummaryPath"] = str(handoff / "actions-summary.json")
    value["actionsControlPath"] = str(control)
    value["repositories"][0]["operations"] = (
        sorted(gateway.DEFAULT_PUBLIC_OPERATIONS)
        + ["actions-status", "actions-control"]
        if operations is None else operations
    )
    value.update(extra or {})
    for key in [key for key, item in value.items() if item is None]:
        del value[key]
    config_path.write_text(json.dumps(value), encoding="utf-8")
    return config_path, bare, handoff, control


@pytest.fixture
def control_app(tmp_path):
    config_path, bare, handoff, control = configure(tmp_path)
    write_summary(handoff / "actions-summary.json", summary_v2())
    clock = {"now": NOW}
    app = gateway.GatewayApplication(
        gateway.load_config(config_path),
        verifier=base.AcceptVerifier(),
        health_signer=base.HealthSigner(),
        materializer=base.FakeArchiveMaterializer(bare),
        clock_ms=lambda: clock["now"],
        log=[].append,
    )
    yield app, handoff, control, clock
    app.close()


def post(app, body, *, target=TARGET, method="POST", headers=None, now=NOW):
    supplied = {
        "X-ForkMesh-Node": NODE,
        "X-ForkMesh-Request-Id": f"capability_{next(_capability_ids):06d}",
        "X-ForkMesh-Issued-At": str(now),
        "X-ForkMesh-Body-Sha256": hashlib.sha256(body).hexdigest(),
        "X-ForkMesh-Signature": base.SIGNATURE,
        "Content-Type": "application/json",
    }
    supplied.update(headers or {})
    return app.dispatch(method, target, supplied, body), supplied


def pending(control):
    return sorted(entry.name for entry in control.iterdir())


# --------------------------------------------------------------------------
# Operation registration.
# --------------------------------------------------------------------------


def test_operation_is_explicit_and_never_a_public_read():
    assert "actions-control" in gateway.PUBLIC_OPERATIONS
    assert "actions-control" in gateway.LOG_OPERATIONS
    assert "actions-control" not in gateway.DEFAULT_PUBLIC_OPERATIONS
    assert "actions-control" not in gateway.DIRECT_PUBLIC_READ_OPERATIONS
    assert "actions-control" not in gateway.WRITABLE_REPOSITORY_OPERATIONS
    assert gateway.OPERATION_QUERY_FIELDS["actions-control"] == frozenset()
    assert "actions-control" not in gateway.WEBSITE_COUNTER_OPERATIONS
    assert "actions-control" in refresh_tool.PUBLIC_OPERATIONS
    assert (
        "actions-control"
        in refresh_tool.DERIVED_REPOSITORY_EXCLUDED_OPERATIONS
    )
    source = (ROOT / "tools" / "mirror_gateway.py").read_text(
        encoding="utf-8")
    alias = source[
        source.index('match = re.fullmatch(r"/api/repo/([^/]+)/([^/]+)/([^/]+)"'):
        source.index('if operation == "mirrors" and method in')
    ]
    # The dashboard alias forwards DIRECT_PUBLIC_READ_OPERATIONS only, and
    # only as GET/HEAD against the /v1/public route.
    assert "operation in DIRECT_PUBLIC_READ_OPERATIONS and method in" in alias
    assert "actions-control" not in alias


def test_route_needs_capability_post_exact_target_and_bounded_body(
    control_app,
):
    app, _handoff, control, _clock = control_app
    body = encode(control_body())

    unsigned, _ = post(
        app, body, headers={"X-ForkMesh-Signature": "A" * 86})
    assert unsigned.status == 404
    direct, _ = post(
        app,
        body,
        target=f"/v1/public/repositories/{OWNER}/{REPOSITORY}/actions-control",
    )
    assert direct.status == 404
    read, _ = post(app, b"", method="GET")
    assert read.status == 404
    with_query, _ = post(app, body, target=TARGET + "?ref=main")
    assert with_query.status == 404
    # The executor rebuilds the signed message from the canonical route, so
    # another spelling of the same repository is refused rather than queued.
    shouting, _ = post(
        app,
        body,
        target=f"/v1/repositories/ALICE/{REPOSITORY}/actions-control",
    )
    assert shouting.status == 404
    bare_query, _ = post(app, body, target=TARGET + "?")
    assert bare_query.status == 404
    empty, _ = post(app, b"")
    assert empty.status == 404
    padded = encode(control_body()) + b" " * gateway.MAX_ACTIONS_CONTROL_BODY
    too_large, _ = post(app, padded)
    assert too_large.status == 404
    unknown_repository, _ = post(
        app,
        body,
        target=f"/v1/repositories/{OWNER}/missing/actions-control",
    )
    assert unknown_repository.status == 404
    assert pending(control) == []

    accepted, _ = post(app, body)
    assert accepted.status == 202
    assert pending(control) == [REQUEST_ID + ".json"]


def test_repository_without_the_operation_is_not_found(tmp_path):
    config_path, bare, handoff, control = configure(
        tmp_path,
        operations=sorted(gateway.DEFAULT_PUBLIC_OPERATIONS)
        + ["actions-status"],
    )
    write_summary(handoff / "actions-summary.json", summary_v2())
    app = gateway.GatewayApplication(
        gateway.load_config(config_path),
        verifier=base.AcceptVerifier(),
        health_signer=base.HealthSigner(),
        materializer=base.FakeArchiveMaterializer(bare),
        clock_ms=lambda: NOW,
        log=[].append,
    )
    try:
        response, _ = post(app, encode(control_body()))
        assert response.status == 404
        assert pending(control) == []
    finally:
        app.close()


# --------------------------------------------------------------------------
# Body validation.
# --------------------------------------------------------------------------


ACTIONS = (
    "dispatch",
    "rerun",
    "cancel",
    "approve",
    "reject",
    "set-workflow-enabled",
    "set-repository-enabled",
)


def validate(value):
    body = value if isinstance(value, bytes) else encode(value)
    return gateway.GatewayApplication._actions_control_request(body, NOW)


@pytest.mark.parametrize("action", ACTIONS)
def test_each_action_requires_exactly_its_keys(action):
    assert set(gateway.ACTIONS_CONTROL_ACTIONS) == set(ACTIONS)
    valid = control_body(action)
    assert validate(valid) == valid
    every_extra = {"workflow", "ref", "runId", "enabled", "node", "sessionToken"}
    for missing in valid:
        value = dict(valid)
        del value[missing]
        with pytest.raises(gateway.GatewayError, match="invalid"):
            validate(value)
    samples = {
        "workflow": ".forkmesh/deploy.yml",
        "ref": "refs/heads/main",
        "runId": 7,
        "enabled": True,
        "node": NODE,
        "sessionToken": "secret",
    }
    for extra in every_extra - set(valid):
        with pytest.raises(gateway.GatewayError, match="invalid"):
            validate({**valid, extra: samples[extra]})


@pytest.mark.parametrize("changes", [
    {"schemaVersion": 2},
    {"schemaVersion": True},
    {"schemaVersion": 1.0},
    {"type": "forkmesh.actions-control-v2"},
    {"action": "delete"},
    {"action": ["dispatch"]},
    {"requestId": "short"},
    {"requestId": "x" * 65},
    {"requestId": "has.dot_in_request_id"},
    {"requestId": 1234567890123456},
    {"actor": "Alice"},
    {"actor": ""},
    {"actor": "-alice"},
    {"actor": "a" * 65},
    {"issuedAt": str(NOW)},
    {"issuedAt": True},
    {"issuedAt": float(NOW)},
    {"issuedAt": NOW - 60_001},
    {"issuedAt": NOW + 60_001},
    {"workflow": "deploy.yml"},
    {"workflow": ".forkmesh/nested/deploy.yml"},
    {"workflow": ".forkmesh/deploy.json"},
    {"workflow": ".forkmesh/" + "w" * 97 + ".yml"},
    {"workflow": ".forkmesh/deploy.yml\n"},
    {"workflow": 7},
    {"ref": "main"},
    {"ref": "refs/tags/v1"},
    {"ref": "refs/heads/"},
    {"ref": "refs/heads/feature/"},
    {"ref": "refs/heads/a..b"},
    {"ref": "refs/heads/a//b"},
    {"ref": "refs/heads/main.lock"},
    {"ref": "refs/heads/has space"},
    {"ref": "refs/heads/" + "r" * 201},
    {"ref": "refs/heads/main\n"},
    {"ref": None},
])
def test_dispatch_body_rejects_bad_values(changes):
    with pytest.raises(gateway.GatewayError, match="invalid"):
        validate(control_body("dispatch", **changes))


def test_issued_at_window_is_inclusive_sixty_seconds():
    for issued_at in (NOW - 60_000, NOW, NOW + 60_000):
        assert validate(control_body(issuedAt=issued_at))["issuedAt"] == issued_at


@pytest.mark.parametrize("run_id", [0, -1, (1 << 53), "7", 7.0, True, None])
def test_run_id_is_a_positive_safe_integer(run_id):
    assert validate(control_body("rerun", runId=(1 << 53) - 1))
    with pytest.raises(gateway.GatewayError, match="invalid"):
        validate(control_body("rerun", runId=run_id))


@pytest.mark.parametrize("enabled", [0, 1, "true", None])
def test_enabled_is_a_boolean(enabled):
    for action in ("set-workflow-enabled", "set-repository-enabled"):
        with pytest.raises(gateway.GatewayError, match="invalid"):
            validate(control_body(action, enabled=enabled))


@pytest.mark.parametrize("body", [
    b"",
    b"[]",
    b"null",
    b"{",
    b"\xff\xfe",
    b'{"action":"rerun","action":"rerun"}',
])
def test_body_must_be_one_duplicate_free_json_object(body):
    with pytest.raises(gateway.GatewayError, match="invalid"):
        validate(body)
    duplicate = encode(control_body("rerun"))[:-1] + b',"runId":7}'
    with pytest.raises(gateway.GatewayError, match="invalid"):
        validate(duplicate)


# --------------------------------------------------------------------------
# Envelope, idempotency, bounds.
# --------------------------------------------------------------------------


def test_accepted_request_is_an_atomic_verbatim_envelope(control_app):
    app, _handoff, control, _clock = control_app
    # Whitespace and key order are the Worker's; the signed hash covers them.
    body = json.dumps(control_body(), indent=1).encode("utf-8")
    response, headers = post(app, body)
    assert response.status == 202
    assert response.headers["Cache-Control"] == "no-store"
    assert json.loads(response.body) == {
        "ok": True,
        "status": "accepted",
        "requestId": REQUEST_ID,
        "node": NODE,
    }
    assert str(control) not in response.body.decode()

    assert pending(control) == [REQUEST_ID + ".json"]
    path = control / (REQUEST_ID + ".json")
    info = path.lstat()
    assert stat.S_ISREG(info.st_mode)
    assert stat.S_IMODE(info.st_mode) == 0o640
    assert info.st_nlink == 1
    assert info.st_size <= gateway.MAX_ACTIONS_CONTROL_ENVELOPE_BYTES
    assert info.st_mtime_ns // 1_000_000 == NOW
    envelope = json.loads(path.read_text(encoding="utf-8"))
    assert list(envelope) == [
        "schemaVersion", "type", "node", "owner", "repository",
        "receivedAt", "capability", "body",
    ]
    assert envelope == {
        "schemaVersion": 1,
        "type": "forkmesh.mirror-actions-control",
        "node": NODE,
        "owner": OWNER,
        "repository": REPOSITORY,
        "receivedAt": NOW,
        "capability": {
            "method": "POST",
            "path": TARGET,
            "requestId": headers["X-ForkMesh-Request-Id"],
            "issuedAt": NOW,
            "bodySha256": hashlib.sha256(body).hexdigest(),
            "signature": base.SIGNATURE,
        },
        "body": body.decode("utf-8"),
    }
    # What the executor recomputes before trusting anything in the body.
    assert (
        hashlib.sha256(envelope["body"].encode("utf-8")).hexdigest()
        == envelope["capability"]["bodySha256"]
    )
    assert gateway.request_message(
        envelope["node"],
        envelope["capability"]["method"],
        envelope["capability"]["path"],
        envelope["capability"]["bodySha256"],
        envelope["capability"]["requestId"],
        envelope["capability"]["issuedAt"],
    ) == app.verifier.messages[-1][0]


def test_failed_write_leaves_no_temporary_file(control_app, monkeypatch):
    app, _handoff, control, _clock = control_app

    def refuse(*_args, **_kwargs):
        raise OSError("disk full")

    monkeypatch.setattr(gateway.os, "fsync", refuse)
    response, _ = post(app, encode(control_body()))
    assert response.status == 404
    monkeypatch.undo()
    assert pending(control) == []


def test_replay_is_idempotent_only_for_identical_bytes(control_app):
    app, _handoff, control, _clock = control_app
    body = encode(control_body())
    first, first_headers = post(app, body)
    assert first.status == 202
    path = control / (REQUEST_ID + ".json")
    written = path.read_bytes()
    inode = path.lstat().st_ino

    again, _ = post(app, body)
    assert again.status == 202
    assert json.loads(again.body) == json.loads(first.body)
    assert path.read_bytes() == written
    assert path.lstat().st_ino == inode
    assert (
        json.loads(written)["capability"]["requestId"]
        == first_headers["X-ForkMesh-Request-Id"]
    )

    different, _ = post(app, encode(control_body(ref="refs/heads/other")))
    assert different.status == 404
    respaced, _ = post(app, json.dumps(control_body()).encode("utf-8"))
    assert respaced.status == 404
    assert path.read_bytes() == written
    assert pending(control) == [REQUEST_ID + ".json"]
    with pytest.raises(gateway.GatewayError, match="already used"):
        app._actions_control(
            OWNER,
            REPOSITORY,
            encode(control_body(ref="refs/heads/other")),
            json.loads(written)["capability"],
        )

    # A planted link or unreadable document under the id is never trusted
    # as a previous acceptance, and never followed.
    outside = control.parent / "outside.json"
    outside.write_bytes(written)
    other_id = "web_control_0000002"
    link = control / (other_id + ".json")
    link.symlink_to(outside)
    os.utime(
        link, ns=(NOW * 1_000_000, NOW * 1_000_000), follow_symlinks=False)
    linked, _ = post(app, encode(control_body(requestId=other_id)))
    assert linked.status == 404
    assert link.is_symlink()
    assert outside.read_bytes() == written


def test_queue_is_bounded_and_expired_files_are_swept_first(control_app):
    app, _handoff, control, clock = control_app
    for index in range(gateway.MAX_ACTIONS_CONTROL_PENDING):
        response, _ = post(
            app, encode(control_body(requestId=f"web_control_{index:07d}")))
        assert response.status == 202
    assert len(pending(control)) == 32

    overflow = encode(control_body(requestId="web_control_overflow"))
    with pytest.raises(gateway.GatewayError, match="control queue is full"):
        app._actions_control(OWNER, REPOSITORY, overflow, {
            "method": "POST", "path": TARGET, "requestId": "capability_x",
            "issuedAt": NOW, "bodySha256": "0" * 64,
            "signature": base.SIGNATURE,
        })
    full, _ = post(app, overflow)
    assert full.status == 404
    # A pending id is still answered while the queue is full.
    replay, _ = post(
        app, encode(control_body(requestId="web_control_0000003")))
    assert replay.status == 202
    assert len(pending(control)) == 32

    abandoned = control / ".web_control_0000001.0011223344556677.tmp"
    abandoned.write_bytes(b"{}")
    os.utime(abandoned, ns=(NOW * 1_000_000, NOW * 1_000_000))
    keep = control / "operator-note.txt"
    keep.write_text("not the gateway's", encoding="utf-8")
    os.utime(keep, ns=(NOW * 1_000_000, NOW * 1_000_000))

    # One millisecond short of expiry nothing is removed.
    clock["now"] = NOW + gateway.ACTIONS_CONTROL_EXPIRY_MS
    write_summary(
        control.parent / "actions-summary.json",
        summary_v2(
            updatedAt=clock["now"] - 1000,
            expiresAt=clock["now"] + 9 * 60 * 1000,
        ),
    )
    still_full, _ = post(
        app,
        encode(control_body(
            requestId="web_control_overflow", issuedAt=clock["now"])),
        now=clock["now"],
    )
    assert still_full.status == 404
    assert len(pending(control)) == 34

    clock["now"] += 1
    swept, _ = post(
        app,
        encode(control_body(
            requestId="web_control_overflow", issuedAt=clock["now"])),
        now=clock["now"],
    )
    assert swept.status == 202
    assert pending(control) == [
        "operator-note.txt", "web_control_overflow.json"]


def test_refuses_while_no_current_v2_summary_shows_an_executor(control_app):
    app, handoff, control, _clock = control_app
    summary = handoff / "actions-summary.json"
    body = encode(control_body())
    capability = {
        "method": "POST", "path": TARGET, "requestId": "capability_x",
        "issuedAt": NOW, "bodySha256": hashlib.sha256(body).hexdigest(),
        "signature": base.SIGNATURE,
    }

    summary.unlink()
    with pytest.raises(gateway.GatewayError, match="executor is unavailable"):
        app._actions_control(OWNER, REPOSITORY, body, capability)
    offline, _ = post(app, body)
    assert offline.status == 404

    for stale in (
        summary_v2(expiresAt=NOW),
        summary_v2(node="mirror-b"),
        summary_v2(controls=[control_entry(status="done")]),
        # A version 1 executor predates control requests.
        summary_v1(),
    ):
        write_summary(summary, stale)
        with pytest.raises(
                gateway.GatewayError, match="executor is unavailable"):
            app._actions_control(OWNER, REPOSITORY, body, capability)
    assert pending(control) == []

    write_summary(summary, summary_v2())
    assert app._actions_control(
        OWNER, REPOSITORY, body, capability).status == 202


def test_queue_directory_is_rechecked_on_every_request(control_app):
    app, _handoff, control, _clock = control_app
    body = encode(control_body())
    control.chmod(0o750)
    loosened, _ = post(app, body)
    assert loosened.status == 404
    assert pending(control) == []
    control.chmod(0o700)

    real = control.with_name("actions-control-real")
    control.rename(real)
    control.symlink_to(real, target_is_directory=True)
    linked, _ = post(app, body)
    assert linked.status == 404
    assert pending(real) == []


# --------------------------------------------------------------------------
# Configuration policy.
# --------------------------------------------------------------------------


def test_config_accepts_the_protected_queue_and_binds_it_to_the_process(
    tmp_path,
):
    config_path, _bare, _handoff, control = configure(tmp_path)
    config = gateway.load_config(config_path)
    assert config.actions_control_path == control
    assert "actions-control" in config.repositories[0].operations
    source = (ROOT / "tools" / "mirror_gateway.py").read_text(
        encoding="utf-8")
    reload_guard = source[
        source.index("def reload_from_disk"):
        source.index('"gateway reload changed a process-bound setting"')
    ]
    assert "new_config.actions_control_path" in reload_guard


def test_config_requires_actions_status_and_a_control_path(tmp_path):
    config_path, *_ = configure(
        tmp_path,
        operations=sorted(gateway.DEFAULT_PUBLIC_OPERATIONS)
        + ["actions-control"],
    )
    with pytest.raises(
            gateway.GatewayError,
            match="actions-control requires actions-status"):
        gateway.load_config(config_path)


def test_config_requires_the_control_path(tmp_path):
    config_path, *_ = configure(tmp_path, extra={"actionsControlPath": None})
    with pytest.raises(
            gateway.GatewayError,
            match="actions-control requires a protected actionsControlPath"):
        gateway.load_config(config_path)


@pytest.mark.parametrize("case", [
    "relative", "unnormalized", "wrong-name", "missing", "file",
    "mode", "symlink", "symlink-parent", "not-string",
])
def test_config_rejects_every_unprotected_control_path(tmp_path, case):
    config_path, _bare, handoff, control = configure(tmp_path)
    value: object = str(control)
    if case == "relative":
        value = "gateway-state/actions-control"
    elif case == "unnormalized":
        value = str(handoff) + "/./actions-control"
    elif case == "wrong-name":
        other = handoff / "control"
        other.mkdir(mode=0o700)
        value = str(other)
    elif case == "missing":
        control.rmdir()
    elif case == "file":
        control.rmdir()
        control.write_text("{}", encoding="utf-8")
    elif case == "mode":
        control.chmod(0o750)
    elif case == "symlink":
        real = handoff / "real"
        control.rename(real)
        control.symlink_to(real, target_is_directory=True)
    elif case == "symlink-parent":
        alias = tmp_path / "gateway-alias"
        alias.symlink_to(handoff, target_is_directory=True)
        value = str(alias / "actions-control")
    elif case == "not-string":
        value = ["actions-control"]
    document = json.loads(config_path.read_text(encoding="utf-8"))
    document["actionsControlPath"] = value
    config_path.write_text(json.dumps(document), encoding="utf-8")
    with pytest.raises(gateway.GatewayError, match="actionsControlPath"):
        gateway.load_config(config_path)


def test_control_directory_policy_is_the_exact_service_owned_0700(tmp_path):
    from types import SimpleNamespace

    good = SimpleNamespace(
        st_mode=stat.S_IFDIR | 0o700, st_uid=991, st_gid=992)
    assert gateway._actions_parent_metadata_allowed(
        good, effective_uid=991, effective_gid=992)
    for changed in (
        SimpleNamespace(st_mode=stat.S_IFDIR | 0o700, st_uid=0, st_gid=992),
        SimpleNamespace(st_mode=stat.S_IFDIR | 0o700, st_uid=991, st_gid=0),
        SimpleNamespace(st_mode=stat.S_IFDIR | 0o770, st_uid=991, st_gid=992),
        SimpleNamespace(st_mode=stat.S_IFREG | 0o700, st_uid=991, st_gid=992),
    ):
        assert not gateway._actions_parent_metadata_allowed(
            changed, effective_uid=991, effective_gid=992)


def test_schemas_describe_the_new_key_operation_and_envelope():
    docs = ROOT / "www" / "docs"
    config_schema = json.loads(
        (docs / "mirror-gateway-config.schema.json").read_text("utf-8"))
    assert "actionsControlPath" in config_schema["properties"]
    assert '"actions-control"' in json.dumps(config_schema)
    refresh_schema = json.loads(
        (docs / "headless-mirror-refresh.schema.json").read_text("utf-8"))
    operations = refresh_schema["properties"]["operations"]
    assert "actions-control" in operations["items"]["enum"]
    assert operations["maxItems"] == len(refresh_tool.PUBLIC_OPERATIONS)
    envelope = json.loads(
        (docs / "mirror-actions-control.schema.json").read_text("utf-8"))
    assert envelope["properties"]["type"]["const"] == (
        gateway.ACTIONS_CONTROL_ENVELOPE_TYPE)
    assert set(envelope["required"]) == {
        "schemaVersion", "type", "node", "owner", "repository",
        "receivedAt", "capability", "body",
    }
    request = envelope["$defs"]["request"]
    assert set(request["properties"]["action"]["enum"]) == set(
        gateway.ACTIONS_CONTROL_ACTIONS)
    assert request["properties"]["type"]["const"] == (
        gateway.ACTIONS_CONTROL_WIRE_TYPE)
    summary = json.loads(
        (docs / "mirror-actions-summary.schema.json").read_text("utf-8"))
    assert summary["properties"]["schemaVersion"] == {"enum": [1, 2]}
    assert set(summary["$defs"]["control"]["properties"]["reason"]["enum"]) == (
        gateway.ACTIONS_CONTROL_REJECTION_REASONS | {""})
    assert set(summary["$defs"]["step"]["properties"]["status"]["enum"]) == (
        gateway.ACTIONS_STEP_STATUSES)
    operations_doc = (
        docs / "operations" / "direct-https-mirror-gateway.md"
    ).read_text("utf-8")
    assert "| `actions-control` | POST |" in operations_doc
    assert "(mirror-actions-control.md)" in operations_doc


# --------------------------------------------------------------------------
# Headless refresh rendering.
# --------------------------------------------------------------------------


def _refresh_config(installation, operations, name):  # noqa: F811
    configured = json.loads(json.dumps(installation["config_value"]))
    configured["operations"].extend(operations)
    path = installation["config_path"].with_name(name)
    path.write_text(
        json.dumps(configured, sort_keys=True, separators=(",", ":")) + "\n",
        encoding="utf-8",
    )
    path.chmod(0o600)
    return path


def test_refresh_renders_and_prepares_the_adjacent_control_directory(
    installation,  # noqa: F811
):
    with pytest.raises(
            refresh_tool.RefreshError,
            match="actions-control requires actions-status"):
        refresh_tool.load_config(_refresh_config(
            installation, ["actions-control"], "control-only.json"))

    config = refresh_tool.load_config(_refresh_config(
        installation,
        ["actions-status", "actions-control"],
        "actions-control-refresh.json",
    ))
    rendered = refresh_tool._render_gateway_config(
        config,
        refresh_tool._load_public_identity(config),
        refresh_tool.SealMetadata(
            ciphertext_sha256="a" * 64,
            ciphertext_bytes=100,
            key_reference="forkmesh-headless-age:test",
            expected_refs_sha256="b" * 64,
        ),
        installation["archive"] / ("archive-" + "a" * 64 + ".age"),
    )
    expected = installation["gateway_config"].parent / "actions-control"
    assert rendered["actionsControlPath"] == str(expected)
    assert rendered["actionsSummaryPath"] == str(
        expected.with_name("actions-summary.json"))
    assert "actions-control" in rendered["repositories"][0]["operations"]
    assert "actions-control" not in (
        refresh_tool._derived_repository_operations(config))

    # Rendering is pure; the directory appears only when a refresh stages.
    assert not expected.exists()
    refresh_tool._prepare_actions_control_directory(config)
    info = expected.lstat()
    assert stat.S_ISDIR(info.st_mode)
    assert stat.S_IMODE(info.st_mode) == 0o700
    assert info.st_uid == os.geteuid()
    refresh_tool._prepare_actions_control_directory(config)
    expected.chmod(0o750)
    with pytest.raises(refresh_tool.RefreshError, match="Actions control"):
        refresh_tool._prepare_actions_control_directory(config)
    expected.chmod(0o700)

    without = installation["config"]
    assert "actions-control" not in without.operations
    expected.rmdir()
    refresh_tool._prepare_actions_control_directory(without)
    assert not expected.exists()


# --------------------------------------------------------------------------
# Summary version 2.
# --------------------------------------------------------------------------


V1_RESPONSE = {
    "ok": True,
    "node": NODE,
    "updatedAt": NOW - 1000,
    "expiresAt": NOW + 9 * 60 * 1000,
    "runs": [{
        "id": 7,
        "workflow": "Deploy",
        "commit": COMMIT,
        "ref": "refs/heads/main",
        "status": "success",
        "createdAt": NOW - 5000,
        "startedAt": NOW - 4000,
        "finishedAt": NOW - 2000,
        "logTail": "All steps completed.",
    }],
}


def test_v1_summary_response_is_unchanged(tmp_path):
    path = tmp_path / "actions-summary.json"
    write_summary(path, summary_v1())
    result = load_summary(path)
    assert result == V1_RESPONSE
    assert list(result) == list(V1_RESPONSE)
    assert list(result["runs"][0]) == list(V1_RESPONSE["runs"][0])
    assert gateway.json_response(result).body == (
        json.dumps(V1_RESPONSE, sort_keys=True, separators=(",", ":"))
        .encode() + b"\n"
    )
    # Version 1 stays exactly version 1: no v2 key may ride along.
    for key, extra in (
        ("repositories", []),
        ("controls", []),
    ):
        write_summary(path, summary_v1(**{key: extra}))
        with pytest.raises(gateway.GatewayError, match="unavailable"):
            load_summary(path)
    value = summary_v1()
    value["runs"][0]["steps"] = []
    write_summary(path, value)
    with pytest.raises(gateway.GatewayError, match="unavailable"):
        load_summary(path)


def test_v2_summary_is_filtered_to_the_repository_and_stripped(tmp_path):
    path = tmp_path / "actions-summary.json"
    write_summary(path, summary_v2())
    result = load_summary(path)
    assert result == {
        **V1_RESPONSE,
        "schemaVersion": 2,
        "runs": [{
            **V1_RESPONSE["runs"][0],
            "workflowPath": ".forkmesh/deploy.yml",
            "pullNumber": 12,
            "steps": [
                {"name": "Checkout", "status": "success"},
                {"name": "", "status": "skipped"},
            ],
        }],
        "actionsEnabled": True,
        "workflows": [
            workflow_entry(),
            workflow_entry(
                path=".forkmesh/broken.yaml",
                name="",
                events=[],
                manual=False,
                runsOn=[],
                valid=False,
            ),
        ],
        "controls": [
            {
                "requestId": REQUEST_ID,
                "action": "dispatch",
                "status": "applied",
                "reason": "",
                "runId": 7,
                "finishedAt": NOW - 1500,
            },
            {
                "requestId": REQUEST_ID,
                "action": "cancel",
                "status": "rejected",
                "reason": "not_cancellable",
                "runId": 7,
                "finishedAt": NOW - 3000,
            },
        ],
    }
    encoded = json.dumps(result)
    assert "Other owner" not in encoded
    assert "other_control_00001" not in encoded

    other = load_summary(path, owner="other")
    assert [run["id"] for run in other["runs"]] == [8]
    assert other["runs"][0]["workflowPath"] == ""
    assert other["actionsEnabled"] is False
    assert [item["name"] for item in other["workflows"]] == ["Other owner"]
    assert [item["requestId"] for item in other["controls"]] == [
        "other_control_00001"]

    # Case-insensitive repository match, as for runs.
    assert load_summary(path, repository="PROJECT")["actionsEnabled"] is True

    absent = load_summary(path, owner="nobody")
    assert absent["schemaVersion"] == 2
    assert absent["runs"] == []
    assert absent["actionsEnabled"] is False
    assert absent["workflows"] == []
    assert absent["controls"] == []


def test_gateway_status_response_carries_v2_without_caching(control_app):
    app, _handoff, _control, _clock = control_app
    response = app._actions_status(OWNER, REPOSITORY)
    assert response.status == 200
    assert response.headers["Cache-Control"] == "no-store"
    payload = json.loads(response.body)
    assert payload["schemaVersion"] == 2
    assert payload["actionsEnabled"] is True
    assert all(
        "owner" not in item and "repository" not in item
        for item in payload["runs"] + payload["controls"]
    )


def _run(value, **changes):
    value["runs"][0].update(changes)


def _drop_run_key(value, key):
    del value["runs"][0][key]


def _repository(value, **changes):
    value["repositories"][0].update(changes)


def _workflow(value, **changes):
    value["repositories"][0]["workflows"][0].update(changes)


def _control(value, **changes):
    value["controls"][0].update(changes)


def _step(value, step):
    value["runs"][0]["steps"] = [step]


V2_REJECTIONS = {
    "version-bool": lambda v: v.update(schemaVersion=True),
    "version-float": lambda v: v.update(schemaVersion=2.0),
    "version-three": lambda v: v.update(schemaVersion=3),
    "version-string": lambda v: v.update(schemaVersion="2"),
    "missing-repositories": lambda v: v.pop("repositories"),
    "missing-controls": lambda v: v.pop("controls"),
    "unknown-top-key": lambda v: v.update(variables={}),
    "run-missing-steps": lambda v: _drop_run_key(v, "steps"),
    "run-missing-pull": lambda v: _drop_run_key(v, "pullNumber"),
    "run-missing-path": lambda v: _drop_run_key(v, "workflowPath"),
    "run-extra-key": lambda v: _run(v, command="make"),
    # Validated for every run, not only the requested repository's.
    "other-run-invalid": lambda v: v["runs"][1].update(pullNumber=-1),
    "path-outside": lambda v: _run(v, workflowPath="/etc/deploy.yml"),
    "path-nested": lambda v: _run(v, workflowPath=".forkmesh/a/b.yml"),
    "path-null": lambda v: _run(v, workflowPath=None),
    "pull-negative": lambda v: _run(v, pullNumber=-1),
    "pull-bool": lambda v: _run(v, pullNumber=False),
    "pull-string": lambda v: _run(v, pullNumber="12"),
    "pull-float": lambda v: _run(v, pullNumber=12.0),
    "pull-unsafe": lambda v: _run(v, pullNumber=1 << 53),
    "steps-object": lambda v: _run(v, steps={}),
    "steps-too-many": lambda v: _run(
        v, steps=[{"name": "s", "status": "success"}] * 65),
    "step-extra-key": lambda v: _step(
        v, {"name": "s", "status": "success", "run": "make"}),
    "step-missing-status": lambda v: _step(v, {"name": "s"}),
    "step-status": lambda v: _step(v, {"name": "s", "status": "queued"}),
    "step-status-type": lambda v: _step(v, {"name": "s", "status": ["success"]}),
    "step-name-long": lambda v: _step(
        v, {"name": "n" * 121, "status": "success"}),
    "step-name-multiline": lambda v: _step(
        v, {"name": "a\nb", "status": "success"}),
    "step-name-type": lambda v: _step(v, {"name": 1, "status": "success"}),
    "step-not-object": lambda v: _step(v, "Checkout"),
    "repositories-object": lambda v: v.update(repositories={}),
    "repositories-too-many": lambda v: v.update(repositories=[
        {
            "owner": OWNER,
            "repository": f"repo-{index}",
            "actionsEnabled": True,
            "workflows": [],
        }
        for index in range(65)
    ]),
    "repository-duplicate": lambda v: v["repositories"].append(
        dict(v["repositories"][0], repository="PROJECT")),
    "repository-extra-key": lambda v: _repository(v, path="/srv/git"),
    "repository-owner": lambda v: _repository(v, owner="not an owner"),
    "repository-owner-type": lambda v: _repository(v, owner=7),
    "repository-name": lambda v: _repository(v, repository="a/b"),
    "repository-enabled": lambda v: _repository(v, actionsEnabled=1),
    "workflows-object": lambda v: _repository(v, workflows={}),
    "workflows-too-many": lambda v: _repository(v, workflows=[
        workflow_entry(path=f".forkmesh/w{index}.yml")
        for index in range(33)
    ]),
    "workflow-duplicate-path": lambda v: _repository(
        v, workflows=[workflow_entry(), workflow_entry()]),
    "workflow-extra-key": lambda v: _workflow(v, content="on: push"),
    "workflow-path-empty": lambda v: _workflow(v, path=""),
    "workflow-path": lambda v: _workflow(v, path=".github/deploy.yml"),
    "workflow-name-long": lambda v: _workflow(v, name="n" * 161),
    "workflow-name-multiline": lambda v: _workflow(v, name="a\rb"),
    "workflow-name-type": lambda v: _workflow(v, name=None),
    "workflow-events-object": lambda v: _workflow(v, events="push"),
    "workflow-events-too-many": lambda v: _workflow(
        v, events=[f"event{index}" for index in range(9)]),
    "workflow-event-long": lambda v: _workflow(v, events=["e" * 41]),
    "workflow-event-empty": lambda v: _workflow(v, events=[""]),
    "workflow-event-type": lambda v: _workflow(v, events=[1]),
    "workflow-runs-on-too-many": lambda v: _workflow(
        v, runsOn=[f"label{index}" for index in range(9)]),
    "workflow-runs-on-long": lambda v: _workflow(v, runsOn=["l" * 65]),
    "workflow-runs-on-nul": lambda v: _workflow(v, runsOn=["a\x00b"]),
    "workflow-runs-on-type": lambda v: _workflow(v, runsOn="linux"),
    "workflow-manual": lambda v: _workflow(v, manual=1),
    "workflow-enabled": lambda v: _workflow(v, enabled="true"),
    "workflow-valid": lambda v: _workflow(v, valid=None),
    "workflow-not-object": lambda v: _repository(v, workflows=["deploy"]),
    "controls-object": lambda v: v.update(controls={}),
    "controls-too-many": lambda v: v.update(
        controls=[control_entry() for _ in range(33)]),
    "control-extra-key": lambda v: _control(v, actor="alice"),
    "control-missing-key": lambda v: v["controls"][0].pop("reason"),
    "control-owner": lambda v: _control(v, owner="Not An Owner"),
    "control-repository": lambda v: _control(v, repository=""),
    "control-request-id": lambda v: _control(v, requestId="short"),
    "control-request-id-type": lambda v: _control(v, requestId=7),
    "control-action": lambda v: _control(v, action="delete"),
    "control-action-type": lambda v: _control(v, action=["dispatch"]),
    "control-status": lambda v: _control(v, status="accepted"),
    "control-applied-reason": lambda v: _control(v, reason="run_not_found"),
    "control-rejected-blank": lambda v: _control(v, status="rejected"),
    "control-rejected-unknown": lambda v: _control(
        v, status="rejected", reason="because"),
    "control-reason-type": lambda v: _control(v, reason=None),
    "control-run-id-negative": lambda v: _control(v, runId=-1),
    "control-run-id-bool": lambda v: _control(v, runId=True),
    "control-run-id-string": lambda v: _control(v, runId="7"),
    "control-run-id-unsafe": lambda v: _control(v, runId=1 << 53),
    "control-finished-negative": lambda v: _control(v, finishedAt=-1),
    "control-finished-future": lambda v: _control(
        v, finishedAt=NOW + 60_001),
    "control-finished-bool": lambda v: _control(v, finishedAt=True),
    "control-finished-float": lambda v: _control(v, finishedAt=float(NOW)),
    "control-not-object": lambda v: v.update(controls=[REQUEST_ID]),
}


@pytest.mark.parametrize("case", sorted(V2_REJECTIONS))
def test_v2_summary_rejects_every_malformed_field(tmp_path, case):
    path = tmp_path / "actions-summary.json"
    value = summary_v2()
    V2_REJECTIONS[case](value)
    write_summary(path, value)
    with pytest.raises(gateway.GatewayError, match="unavailable"):
        load_summary(path)
    # The same document is rejected whichever repository is asked for.
    with pytest.raises(gateway.GatewayError, match="unavailable"):
        load_summary(path, owner="nobody")


def test_v2_summary_accepts_every_documented_bound(tmp_path):
    path = tmp_path / "actions-summary.json"
    value = summary_v2()
    value["runs"][0]["steps"] = [
        {"name": "n" * 120, "status": status}
        for status in sorted(gateway.ACTIONS_STEP_STATUSES)
    ] + [{"name": "s", "status": "success"}] * 58
    value["runs"][0]["pullNumber"] = (1 << 53) - 1
    value["repositories"] = [{
        "owner": OWNER,
        "repository": REPOSITORY,
        "actionsEnabled": False,
        "workflows": [
            workflow_entry(
                path=f".forkmesh/{'w' * 94}{index:02d}.yaml",
                name="n" * 160,
                events=["e" * 40] * 8,
                runsOn=["l" * 64] * 8,
            )
            for index in range(32)
        ],
    }]
    value["controls"] = [
        control_entry(
            requestId=f"web_control_{index:07d}",
            action=action,
            status="rejected",
            reason=reason,
            runId=0,
            finishedAt=0,
        )
        for index, (action, reason) in enumerate(zip(
            itertools.cycle(sorted(gateway.ACTIONS_CONTROL_ACTIONS)),
            sorted(gateway.ACTIONS_CONTROL_REJECTION_REASONS),
        ))
    ]
    assert len(value["controls"]) == 13
    write_summary(path, value)
    result = load_summary(path)
    assert len(result["runs"][0]["steps"]) == 64
    assert len(result["workflows"]) == 32
    assert result["actionsEnabled"] is False
    assert len(result["controls"]) == 13


def test_v2_summary_rejects_duplicate_json_keys(tmp_path):
    path = tmp_path / "actions-summary.json"
    text = json.dumps(summary_v2(), separators=(",", ":"))
    needle = '"actionsEnabled":true'
    assert needle in text
    path.write_text(
        text.replace(needle, needle + ',"actionsEnabled":false', 1),
        encoding="utf-8",
    )
    path.chmod(0o600)
    with pytest.raises(gateway.GatewayError, match="unavailable"):
        load_summary(path)
