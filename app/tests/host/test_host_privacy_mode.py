from __future__ import annotations

import ipaddress
import logging
import sys
from pathlib import Path
from types import SimpleNamespace

import pytest

SRC = Path(__file__).resolve().parents[2] / "src"
if str(SRC) not in sys.path:
    sys.path.append(str(SRC))

from forkmesh_host import d1_caller  # noqa: E402
from forkmesh_host import js as _js  # noqa: E402
from forkmesh_host import privacy  # noqa: E402

STRICT = SimpleNamespace(INSTANCE_PRIVACY_MODE="1")
OFF = SimpleNamespace()


@pytest.fixture(autouse=True)
def fresh_cache():
    privacy.module().forget()
    yield
    privacy.module().forget()


def _headers():
    return _js.Headers({
        "cf-connecting-ip": "203.0.113.9",
        "cf-ipcountry": "NL",
        "x-forwarded-for": "203.0.113.9, 10.0.0.1",
        "x-real-ip": "203.0.113.9",
        "user-agent": "Mozilla/5.0",
    })


def test_the_app_never_sees_a_visitor_address_or_country():
    headers = _headers()
    assert privacy.scrub_headers(headers, STRICT) is True
    address = ipaddress.ip_address(headers.get("cf-connecting-ip"))
    assert address in ipaddress.ip_network("fd00::/8")
    for name in ("cf-ipcountry", "x-forwarded-for", "x-real-ip"):
        assert not headers.has(name)
    assert headers.get("user-agent") == "Mozilla/5.0"


def test_nothing_changes_when_privacy_mode_is_off():
    headers = _headers()
    assert privacy.scrub_headers(headers, OFF) is False
    assert headers.get("cf-connecting-ip") == "203.0.113.9"
    assert headers.get("cf-ipcountry") == "NL"


def test_log_lines_carry_route_groups_not_paths():
    assert privacy.masked_path("/alice/secret-repo/issues/4") == "/:page"
    assert privacy.masked_path("/") == "/"
    assert privacy.masked_path("/api/accounts/alice").startswith("/api/")
    assert "alice" not in privacy.masked_path("/api/accounts/alice")
    assert privacy.log_path(OFF, "/alice/x") == "/alice/x"
    assert privacy.log_path(STRICT, "/alice/x") == "/:page"


def test_uvicorn_info_and_access_lines_are_dropped():
    env = SimpleNamespace()
    error, access = privacy.install_log_filters(
        env, names=("privacy-test.error", "privacy-test.access"))
    info = logging.LogRecord("privacy-test.error", logging.INFO, "", 0,
                             '%s - "WebSocket %s" [accepted]', ("1.2.3.4",
                                                               "/a/b"), None)
    warning = logging.LogRecord("privacy-test.error", logging.WARNING, "", 0,
                                "stopping", (), None)
    assert error.filter(info) and access.filter(info)
    env.INSTANCE_PRIVACY_MODE = "1"
    assert not error.filter(info)
    assert error.filter(warning)
    assert not access.filter(warning)
    again = privacy.install_log_filters(
        env, names=("privacy-test.error", "privacy-test.access"))
    assert logging.getLogger("privacy-test.error").filters == [again[0]]


def test_database_caller_labels_drop_the_address_and_the_account():
    request = SimpleNamespace(
        method="GET", url="https://code.example.org/alice/repo",
        headers=_headers())
    assert "IP 203.0.113.9" in d1_caller.request_label(request)
    assert d1_caller.request_label(request, True) == "GET /:page"
    with d1_caller.request_scope(request, private=True):
        d1_caller.note_session("alice", "abcdef0123456789")
        assert d1_caller.clause() == "GET /:page"
    with d1_caller.request_scope(request):
        d1_caller.note_session("alice", "abcdef0123456789")
        assert "user alice" in d1_caller.clause()


def _host(env):
    from forkmesh_host import request_guard
    from forkmesh_host.asgi import HostASGI
    host = HostASGI.__new__(HostASGI)
    host.runtime = SimpleNamespace(env=env)
    host.trust_forwarded = True
    host.edge_verifier = request_guard.EdgeClientVerifier()
    return host


def _scope():
    return {"type": "http", "method": "GET", "path": "/alice/repo",
            "raw_path": b"/alice/repo", "query_string": b"",
            "scheme": "http", "http_version": "1.1",
            "client": ("203.0.113.9", 50000), "server": ("127.0.0.1", 8080),
            "headers": [(b"host", b"code.example.org"),
                        (b"cf-ipcountry", b"NL"),
                        (b"user-agent", b"Mozilla/5.0")]}


def test_the_host_hands_the_worker_a_pseudonym():
    request = _host(STRICT)._request(_scope(), None, "GET", None)
    address = request.headers.get("cf-connecting-ip")
    assert address != "203.0.113.9"
    assert ipaddress.ip_address(address) in ipaddress.ip_network("fd00::/8")
    assert not request.headers.has("cf-ipcountry")
    assert getattr(request.cf, "country", None) in (None, "")
    request = _host(OFF)._request(_scope(), None, "GET", None)
    assert request.headers.get("cf-connecting-ip") == "203.0.113.9"
    assert request.headers.get("cf-ipcountry") == "NL"
