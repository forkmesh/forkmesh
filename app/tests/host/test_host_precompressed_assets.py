"""Static files are answered from their brotli or gzip copy when the request
accepts that coding (tools/precompress_assets.py writes the copies)."""
from __future__ import annotations

import asyncio
import gzip
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

from forkmesh_host import js as _js
from forkmesh_host.asgi import HostASGI, WorkerFirstRules
from forkmesh_host.bindings import (StaticAssets, accepted_codings,
                                    precompressed_variant)

APP = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(APP / "tools"))
import precompress_assets  # noqa: E402

SCRIPT = ("const greeting = 'hello';\n" * 400).encode()


class _Env:
    FORKMESH_PLATFORM = "vps"
    APP_ORIGIN = "https://app.example.test"
    WORLD_ORIGIN = "https://world.example.test"


class _Runtime:
    def __init__(self, assets):
        self.env = _Env()
        self.env.ASSETS = assets
        self.sealed = False
        self.durable = None

    async def fetch(self, request):
        return _js.Response(b"{}", 200, "", _js.Headers(
            {"content-type": "application/json"}))


def _get(assets, path, headers=(), method="GET"):
    app = HostASGI(_Runtime(assets), serve_assets=True,
                   worker_first=WorkerFirstRules(["/api/*"]))
    sent = []
    scope = {"type": "http", "method": method, "path": path,
             "raw_path": path.encode(), "query_string": b"",
             "headers": [(b"host", b"app.example.test"), *headers],
             "client": ("127.0.0.1", 50000), "scheme": "http",
             "http_version": "1.1"}

    async def receive():
        return {"type": "http.request", "body": b"", "more_body": False}

    async def send(message):
        sent.append(message)

    asyncio.run(app(scope, receive, send))
    found = {name.decode(): value.decode()
             for name, value in sent[0]["headers"]}
    body = b"".join(m.get("body", b"") for m in sent[1:])
    return sent[0]["status"], found, body


def _tree(tmp_path):
    (tmp_path / "app.js").write_bytes(SCRIPT)
    (tmp_path / "app.js.br").write_bytes(b"BROTLI-COPY")
    (tmp_path / "app.js.gz").write_bytes(gzip.compress(SCRIPT))
    (tmp_path / "plain.js").write_bytes(SCRIPT)
    (tmp_path / "404.html").write_text("missing")
    return StaticAssets(tmp_path, "auto-trailing-slash", "404-page")


def _encoding(headers):
    return headers.get("content-encoding")


def test_accept_encoding_values_are_parsed_with_q():
    assert accepted_codings("gzip, deflate, br, zstd") == {
        "gzip": 1.0, "deflate": 1.0, "br": 1.0, "zstd": 1.0}
    assert accepted_codings("br;q=0, gzip;q=0.5, *;q=0.1") == {
        "br": 0.0, "gzip": 0.5, "*": 0.1}
    assert accepted_codings("") == {}
    assert accepted_codings("gzip;q=x") == {"gzip": 0.0}


def test_brotli_when_accepted(tmp_path):
    status, headers, body = _get(_tree(tmp_path), "/app.js",
                                 [(b"accept-encoding", b"gzip, deflate, br, zstd")])
    assert status == 200
    assert _encoding(headers) == "br"
    assert body == b"BROTLI-COPY"
    assert headers["content-length"] == str(len(b"BROTLI-COPY"))
    assert headers["vary"] == "accept-encoding"
    assert headers["etag"].endswith('-br"')
    assert headers["content-type"].startswith("text/javascript")


def test_gzip_when_brotli_is_refused_or_absent(tmp_path):
    assets = _tree(tmp_path)
    for value in (b"gzip", b"br;q=0, gzip", b"gzip;q=1, br;q=0.5"):
        status, headers, body = _get(assets, "/app.js",
                                     [(b"accept-encoding", value)])
        assert status == 200 and _encoding(headers) == "gzip", value
        assert gzip.decompress(body) == SCRIPT
        assert headers["etag"].endswith('-gzip"')


def test_identity_fallback_still_varies(tmp_path):
    assets = _tree(tmp_path)
    for headers_in in ([], [(b"accept-encoding", b"identity")],
                       [(b"accept-encoding", b"br;q=0, gzip;q=0")],
                       [(b"accept-encoding", b"zstd")]):
        status, headers, body = _get(assets, "/app.js", headers_in)
        assert status == 200 and _encoding(headers) is None
        assert body == SCRIPT
        assert headers["vary"] == "accept-encoding"


def test_wildcard_accepts_a_copy(tmp_path):
    _, headers, _ = _get(_tree(tmp_path), "/app.js",
                         [(b"accept-encoding", b"*")])
    assert _encoding(headers) == "br"


def test_file_without_copies_is_unchanged(tmp_path):
    status, headers, body = _get(_tree(tmp_path), "/plain.js",
                                 [(b"accept-encoding", b"br, gzip")])
    assert status == 200 and body == SCRIPT
    assert _encoding(headers) is None and "vary" not in headers


