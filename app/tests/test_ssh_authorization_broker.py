#!/usr/bin/env python3
"""Security boundaries for the local SSH authorization broker."""

import importlib.util
import json
import os
from pathlib import Path
import socket
import struct
import sys
from types import SimpleNamespace

import pytest


PROJECT_ROOT = Path(__file__).resolve().parents[2]
PACKAGING = PROJECT_ROOT / "server" / "packaging"


def _module(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


broker = _module(
    PROJECT_ROOT / "tools" / "ssh_authorization_broker.py",
    "forkmesh_ssh_authorization_broker",
)


def _config():
    return broker.BrokerConfig(
        api_origin="https://forkmesh.example",
        token="t" * 48,
        socket_path=Path("/run/forkmesh-ssh-auth/authorize.sock"),
        socket_gid=123,
        lookup_uid=1001,
        git_uid=1002,
    )


def _key_line():
    key_type = b"ssh-ed25519"
    material = b"\x42" * 32
    blob = (
        len(key_type).to_bytes(4, "big")
        + key_type
        + len(material).to_bytes(4, "big")
        + material
    )
    import base64

    return "ssh-ed25519 " + base64.b64encode(blob).decode()


def test_broker_peer_role_allows_only_one_exact_action_shape():
    config = _config()
    lookup = {"action": "lookup", "publicKey": _key_line()}
    authorize = {
        "action": "authorize",
        "keyId": "sk_" + "a" * 24,
        "owner": "mirror2",
        "repository": "forkmesh",
        "operation": "git-receive-pack",
    }
    assert broker._validate_request(lookup, config.lookup_uid, config) == lookup
    assert broker._validate_request(authorize, config.git_uid, config) == (authorize)
    for value, uid in (
        (lookup, config.git_uid),
        (authorize, config.lookup_uid),
        ({**lookup, "url": "https://evil.invalid"}, config.lookup_uid),
        ({**authorize, "header": "x"}, config.git_uid),
    ):
        with pytest.raises(broker.BrokerError):
            broker._validate_request(value, uid, config)


def test_broker_frame_rejects_duplicate_trailing_and_oversize_data():
    def receive(raw):
        reader, writer = socket.socketpair()
        try:
            writer.sendall(raw)
            writer.shutdown(socket.SHUT_WR)
            return broker._receive_frame(reader)
        finally:
            reader.close()
            writer.close()

    body = b'{"action":"lookup","action":"authorize"}'
    with pytest.raises(broker.BrokerError, match="duplicate"):
        receive(struct.pack("!I", len(body)) + body)

    body = b'{"action":"lookup"}'
    with pytest.raises(broker.BrokerError, match="trailing"):
        receive(struct.pack("!I", len(body)) + body + b"x")

    with pytest.raises(broker.BrokerError, match="size"):
        receive(struct.pack("!I", broker.MAX_FRAME_BYTES + 1))


def test_broker_disables_proxy_redirects_and_binds_worker_response(
    monkeypatch,
):
    config = _config()
    captured = {}

    class Response:
        status = 200

        def __enter__(self):
            return self

        def __exit__(self, *_args):
            return False

        def read(self, _maximum):
            payload = json.loads(captured["request"].data)
            return json.dumps(
                {
                    "ok": True,
                    "authorized": True,
                    "keyId": "sk_" + "a" * 24,
                    "requestId": payload["requestId"],
                }
            ).encode()

    class Opener:
        def open(self, request, timeout):
            captured.update(request=request, timeout=timeout)
            return Response()

    def build(*handlers):
        captured["handlers"] = handlers
        return Opener()

    monkeypatch.setattr(broker, "build_opener", build)
    result = broker._authorize_upstream(
        config, {"action": "lookup", "publicKey": _key_line()}
    )
    assert result == {"authorized": True, "keyId": "sk_" + "a" * 24}
    assert captured["request"].full_url == (
        "https://forkmesh.example/api/ssh/authorize"
    )
    assert captured["request"].get_header("Authorization") == ("Bearer " + "t" * 48)
    assert any(
        isinstance(handler, broker._NoRedirect) for handler in captured["handlers"]
    )
    assert any(
        getattr(handler, "proxies", None) == {} for handler in captured["handlers"]
    )
    assert (
        broker._NoRedirect().redirect_request(
            SimpleNamespace(),
            None,
            302,
            "Found",
            {},
            "http://evil.invalid/",
        )
        is None
    )


def test_broker_rejects_replayed_or_swapped_worker_response():
    request = {
        "action": "authorize",
        "keyId": "sk_" + "a" * 24,
        "owner": "forkmesh",
        "repository": "forkmesh",
        "operation": "git-receive-pack",
    }
    response = {
        "ok": True,
        "authorized": True,
        "keyId": request["keyId"],
        "requestId": "r" * 24,
        "operation": request["operation"],
        "owner": "mirror2",
        "repository": "forkmesh",
        "repositoryRelativePath": "mirror2/forkmesh.git",
    }
    assert (
        broker._validated_upstream_response(request, response, "r" * 24)["owner"]
        == "mirror2"
    )
    for changed in (
        {**response, "requestId": "s" * 24},
        {**response, "keyId": "sk_" + "b" * 24},
        {**response, "operation": "git-upload-pack"},
    ):
        with pytest.raises(broker.BrokerError):
            broker._validated_upstream_response(request, changed, "r" * 24)


@pytest.mark.skipif(
    not hasattr(socket, "SO_PEERCRED"),
    reason="SO_PEERCRED is a Linux-only broker boundary",
)
def test_broker_reads_kernel_peer_credentials():
    left, right = socket.socketpair()
    try:
        assert broker._peer_uid(left) == os.geteuid()
    finally:
        left.close()
        right.close()


def test_packaging_separates_token_git_lookup_and_mirror_accounts():
    ssh = (PACKAGING / "ssh" / "99-forkmesh-git.conf").read_text(
        encoding="utf-8"
    )
    broker_unit = (
        PACKAGING
        / "systemd"
        / "forkmesh-ssh-authorization-broker.service"
    ).read_text(encoding="utf-8")
    notifier = (PACKAGING / "ssh" / "ssh-refresh-notify").read_text(
        encoding="utf-8"
    )
    entrypoint = (
        PACKAGING / "ssh" / "ssh-gateway-entrypoint"
    ).read_text(encoding="utf-8")
    assert "Match User git" in ssh
    assert "AuthorizedKeysCommandUser forkmesh-ssh-lookup" in ssh
    assert "TrustedUserCAKeys none" in ssh
    assert "AcceptEnv GIT_PROTOCOL" in ssh
    assert "AcceptEnv *" not in ssh
    assert "DisableForwarding yes" in ssh
    assert "User=forkmesh-ssh-auth" in broker_unit
    assert "Group=forkmesh-ssh-gateway" in broker_unit
    assert "/usr/bin/python3 -I" in broker_unit
    assert "/usr/bin/env -i" in entrypoint
    assert 'SSH_ORIGINAL_COMMAND="$original_command"' in entrypoint
    assert 'GIT_PROTOCOL="$git_protocol"' in entrypoint
    for forbidden in (
        "PYTHONPATH",
        "LD_PRELOAD",
        "SSH_AUTH_SOCK",
        "FORKMESH_SSH_GATEWAY_TOKEN",
    ):
        assert forbidden not in entrypoint
    assert "$@" not in notifier
    assert "SSH_ORIGINAL_COMMAND" not in notifier


def _relay_response(request, **credential):
    password = "fms1.%s.1783000000000.%s" % (request["keyId"], "a" * 64)
    return {
        "ok": True,
        "authorized": True,
        "keyId": request["keyId"],
        "requestId": "r" * 24,
        "operation": request["operation"],
        "owner": "acme",
        "repository": "widget",
        "repositoryRelativePath": "acme/widget.git",
        "httpCredential": {
            "username": "alice",
            "password": password,
            "expiresAt": 1783000000000,
            **credential,
        },
    }


def test_broker_passes_a_relay_credential_only_for_the_asking_key():
    config = _config()
    request = broker._validate_request({
        "action": "authorize",
        "keyId": "sk_" + "a" * 24,
        # A short address: the control plane resolves the owner.
        "owner": "",
        "repository": "widget",
        "operation": "git-receive-pack",
    }, config.git_uid, config)
    assert request["owner"] == ""
    result = broker._validated_upstream_response(
        request, _relay_response(request), "r" * 24)
    assert result["httpCredential"]["username"] == "alice"
    assert result["owner"] == "acme"
    for credential in (
        {"password": "fms1.%s.1783000000000.%s" % ("sk_" + "b" * 24, "a" * 64)},
        {"password": "fmp1.x"},
        {"username": "Alice Smith"},
        {"expiresAt": "soon"},
        {"extra": True},
    ):
        with pytest.raises(broker.BrokerError):
            broker._validated_upstream_response(
                request, _relay_response(request, **credential), "r" * 24)
    with pytest.raises(broker.BrokerError):
        broker._validate_request(
            {**request, "owner": "Bad Owner"}, config.git_uid, config)


def test_broker_connect_address_is_loopback_only(monkeypatch):
    assert broker.loopback_address("127.0.0.1:443") == ("127.0.0.1", 443)
    assert broker.loopback_address("[::1]:8443") == ("::1", 8443)
    for bad in ("10.0.0.1:443", "localhost:443", "127.0.0.1", "127.0.0.1:0"):
        assert broker.loopback_address(bad) is None
    config = broker.BrokerConfig(
        api_origin="https://forkmesh.example", token="t" * 48,
        socket_path=Path("/run/x/authorize.sock"), socket_gid=1,
        lookup_uid=1001, git_uid=1002,
        api_connect_address=("127.0.0.1", 443))
    captured = {}

    def build(*handlers):
        captured["handlers"] = handlers
        raise OSError("no network in tests")

    monkeypatch.setattr(broker, "build_opener", build)
    with pytest.raises((broker.BrokerError, OSError)):
        broker._authorize_upstream(
            config, {"action": "lookup", "publicKey": _key_line()})
    assert any(isinstance(handler, broker.HTTPSHandler)
               for handler in captured["handlers"])
