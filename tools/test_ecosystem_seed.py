#!/usr/bin/env python3
import base64
import http.server
import importlib.util
import json
import threading
from pathlib import Path

import pytest
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import (
    Ed25519PrivateKey,
)


HERE = Path(__file__).resolve().parent
REPOSITORY = HERE.parent


def _load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


seed = _load("ecosystem_seed_under_test", HERE / "ecosystem_seed.py")


def _server_side():
    path = REPOSITORY / "app" / "src" / "ecosystem.py"
    if not path.is_file():
        pytest.skip("the App's ecosystem module is not in this checkout")
    return _load("ecosystem_under_test", path)


def _key_file(tmp_path):
    key = Ed25519PrivateKey.generate()
    path = tmp_path / "ed25519.pem"
    path.write_bytes(key.private_bytes(
        serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
        serialization.NoEncryption()))
    return key, path


def _verifies(key, body):
    signature = body["sig"] + "=" * (-len(body["sig"]) % 4)
    key.public_key().verify(
        base64.urlsafe_b64decode(signature),
        seed.canonical(body["node"], body["ts"], body["requestId"],
                       body["payload"]))
    return True


def test_the_client_signs_the_same_canonical_the_server_checks():
    server = _server_side()
    args = ("root", "1789560000000", "req-0123456789abcdef", '[{"a": 1}]')
    assert seed.canonical(*args) == server.import_canonical(*args)
    key = Ed25519PrivateKey.generate()
    body = seed.signed_body(key, "root", "[]", now_ms=1789560000000)
    assert body["ts"] == "1789560000000"
    assert server.REQUEST_ID_RE.match(body["requestId"])
    assert _verifies(key, body)


class _Relay(http.server.BaseHTTPRequestHandler):
    answer = (200, {"ok": True, "added": 2, "updated": 1, "unchanged": 0})
    seen = []

    def do_POST(self):
        length = int(self.headers.get("content-length") or 0)
        headers = {name.lower(): value
                   for name, value in self.headers.items()}
        type(self).seen.append(
            (self.path, headers, json.loads(self.rfile.read(length))))
        status, payload = type(self).answer
        data = json.dumps(payload).encode()
        self.send_response(status)
        self.send_header("content-type", "application/json")
        self.send_header("content-length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, *args):
        return None


def _run(tmp_path, answer, capsys, text='[{"slug": "git-bug"}]'):
    key, key_path = _key_file(tmp_path)
    projects = tmp_path / "projects.json"
    projects.write_text(text, encoding="utf-8")
    _Relay.answer = answer
    _Relay.seen = []
    server = http.server.HTTPServer(("127.0.0.1", 0), _Relay)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        code = seed.main([str(projects), "--node", "Root", "--key",
                          str(key_path), "--origin",
                          "http://127.0.0.1:%d/" % server.server_port])
    finally:
        server.shutdown()
        server.server_close()
    return key, code, capsys.readouterr()


def test_a_run_posts_the_signed_file_and_reports_the_counts(tmp_path, capsys):
    key, code, output = _run(
        tmp_path, (200, {"ok": True, "added": 2, "updated": 1,
                         "unchanged": 0}), capsys)
    assert code == 0
    assert output.out == "Imported 3 projects: 2 added, 1 updated, 0 unchanged.\n"
    path, headers, body = _Relay.seen[0]
    assert path == "/api/ecosystem/import"
    assert headers["user-agent"] == "forkmesh-ecosystem-seed/1"
    assert body["node"] == "root"
    assert body["payload"] == '[{"slug": "git-bug"}]'
    assert _verifies(key, body)


def test_a_refused_run_prints_every_error_and_fails(tmp_path, capsys):
    _key, code, output = _run(
        tmp_path, (400, {"ok": False, "error": "invalid_projects",
                         "errors": ["Project 1 (git-bug): url is required"]}),
        capsys)
    assert code == 1
    assert "HTTP 400 invalid_projects" in output.err
    assert "  Project 1 (git-bug): url is required" in output.err

    _key, code, output = _run(tmp_path, (401, {"error": "unauthorized"}),
                              capsys)
    assert code == 1
    assert "--node is an admin account" in output.err


def test_malformed_json_never_leaves_the_machine(tmp_path, capsys):
    _key, code, output = _run(tmp_path, (200, {"ok": True}), capsys,
                              text="[{oops")
    assert code == 2
    assert "is not valid JSON" in output.err
    assert _Relay.seen == []


def test_the_third_party_file_imports_cleanly_as_published_listings():
    server = _server_side()
    text = (HERE / "ecosystem_third_parties.json").read_text(encoding="utf-8")
    items, errors = server.parse_payload(text)
    assert errors == []
    changes, errors = server.plan_import(items, [], 1789560000000)
    assert errors == []
    assert len(changes) == len(items)
    for raw in items:
        assert raw["published"] is True, raw["slug"]
        assert "internal" not in raw, raw["slug"]
        assert raw["integration"] in ("uses", "available", "watching")
        assert raw["integrations"], raw["slug"]
    order = [key for key, _label in server.CATEGORIES]
    keys = [(order.index(raw["category"]), raw["name"].lower())
            for raw in items]
    assert keys == sorted(keys)


def test_the_hn_launch_file_imports_cleanly_with_its_sources():
    server = _server_side()
    text = (HERE / "ecosystem_hn_launches.json").read_text(encoding="utf-8")
    items, errors = server.parse_payload(text)
    assert errors == []
    changes, errors = server.plan_import(items, [], 1790810000000)
    assert errors == []
    assert len(changes) == len(items)
    third_parties = {
        raw["slug"] for raw in json.loads(
            (HERE / "ecosystem_third_parties.json").read_text(encoding="utf-8"))
    }
    for raw in items:
        assert raw["slug"] not in third_parties, raw["slug"]
        assert raw["slug"] != "rickub"
        assert raw["published"] is True, raw["slug"]
        assert raw["integration"] == "watching", raw["slug"]
        sources = raw["internal"]["sources"]
        assert sources and all(
            source.startswith("https://news.ycombinator.com/item?id=")
            for source in sources), raw["slug"]


def test_the_requested_file_imports_cleanly_without_overlap():
    server = _server_side()
    text = (HERE / "ecosystem_requested.json").read_text(encoding="utf-8")
    items, errors = server.parse_payload(text)
    assert errors == []
    changes, errors = server.plan_import(items, [], 1790870000000)
    assert errors == []
    assert len(changes) == len(items)
    elsewhere = set()
    for name in ("ecosystem_third_parties.json", "ecosystem_hn_launches.json"):
        elsewhere |= {raw["slug"] for raw in json.loads(
            (HERE / name).read_text(encoding="utf-8"))}
    for raw in items:
        assert raw["slug"] not in elsewhere, raw["slug"]
        assert raw["published"] is True, raw["slug"]
        assert raw["internal"]["sources"], raw["slug"]
