#!/usr/bin/env python3
"""Fixed-input SSH push-to-mirror publication orchestration."""

import base64
from io import BytesIO
import importlib.util
import json
from pathlib import Path
import shlex
import sys
from types import SimpleNamespace
from urllib.parse import parse_qs, urlsplit

import pytest
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey


PROJECT_ROOT = Path(__file__).resolve().parents[2]
PACKAGING = PROJECT_ROOT / "server" / "packaging"


def _module(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


bridge = _module(
    PROJECT_ROOT / "tools" / "ssh_post_receive_refresh.py",
    "forkmesh_ssh_post_receive_refresh",
)
edge_routing = _module(
    PROJECT_ROOT / "app" / "src" / "edge_routing.py",
    "forkmesh_edge_routing_for_renew_timer",
)


def _config(tmp_path, *, health_timeout_seconds=10):
    return bridge.BridgeConfig(
        trigger_path=tmp_path / "pending",
        notify_user="git",
        mirror_user="forkmesh-mirror",
        python_program=Path("/usr/bin/python3.13"),
        refresh_program=Path("/opt/forkmesh-mirror/headless_mirror_refresh.py"),
        refresh_config_path=Path(
            "/var/lib/forkmesh-mirror/gateway/mirror-refresh.json"
        ),
        gateway_config_path=tmp_path / "mirror-gateway.json",
        mirror_service="forkmesh-mirror.service",
        health_timeout_seconds=health_timeout_seconds,
    )


def _b64url(value):
    return base64.urlsafe_b64encode(value).decode().rstrip("=")


def _operation(command):
    return command[1] if command[0] == "/usr/bin/systemctl" else command[-1]


def test_notify_discards_ref_input_and_creates_only_fixed_marker(tmp_path, monkeypatch):
    config = _config(tmp_path)
    monkeypatch.setattr(
        bridge.pwd,
        "getpwuid",
        lambda _uid: SimpleNamespace(pw_name="git"),
    )
    monkeypatch.setattr(bridge, "_require_trigger_directory", lambda _config: tmp_path)
    bridge.notify(
        config,
        input_stream=BytesIO(
            b"0" * 40 + b" " + b"1" * 40 + b" refs/heads/main;touch-pwned\n"
        ),
    )
    assert config.trigger_path.read_bytes() == b"forkmesh-refresh-v1\n"
    assert sorted(path.name for path in tmp_path.iterdir()) == ["pending"]


def test_run_releases_push_path_before_periodic_renewal(
    tmp_path, monkeypatch
):
    config = _config(tmp_path)
    config.trigger_path.write_text("forkmesh-refresh-v1\n", encoding="ascii")
    monkeypatch.setattr(bridge.os, "geteuid", lambda: 0)
    events = []

    def runner(command, **_kwargs):
        events.append(command)
        return SimpleNamespace(returncode=0)

    result = bridge.run(
        config,
        runner=runner,
        health_waiter=lambda _config: events.append(["signed-health"]),
        refresh_sleeper=lambda seconds: events.append(["sleep", seconds]),
    )
    assert result["ok"] is True
    assert events[0] == [
        "/usr/bin/systemctl",
        "stop",
        bridge.RENEW_TIMER,
        bridge.RENEW_SERVICE,
    ]
    assert events[1][-1] == "refresh"
    assert events[2] == [
        "/usr/bin/systemctl",
        "restart",
        "forkmesh-mirror.service",
    ]
    assert events[3] == ["signed-health"]
    assert events[4] == [
        "/usr/bin/systemctl",
        "start",
        bridge.RENEW_TIMER,
    ]
    assert not config.trigger_path.exists()


def test_run_gives_every_child_only_the_fixed_disk_backed_tmpdir(
    tmp_path, monkeypatch
):
    config = _config(tmp_path)
    config.trigger_path.write_text("forkmesh-refresh-v1\n", encoding="ascii")
    monkeypatch.setattr(bridge.os, "geteuid", lambda: 0)
    monkeypatch.setenv("TMPDIR", "/tmp/attacker-controlled")
    monkeypatch.setenv("PATH", "/tmp/untrusted-bin")
    monkeypatch.setenv("PUSH_SECRET", "must-not-be-forwarded")
    children = []

    def runner(command, **kwargs):
        operation = _operation(command)
        children.append((operation, kwargs["env"]))
        return SimpleNamespace(returncode=0)

    bridge.run(
        config,
        runner=runner,
        health_waiter=lambda _config: None,
        refresh_sleeper=lambda _seconds: pytest.fail(
            "a successful refresh must not sleep"
        ),
    )

    assert [operation for operation, _environment in children] == [
        "stop",
        "refresh",
        "restart",
        "start",
    ]
    expected = {
        "PATH": "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin",
        "LANG": "C",
        "LC_ALL": "C",
        "TMPDIR": "/var/lib/forkmesh-mirror/runtime-tmp",
    }
    assert all(environment == expected for _operation, environment in children)
    assert all(
        environment["TMPDIR"] != "/tmp/attacker-controlled"
        for _operation, environment in children
    )
    assert not config.trigger_path.exists()


def test_run_retries_only_refresh_once_then_publishes_in_exact_order(
    tmp_path, monkeypatch
):
    config = _config(tmp_path)
    config.trigger_path.write_text("forkmesh-refresh-v1\n", encoding="ascii")
    monkeypatch.setattr(bridge.os, "geteuid", lambda: 0)
    now = [100.0]
    events = []
    refresh_timeouts = []
    refresh_attempts = [0]

    def runner(command, **kwargs):
        operation = _operation(command)
        events.append(operation)
        if operation == "refresh":
            refresh_timeouts.append(kwargs["timeout"])
            refresh_attempts[0] += 1
            now[0] += 3.0
            return SimpleNamespace(
                returncode=1 if refresh_attempts[0] == 1 else 0
            )
        return SimpleNamespace(returncode=0)

    def sleep(seconds):
        events.append(("sleep", seconds))
        now[0] += seconds

    result = bridge.run(
        config,
        runner=runner,
        health_waiter=lambda _config: events.append("signed-health"),
        refresh_monotonic=lambda: now[0],
        refresh_sleeper=sleep,
    )

    assert result["ok"] is True
    assert events == [
        "stop",
        "refresh",
        ("sleep", bridge.REFRESH_RETRY_DELAY_SECONDS),
        "refresh",
        "restart",
        "signed-health",
        "start",
    ]
    assert refresh_timeouts == [
        bridge.REFRESH_TIMEOUT_SECONDS,
        bridge.REFRESH_TIMEOUT_SECONDS
        - 3.0
        - bridge.REFRESH_RETRY_DELAY_SECONDS,
    ]
    assert not config.trigger_path.exists()


def test_run_stops_after_two_refresh_failures(tmp_path, monkeypatch):
    config = _config(tmp_path)
    config.trigger_path.write_text("forkmesh-refresh-v1\n", encoding="ascii")
    monkeypatch.setattr(bridge.os, "geteuid", lambda: 0)
    now = [20.0]
    events = []

    def runner(command, **kwargs):
        operation = _operation(command)
        if operation != "refresh":
            events.append(operation)
            return SimpleNamespace(returncode=0)
        events.append(("refresh", kwargs["timeout"]))
        return SimpleNamespace(returncode=1)

    def sleep(seconds):
        events.append(("sleep", seconds))
        now[0] += seconds

    with pytest.raises(
        bridge.RefreshBridgeError,
        match="fixed refresh command failed",
    ):
        bridge.run(
            config,
            runner=runner,
            health_waiter=lambda _config: pytest.fail(
                "health must not run after refresh failure"
            ),
            refresh_monotonic=lambda: now[0],
            refresh_sleeper=sleep,
        )

    assert events == [
        "stop",
        ("refresh", bridge.REFRESH_TIMEOUT_SECONDS),
        ("sleep", bridge.REFRESH_RETRY_DELAY_SECONDS),
        (
            "refresh",
            bridge.REFRESH_TIMEOUT_SECONDS
            - bridge.REFRESH_RETRY_DELAY_SECONDS,
        ),
        "start",
    ]
    assert not config.trigger_path.exists()
    status = bridge.refresh_status(config)
    assert status["ok"] is False
    assert status["status"] == "retry-pending"
    assert status["retryPending"] is True
    assert status["attempts"] == 1
    assert status["phase"] == "refresh"
    assert status["lastError"] == "publication_phase_failed"


def test_run_does_not_retry_refresh_without_remaining_deadline(
    tmp_path, monkeypatch
):
    config = _config(tmp_path)
    config.trigger_path.write_text("forkmesh-refresh-v1\n", encoding="ascii")
    monkeypatch.setattr(bridge.os, "geteuid", lambda: 0)
    now = [5.0]
    calls = []
    sleeps = []

    def runner(command, **kwargs):
        if _operation(command) != "refresh":
            return SimpleNamespace(returncode=0)
        calls.append((command[-1], kwargs["timeout"]))
        now[0] += bridge.REFRESH_TIMEOUT_SECONDS
        return SimpleNamespace(returncode=1)

    with pytest.raises(bridge.RefreshBridgeError):
        bridge.run(
            config,
            runner=runner,
            health_waiter=lambda _config: pytest.fail(
                "health must not run after refresh failure"
            ),
            refresh_monotonic=lambda: now[0],
            refresh_sleeper=sleeps.append,
        )

    assert calls == [("refresh", bridge.REFRESH_TIMEOUT_SECONDS)]
    assert sleeps == []
    assert not config.trigger_path.exists()


@pytest.mark.parametrize("failure_phase", ["restart", "health"])
def test_run_never_retries_later_publication_phases(
    tmp_path, monkeypatch, failure_phase
):
    config = _config(tmp_path)
    config.trigger_path.write_text("forkmesh-refresh-v1\n", encoding="ascii")
    monkeypatch.setattr(bridge.os, "geteuid", lambda: 0)
    events = []
    sleeps = []

    def runner(command, **_kwargs):
        operation = _operation(command)
        events.append(operation)
        return SimpleNamespace(
            returncode=1 if operation == failure_phase else 0
        )

    def health(_config):
        events.append("health")
        if failure_phase == "health":
            raise bridge.RefreshBridgeError("signed health failed")

    with pytest.raises(bridge.RefreshBridgeError):
        bridge.run(
            config,
            runner=runner,
            health_waiter=health,
            refresh_sleeper=sleeps.append,
        )

    expected = {
        "restart": ["stop", "refresh", "restart", "start"],
        "health": ["stop", "refresh", "restart", "health", "start"],
    }
    assert events == expected[failure_phase]
    assert events.count("refresh") == 1
    assert events.count("restart") == 1
    assert events.count("health") <= 1
    assert "renew" not in events
    assert sleeps == []
    assert not config.trigger_path.exists()
    status = bridge.refresh_status(config)
    assert status["status"] == "retry-pending"
    assert status["phase"] == failure_phase


def test_failed_publication_is_durable_deferred_then_retried(
    tmp_path, monkeypatch
):
    config = _config(tmp_path)
    config.trigger_path.write_text("forkmesh-refresh-v1\n", encoding="ascii")
    monkeypatch.setattr(bridge.os, "geteuid", lambda: 0)
    now_ms = [1_000_000]

    def failing_runner(command, **_kwargs):
        return SimpleNamespace(
            returncode=1 if _operation(command) == "refresh" else 0
        )

    with pytest.raises(bridge.RefreshBridgeError):
        bridge.run(
            config,
            runner=failing_runner,
            health_waiter=lambda _config: None,
            refresh_sleeper=lambda _seconds: None,
            clock_ms=lambda: now_ms[0],
        )

    retry_path = config.trigger_path.with_name(".retry")
    state_path = config.trigger_path.with_name("refresh-state.json")
    assert retry_path.is_file()
    state = json.loads(state_path.read_text(encoding="utf-8"))
    assert state == {
        "attempts": 1,
        "lastError": "publication_phase_failed",
        "nextRetryAt": 1_060_000,
        "phase": "refresh",
        "schemaVersion": 1,
        "status": "retry-pending",
        "updatedAt": 1_000_000,
    }
    # The periodic service sees the retained work but does not spin before the
    # capped backoff expires.
    calls = []
    deferred = bridge.run(
        config,
        runner=lambda command, **_kwargs: calls.append(command),
        health_waiter=lambda _config: calls.append(["health"]),
        clock_ms=lambda: now_ms[0] + 59_999,
        reconcile=True,
    )
    assert deferred == {
        "ok": False,
        "event": "ssh_push_refresh_retry_deferred",
    }
    assert calls == []
    assert retry_path.is_file()

    events = []

    def successful_runner(command, **_kwargs):
        events.append(_operation(command))
        return SimpleNamespace(returncode=0)

    published = bridge.run(
        config,
        runner=successful_runner,
        health_waiter=lambda _config: events.append("health"),
        refresh_sleeper=lambda _seconds: pytest.fail(
            "successful retry must not sleep"
        ),
        clock_ms=lambda: now_ms[0] + 60_000,
        reconcile=True,
    )
    assert published["event"] == "ssh_push_refresh_published"
    assert events == [
        "stop", "refresh", "restart", "health", "start",
    ]
    assert not retry_path.exists()
    status = bridge.refresh_status(
        config, clock_ms=lambda: now_ms[0] + 60_000)
    assert status["ok"] is True
    assert status["status"] == "published"
    assert status["attempts"] == 0
    assert status["lastError"] == ""


def test_reconcile_recovers_processing_marker_left_by_killed_service(
    tmp_path, monkeypatch
):
    config = _config(tmp_path)
    processing = config.trigger_path.with_name(".processing")
    processing.write_text("forkmesh-refresh-v1\n", encoding="ascii")
    monkeypatch.setattr(bridge.os, "geteuid", lambda: 0)
    events = []

    def runner(command, **_kwargs):
        events.append(_operation(command))
        return SimpleNamespace(returncode=0)

    result = bridge.run(
        config,
        runner=runner,
        health_waiter=lambda _config: events.append("health"),
        reconcile=True,
    )
    assert result["event"] == "ssh_push_refresh_published"
    assert events == [
        "stop", "refresh", "restart", "health", "start",
    ]
    assert not processing.exists()


def test_periodic_reconcile_runs_without_a_push_marker(tmp_path, monkeypatch):
    config = _config(tmp_path)
    monkeypatch.setattr(bridge.os, "geteuid", lambda: 0)
    events = []

    def runner(command, **_kwargs):
        events.append(_operation(command))
        return SimpleNamespace(returncode=0)

    result = bridge.run(
        config,
        runner=runner,
        health_waiter=lambda _config: events.append("health"),
        reconcile=True,
    )
    assert result["event"] == "ssh_push_refresh_published"
    assert events == [
        "stop", "refresh", "restart", "health", "start",
    ]


def test_signed_health_binds_both_origins_to_nonce_node_key_and_signature(tmp_path):
    private = Ed25519PrivateKey.generate()
    public = private.public_key().public_bytes_raw()
    config = _config(tmp_path)
    config.gateway_config_path.write_text(
        json.dumps(
            {
                "node": {"name": "mirror2", "publicKey": _b64url(public)},
                "publicOrigin": "https://mirror2.forkmesh.com",
                "listen": {"host": "127.0.0.1", "port": 8790},
            }
        ),
        encoding="utf-8",
    )
    config.gateway_config_path.chmod(0o600)

    class Response:
        status = 200

        def __init__(self, request):
            query = parse_qs(urlsplit(request.full_url).query)
            self.nonce = query["nonce"][0]
            self.issued = int(query["issuedAt"][0])

        def __enter__(self):
            return self

        def __exit__(self, *_args):
            return False

        def read(self, _maximum):
            message = (
                f"forkmesh-https-health-v1\nmirror2\n{self.nonce}\n{self.issued}"
            ).encode()
            import hashlib

            return json.dumps(
                {
                    "ok": True,
                    "integrity": "ok",
                    "node": "mirror2",
                    "publicKey": _b64url(public),
                    "publicRepositoryCount": 2,
                    "challenge": {
                        "messageType": "forkmesh-https-health-v1",
                        "nonce": self.nonce,
                        "issuedAt": self.issued,
                        "algorithm": "Ed25519",
                        "encoding": "base64url-no-padding",
                        "messageSha256": hashlib.sha256(message).hexdigest(),
                        "signature": _b64url(private.sign(message)),
                    },
                }
            ).encode()

    class Opener:
        origins = []

        def open(self, request, timeout):
            assert timeout == 3
            assert request.get_header("User-agent") == bridge.HEALTH_USER_AGENT
            self.origins.append(
                f"{urlsplit(request.full_url).scheme}://"
                f"{urlsplit(request.full_url).netloc}"
            )
            return Response(request)

    opener = Opener()
    bridge.wait_for_signed_health(
        config,
        clock_ms=lambda: 1234567890,
        sleeper=lambda _seconds: None,
        opener=opener,
    )
    assert opener.origins == [
        "http://127.0.0.1:8790",
        "https://mirror2.forkmesh.com",
    ]


def test_signed_health_retries_loopback_then_public_until_tunnel_ready(
    tmp_path, monkeypatch
):
    private = Ed25519PrivateKey.generate()
    public = _b64url(private.public_key().public_bytes_raw())
    config = _config(tmp_path)
    config.gateway_config_path.write_text(
        json.dumps(
            {
                "node": {"name": "mirror2", "publicKey": public},
                "publicOrigin": "https://mirror2.forkmesh.com",
                "listen": {"host": "127.0.0.1", "port": 8790},
            }
        ),
        encoding="utf-8",
    )
    config.gateway_config_path.chmod(0o600)
    now = [0.0]
    calls = []
    public_attempts = [0]

    def signed_once(
        node,
        public_key,
        origin,
        *,
        opener,
        clock_ms,
        timeout_seconds,
    ):
        assert node == "mirror2"
        assert public_key == public
        assert opener is marker_opener
        assert clock_ms() == 1234
        assert 0 < timeout_seconds <= 3
        calls.append(origin)
        if origin.startswith("http://"):
            return True
        public_attempts[0] += 1
        return public_attempts[0] == 3

    def sleep(seconds):
        now[0] += seconds

    marker_opener = object()
    monkeypatch.setattr(bridge, "_signed_health_once", signed_once)
    bridge.wait_for_signed_health(
        config,
        clock_ms=lambda: 1234,
        monotonic=lambda: now[0],
        sleeper=sleep,
        opener=marker_opener,
    )
    assert calls == [
        "http://127.0.0.1:8790",
        "https://mirror2.forkmesh.com",
        "http://127.0.0.1:8790",
        "https://mirror2.forkmesh.com",
        "http://127.0.0.1:8790",
        "https://mirror2.forkmesh.com",
    ]
    assert now[0] == 1.0


def test_signed_health_uses_one_bounded_deadline_and_fails_closed(
    tmp_path, monkeypatch
):
    private = Ed25519PrivateKey.generate()
    public = _b64url(private.public_key().public_bytes_raw())
    config = _config(tmp_path, health_timeout_seconds=5)
    config.gateway_config_path.write_text(
        json.dumps(
            {
                "node": {"name": "mirror2", "publicKey": public},
                "publicOrigin": "https://mirror2.forkmesh.com",
                "listen": {"host": "127.0.0.1", "port": 8790},
            }
        ),
        encoding="utf-8",
    )
    config.gateway_config_path.chmod(0o600)
    now = [10.0]
    calls = []

    def signed_once(
        _node,
        _public_key,
        origin,
        *,
        opener,
        clock_ms,
        timeout_seconds,
    ):
        del opener, clock_ms
        calls.append((origin, timeout_seconds))
        now[0] += timeout_seconds
        return origin.startswith("http://")

    monkeypatch.setattr(bridge, "_signed_health_once", signed_once)
    with pytest.raises(
        bridge.RefreshBridgeError,
        match="signed gateway health did not become ready",
    ):
        bridge.wait_for_signed_health(
            config,
            monotonic=lambda: now[0],
            sleeper=lambda seconds: now.__setitem__(0, now[0] + seconds),
            opener=object(),
        )
    assert now[0] == 15.0
    assert calls == [
        ("http://127.0.0.1:8790", 3.0),
        ("https://mirror2.forkmesh.com", 2.0),
    ]


@pytest.mark.parametrize(
    "public_origin",
    [
        "http://mirror2.forkmesh.com",
        "https://user@mirror2.forkmesh.com",
        "https://mirror2.forkmesh.com/health",
        "https://mirror2.forkmesh.com?redirect=1",
        "https://localhost",
        "https://127.0.0.1",
    ],
)
def test_gateway_public_origin_is_strict_and_fail_closed(
    tmp_path, public_origin
):
    public = _b64url(Ed25519PrivateKey.generate().public_key().public_bytes_raw())
    config = _config(tmp_path)
    config.gateway_config_path.write_text(
        json.dumps(
            {
                "node": {"name": "mirror2", "publicKey": public},
                "publicOrigin": public_origin,
                "listen": {"host": "127.0.0.1", "port": 8790},
            }
        ),
        encoding="utf-8",
    )
    config.gateway_config_path.chmod(0o600)
    with pytest.raises(
        bridge.RefreshBridgeError,
        match="gateway public origin is invalid",
    ):
        bridge._parse_gateway_identity(config.gateway_config_path)


def test_direct_health_transport_disables_proxies_and_redirects(monkeypatch):
    handlers = []
    marker_opener = object()

    def build_opener(*values):
        handlers.extend(values)
        return marker_opener

    monkeypatch.setattr(bridge, "build_opener", build_opener)
    assert bridge._direct_opener() is marker_opener
    assert len(handlers) == 2
    assert isinstance(handlers[0], bridge.ProxyHandler)
    assert handlers[0].proxies == {}
    assert isinstance(handlers[1], bridge._NoRedirectHandler)
    assert (
        handlers[1].redirect_request(
            object(), None, 302, "found", {}, "https://other.invalid/health"
        )
        is None
    )


def test_invalid_public_health_signature_never_becomes_ready(tmp_path):
    private = Ed25519PrivateKey.generate()
    wrong_private = Ed25519PrivateKey.generate()
    public = private.public_key().public_bytes_raw()
    config = _config(tmp_path, health_timeout_seconds=5)
    config.gateway_config_path.write_text(
        json.dumps(
            {
                "node": {"name": "mirror2", "publicKey": _b64url(public)},
                "publicOrigin": "https://mirror2.forkmesh.com",
                "listen": {"host": "127.0.0.1", "port": 8790},
            }
        ),
        encoding="utf-8",
    )
    config.gateway_config_path.chmod(0o600)

    class Response:
        status = 200

        def __init__(self, request, *, signer):
            query = parse_qs(urlsplit(request.full_url).query)
            self.nonce = query["nonce"][0]
            self.issued = int(query["issuedAt"][0])
            self.signer = signer

        def __enter__(self):
            return self

        def __exit__(self, *_args):
            return False

        def read(self, _maximum):
            message = (
                f"forkmesh-https-health-v1\nmirror2\n{self.nonce}\n{self.issued}"
            ).encode()
            return json.dumps(
                {
                    "ok": True,
                    "integrity": "ok",
                    "node": "mirror2",
                    "publicKey": _b64url(public),
                    "publicRepositoryCount": 2,
                    "challenge": {
                        "messageType": "forkmesh-https-health-v1",
                        "nonce": self.nonce,
                        "issuedAt": self.issued,
                        "algorithm": "Ed25519",
                        "encoding": "base64url-no-padding",
                        "messageSha256": bridge.hashlib.sha256(message).hexdigest(),
                        "signature": _b64url(self.signer.sign(message)),
                    },
                }
            ).encode()

    class Opener:
        def open(self, request, timeout):
            del timeout
            signer = (
                private
                if urlsplit(request.full_url).scheme == "http"
                else wrong_private
            )
            return Response(request, signer=signer)

    now = [0.0]
    with pytest.raises(
        bridge.RefreshBridgeError,
        match="signed gateway health did not become ready",
    ):
        bridge.wait_for_signed_health(
            config,
            monotonic=lambda: now[0],
            sleeper=lambda seconds: now.__setitem__(0, now[0] + seconds),
            opener=Opener(),
        )
    assert now[0] == 5.0


def test_refresh_packaging_has_no_user_derived_commands():
    service = (
        PACKAGING / "systemd" / "forkmesh-mirror-refresh.service"
    ).read_text(encoding="utf-8")
    gateway_service = (
        PACKAGING / "systemd" / "forkmesh-mirror.service"
    ).read_text(encoding="utf-8")
    path_unit = (
        PACKAGING / "systemd" / "forkmesh-mirror-refresh.path"
    ).read_text(encoding="utf-8")
    reconcile_service = (
        PACKAGING
        / "systemd"
        / "forkmesh-mirror-reconcile.service"
    ).read_text(encoding="utf-8")
    reconcile_timer = (
        PACKAGING
        / "systemd"
        / "forkmesh-mirror-reconcile.timer"
    ).read_text(encoding="utf-8")
    notifier = (PACKAGING / "ssh" / "ssh-refresh-notify").read_text(
        encoding="utf-8"
    )
    assert "PathExists=/run/forkmesh-mirror-refresh/pending" in path_unit
    assert "forkmesh-mirror.service" not in notifier
    assert "$@" not in notifier
    assert "SSH_ORIGINAL_COMMAND" not in notifier
    assert "/run/forkmesh-mirror-refresh/pending" in notifier
    assert "mktemp" in notifier
    assert "forkmesh-refresh-v1" in notifier
    assert "ExecStart=/usr/bin/python3 -I " in service
    assert " reconcile" in reconcile_service
    assert "OnUnitActiveSec=30min" in reconcile_timer
    assert "Persistent=true" in reconcile_timer
    assert "RandomizedDelaySec=2min" in reconcile_timer
    assert (
        "CapabilityBoundingSet=CAP_DAC_READ_SEARCH CAP_SETGID CAP_SETUID"
        in service
    )
    for unit in (service, reconcile_service, gateway_service):
        assert (
            "Environment=TMPDIR=/var/lib/forkmesh-mirror/runtime-tmp"
            in unit
        )
    assert (
        "ReadWritePaths=/var/lib/forkmesh-mirror/identity "
        "/var/lib/forkmesh-mirror/encrypted "
        "/var/lib/forkmesh-mirror/gateway "
        "/var/lib/forkmesh-mirror/runtime-tmp "
        "-/var/lib/forkmesh-mirror/source -/srv/forkmesh-git"
        in gateway_service
    )


def test_gateway_merge_executor_is_writable_only_inside_service_data_roots():
    service = (
        PACKAGING / "systemd" / "forkmesh-mirror.service"
    ).read_text(encoding="utf-8")
    directives = [
        shlex.split(line.split("=", 1)[1])
        for line in service.splitlines()
        if line.startswith("ReadWritePaths=")
    ]

    # A push-hosting node additionally creates <owner>/<repo>.git under its
    # writable root inside /srv/forkmesh-git and drops the coalescing marker
    # that wakes forkmesh-mirror-refresh.path. The trigger directory is the
    # only addition, and it is optional so a read-only node still starts.
    assert directives == [[
        "/var/lib/forkmesh-mirror/identity",
        "/var/lib/forkmesh-mirror/encrypted",
        "/var/lib/forkmesh-mirror/gateway",
        "/var/lib/forkmesh-mirror/runtime-tmp",
        "-/var/lib/forkmesh-mirror/source",
        "-/srv/forkmesh-git",
        "-/run/forkmesh-mirror-refresh",
    ]]
    assert "ProtectSystem=strict" in service
    assert "/var/lib/forkmesh-mirror/releases" not in directives[0]
    assert "/var/lib/forkmesh-mirror" not in directives[0]
    assert "/run" not in directives[0]
    assert "/" not in directives[0]


def test_gateway_startup_uses_fast_integrity_precheck():
    service = (
        PACKAGING / "systemd" / "forkmesh-mirror.service"
    ).read_text(encoding="utf-8")
    directives = {}
    for line in service.splitlines():
        if line.startswith(("ExecStartPre=", "ExecStart=")):
            key, value = line.split("=", 1)
            directives[key] = shlex.split(value)

    precheck = directives["ExecStartPre"]
    start = directives["ExecStart"]
    assert precheck == [
        "/usr/bin/python3",
        "-I",
        "/opt/forkmesh-mirror/mirror_gateway.py",
        "--config",
        "/var/lib/forkmesh-mirror/gateway/mirror-gateway.json",
        "--check",
    ]
    assert start == [
        "/usr/bin/python3",
        "/opt/forkmesh-mirror/mirror_gateway.py",
        "--config",
        "/var/lib/forkmesh-mirror/gateway/mirror-gateway.json",
    ]
    assert "headless_mirror_refresh.py" not in service
    assert precheck[2:5] == start[1:4]

    # This is the gateway's bounded configuration/manifest/repository/pin
    # validator, not the refresh command whose active check performs full fsck.
    parser_source = (
        PROJECT_ROOT / "tools" / "mirror_gateway.py"
    ).read_text(encoding="utf-8")
    assert (
        "validate configuration, manifest, repositories, and integrity pins"
        in parser_source
    )


def test_refresh_health_timeout_default_and_example_allow_tunnel_startup():
    example = json.loads(
        (
            PACKAGING / "ssh" / "ssh-refresh.json.example"
        ).read_text(encoding="utf-8")
    )
    schema = json.loads(
        (
            PROJECT_ROOT / "www" / "docs" / "ssh-post-receive-refresh.schema.json"
        ).read_text(encoding="utf-8")
    )
    assert bridge.DEFAULT_HEALTH_TIMEOUT_SECONDS == 180
    assert example["healthTimeoutSeconds"] == 180
    assert schema["properties"]["healthTimeoutSeconds"]["default"] == 180


def test_tunnel_restarts_with_the_mirror_gateway():
    tunnel_service = (
        PACKAGING
        / "systemd"
        / "cloudflared-forkmesh.service"
    ).read_text(encoding="utf-8")
    assert "Requires=forkmesh-mirror.service" in tunnel_service
    assert "PartOf=forkmesh-mirror.service" in tunnel_service


def test_signed_health_renewal_timer_is_bounded_and_gateway_coupled():
    service = (
        PACKAGING
        / "systemd"
        / "forkmesh-mirror-renew.service"
    ).read_text(encoding="utf-8")
    timer = (
        PACKAGING
        / "systemd"
        / "forkmesh-mirror-renew.timer"
    ).read_text(encoding="utf-8")

    assert "User=forkmesh-mirror" in service
    assert "Requires=forkmesh-mirror.service" in service
    assert "PartOf=forkmesh-mirror.service" in service
    assert "headless_mirror_refresh.py" in service
    assert " renew" in service
    assert "ProtectSystem=strict" in service
    assert (
        "ReadWritePaths=/var/lib/forkmesh-mirror/gateway "
        "/var/lib/forkmesh-mirror/identity"
    ) in service
    def directives(text):
        parsed = {}
        for raw in text.splitlines():
            line = raw.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            key, value = line.split("=", 1)
            assert key not in parsed
            parsed[key] = value
        return parsed

    timer_directives = directives(timer)
    service_directives = directives(service)
    assert timer_directives["OnBootSec"] == "2min"
    assert timer_directives["OnUnitActiveSec"] == "4min"
    assert timer_directives["AccuracySec"] == "15s"
    assert timer_directives["RandomizedDelaySec"] == "30s"
    assert service_directives["TimeoutStartSec"] == "5min"
    assert service_directives["Restart"] == "on-failure"
    assert service_directives["RestartSec"] == "20s"
    # Conservatively include a complete service timeout after the maximum
    # scheduled interval. Repository requests also require a separate signed
    # proof whose production cache lifetime is only 60 seconds.
    worst_case_renewal_ms = (4 * 60 + 15 + 30 + 5 * 60) * 1000
    assert worst_case_renewal_ms < edge_routing.ENDPOINT_STALE_MS