def test_a_copy_older_than_its_file_is_ignored(tmp_path):
    assets = _tree(tmp_path)
    source = tmp_path / "app.js"
    stamp = source.stat().st_mtime_ns
    os.utime(tmp_path / "app.js.br", ns=(stamp - 10**9, stamp - 10**9))
    status, headers, body = _get(assets, "/app.js",
                                 [(b"accept-encoding", b"br")])
    assert _encoding(headers) is None and body == SCRIPT
    _, headers, body = _get(assets, "/app.js",
                            [(b"accept-encoding", b"br, gzip")])
    assert _encoding(headers) == "gzip"


def test_revalidation_matches_the_encoded_variant(tmp_path):
    assets = _tree(tmp_path)
    _, first, _ = _get(assets, "/app.js", [(b"accept-encoding", b"br")])
    status, headers, body = _get(assets, "/app.js", [
        (b"accept-encoding", b"br"), (b"if-none-match", first["etag"].encode())])
    assert status == 304 and body == b""
    assert _encoding(headers) == "br"
    # The plain variant's tag does not validate the brotli body, nor the
    # reverse.
    status, _, _ = _get(assets, "/app.js", [
        (b"accept-encoding", b"gzip"), (b"if-none-match", first["etag"].encode())])
    assert status == 200


def test_head_and_not_found_and_internal_reads(tmp_path):
    assets = _tree(tmp_path)
    status, headers, body = _get(assets, "/app.js",
                                 [(b"accept-encoding", b"br")], method="HEAD")
    assert status == 200 and body == b"" and _encoding(headers) == "br"
    missing = assets.serve("/missing.js", accept_encoding="br")
    assert int(missing.status) == 404
    assert missing.headers.get("content-encoding") is None
    # env.ASSETS.fetch (the Worker reading a document) always gets the file.
    response = asyncio.run(assets.fetch("https://app.example.test/app.js"))
    assert response.headers.get("content-encoding") is None
    assert response._body == SCRIPT


def test_variant_helper_reports_copies(tmp_path):
    _tree(tmp_path)
    coding, copy, varies = precompressed_variant(tmp_path / "app.js", "gzip")
    assert (coding, copy.name, varies) == ("gzip", "app.js.gz", True)
    assert precompressed_variant(tmp_path / "plain.js", "br") == (None, None, False)


def _node_unbrotli(path):
    script = ("process.stdout.write(require('zlib').brotliDecompressSync("
              "require('fs').readFileSync(process.argv[1])))")
    return subprocess.run(["node", "-e", script, str(path)], check=True,
                          capture_output=True).stdout


def test_precompress_writes_fresh_copies_once(tmp_path):
    if shutil.which("node") is None:
        pytest.skip("node is not installed")
    (tmp_path / "big.js").write_bytes(SCRIPT)
    (tmp_path / "style.css").write_bytes(b"a{color:red}\n" * 200)
    (tmp_path / "data.json").write_text(json.dumps({"k": ["v"] * 500}))
    (tmp_path / "tiny.js").write_text("x()")
    (tmp_path / "page.html").write_bytes(b"<p>hi</p>" * 400)
    fragments = tmp_path / "dashboard" / "js"
    fragments.mkdir(parents=True)
    (fragments / "01-state.js").write_bytes(SCRIPT)

    counts = precompress_assets.precompress(tmp_path)
    assert counts == {"brotli": 3, "gzip": 3}
    for name, data in (("big.js", SCRIPT),
                       ("style.css", (tmp_path / "style.css").read_bytes())):
        assert gzip.decompress((tmp_path / (name + ".gz")).read_bytes()) == data
        assert _node_unbrotli(tmp_path / (name + ".br")) == data
        assert (tmp_path / (name + ".br")).stat().st_size < len(data)
    for skipped in ("tiny.js", "page.html", "dashboard/js/01-state.js"):
        assert not (tmp_path / (skipped + ".br")).exists(), skipped
        assert not (tmp_path / (skipped + ".gz")).exists(), skipped

    # A second run leaves fresh copies alone and redoes a rewritten file.
    assert precompress_assets.precompress(tmp_path) == {"brotli": 0, "gzip": 0}
    later = (tmp_path / "big.js.br").stat().st_mtime_ns + 10**9
    (tmp_path / "big.js").write_bytes(SCRIPT + b"// more\n")
    os.utime(tmp_path / "big.js", ns=(later, later))
    assert precompress_assets.precompress(tmp_path) == {"brotli": 1, "gzip": 1}
    assert _node_unbrotli(tmp_path / "big.js.br") == SCRIPT + b"// more\n"


def test_precompress_without_node_writes_gzip_only(tmp_path, monkeypatch):
    monkeypatch.setattr(precompress_assets, "node_binary", lambda: None)
    (tmp_path / "big.js").write_bytes(SCRIPT)
    assert precompress_assets.precompress(tmp_path) == {"brotli": 0, "gzip": 1}
    assert not (tmp_path / "big.js.br").exists()


def test_the_host_build_and_the_instance_world_stage_precompress():
    from forkmesh_host import serve
    assert serve.BUILD_STEPS[-1] == ("tools/precompress_assets.py", "dist")
    site = (APP / "tools" / "build_site_assets.py").read_text(encoding="utf-8")
    world = site[site.index("def stage_instance_world"):]
    world = world[:world.index("\ndef ")]
    assert "precompress_assets.precompress(destination)" in world
