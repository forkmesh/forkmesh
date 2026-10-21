from __future__ import annotations

import asyncio
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

from hidden_unicode import POLICY, clean, clean_pairs, clean_query, clean_value, has_hidden


ROOT = Path(__file__).resolve().parents[2]
SUITE = ROOT / "conformance" / "hidden-unicode" / "v1"
VECTORS = json.loads((SUITE / "vectors.json").read_text(encoding="utf-8"))
SAFE_INPUT = ROOT / "www" / "public" / "safe-input.js"
ZWSP = chr(0x200B)
RLO = chr(0x202E)
TAG_A = chr(0xE0041)


def test_vectors_are_current():
    result = subprocess.run(
        [sys.executable, str(SUITE / "generate_vectors.py"), "--check"],
        capture_output=True, text=True)
    assert result.returncode == 0, result.stderr


def test_vectors_name_the_policy():
    assert VECTORS["policy"] == POLICY
    assert len(VECTORS["cases"]) >= 40


@pytest.mark.parametrize("case", VECTORS["cases"], ids=lambda case: case["id"])
def test_reference_reproduces_every_vector(case):
    assert clean(case["input"]) == case["output"]
    assert has_hidden(case["input"]) is case["hidden"]
    assert clean(case["output"]) == case["output"]


def test_lone_surrogates_are_dropped():
    assert clean("a" + chr(0xD800) + "b" + chr(0xDFFF) + "c") == "abc"


def test_non_strings_pass_through():
    assert clean(None) is None
    assert clean_value(7) == 7
    assert clean_value(True) is True


def test_parsed_bodies_are_cleaned_but_secrets_stay_exact():
    body = {
        "title": "Fix" + ZWSP + " bug",
        "labels": ["ok" + RLO, {"name" + ZWSP: "x" + TAG_A}],
        "password": "pass" + ZWSP + "word",
        "new_password": "a" + ZWSP,
        "access_token": "t" + ZWSP,
        "signature": "s" + ZWSP,
        "ciphertext": {"v": "c" + ZWSP},
        "count": 3,
    }
    assert clean_value(body) == {
        "title": "Fix bug",
        "labels": ["ok", {"name": "x"}],
        "password": "pass" + ZWSP + "word",
        "new_password": "a" + ZWSP,
        "access_token": "t" + ZWSP,
        "signature": "s" + ZWSP,
        "ciphertext": {"v": "c" + ZWSP},
        "count": 3,
    }


def test_form_pairs_and_queries_are_cleaned():
    assert clean_pairs([("q", "a" + RLO + "b"), ("csrf_token", "z" + ZWSP)]) == [
        ("q", "ab"), ("csrf_token", "z" + ZWSP)]
    assert clean_query({"q": ["a" + ZWSP + "b"], "password": ["p" + ZWSP]}) == {
        "q": ["ab"], "password": ["p" + ZWSP]}


class _TextRequest:
    url = "https://forkmesh.test/api/x"

    def __init__(self, text):
        self._text = text
        self.headers = {}
        self.body = None

    async def text(self):
        return self._text


def test_bounded_json_request_strips_hidden_text():
    import request_bodies

    request_bodies.RequestBodyTooLarge = type("TooLarge", (BaseException,), {})
    request_bodies.urlparse = __import__("urllib.parse").parse.urlparse
    request_bodies.json = json
    payload = json.dumps({"body": "hi" + TAG_A + TAG_A, "password": "p" + ZWSP})
    data = asyncio.run(request_bodies.bounded_json_request(_TextRequest(payload)))
    assert data == {"body": "hi", "password": "p" + ZWSP}


def test_host_request_json_and_form_are_cleaned():
    sys.path.insert(0, str(ROOT / "app" / "host"))
    try:
        from forkmesh_host.js import Request
    except Exception as error:
        pytest.skip(f"host runtime not importable: {error}")
    body = json.dumps({"name": "a" + ZWSP + "b"}).encode("utf-8")
    parsed = asyncio.run(Request("https://forkmesh.test/", "POST", body=body).json())
    assert parsed["name"] == "ab"
    form = "q=a%E2%80%8Bb&password=p%E2%80%8B".encode("ascii")
    pairs = asyncio.run(Request("https://forkmesh.test/", "POST", body=form).formData())
    assert dict(pairs.to_py() if hasattr(pairs, "to_py") else pairs) == {"q": "ab", "password": "p" + ZWSP}


JSON_BODY_SITES = {
    "app/src/entry.py": 5,
    "app/src/marketplace_runtime.py": 1,
    "app/src/programs_runtime.py": 1,
    "app/src/microjobs_runtime.py": 1,
    "app/src/ssh_gateway_routes.py": 1,
    "app/src/provider_runtimes.py": 2,
    "app/src/workflows_runtime.py": 1,
    "app/src/ledger.py": 1,
    "app/src/org_office_room.py": 1,
    "app/src/org_office_routes.py": 1,
}

FORM_SITES = (
    "app/src/admin_support.py",
    "app/src/instance_directory_admin.py",
    "app/src/not_found_log.py",
    "app/src/marketing_email_runtime.py",
    "app/src/admin_shell.py",
    "app/src/product_analysis.py",
    "app/src/app_runtime.py",
    "app/src/hackathon_runtime.py",
    "app/src/server_update_admin.py",
    "app/src/ecosystem.py",
    "app/src/instance_console.py",
)


@pytest.mark.parametrize("path,count", sorted(JSON_BODY_SITES.items()))
def test_json_body_readers_clean_what_they_parse(path, count):
    text = (ROOT / path).read_text(encoding="utf-8")
    assert text.count("clean_hidden_unicode(json.loads(raw") == count


@pytest.mark.parametrize("path", FORM_SITES)
def test_form_readers_clean_what_they_parse(path):
    text = (ROOT / path).read_text(encoding="utf-8")
    assert "clean_hidden_query(parse_qs(" in text
    assert not re.search(r"(form|fields) = (\{.*)?parse_qs\((str\(raw|await)", text)


def test_browser_script_reproduces_every_vector():
    node = shutil.which("node")
    if not node:
        pytest.skip("node is not installed")
    script = (
        "const api = require(process.argv[1]);"
        "const data = require(process.argv[2]);"
        "const out = data.cases.map(c => [c.id, api.clean(c.input), api.hasHidden(c.input)]);"
        "out.push(['lone', api.clean('a\\ud800b\\udfffc'), true]);"
        "process.stdout.write(JSON.stringify(out));"
    )
    result = subprocess.run(
        [node, "-e", script, str(SAFE_INPUT), str(SUITE / "vectors.json")],
        capture_output=True, text=True, check=True)
    rows = json.loads(result.stdout)
    expected = [[c["id"], c["output"], c["hidden"]] for c in VECTORS["cases"]] + [["lone", "abc", True]]
    assert rows == expected


def test_browser_script_cleans_fields_in_the_capture_phase():
    js = SAFE_INPUT.read_text(encoding="utf-8")
    assert js.isascii()
    assert 'doc.addEventListener("input", onInput, true)' in js
    assert "event.isComposing" in js
    assert "password: 1" in js


def test_new_sources_hold_no_literal_hidden_characters():
    paths = [
        ROOT / "app" / "src" / "hidden_unicode.py",
        SUITE / "generate_vectors.py",
        SAFE_INPUT,
        ROOT / "desktop" / "src" / "HiddenUnicode.cpp",
        ROOT / "desktop" / "src" / "HiddenUnicodeGuard.cpp",
        ROOT / "native" / "ios" / "ForkMeshCore" / "Sources" / "FMBase" / "HiddenUnicode.swift",
        ROOT / "native" / "ios" / "ForkMesh" / "Platform" / "HiddenUnicodeInputGuard.swift",
        ROOT / "native" / "android" / "core" / "src" / "main" / "kotlin" / "com" / "forkmesh" / "nativeapp" / "security" / "HiddenUnicode.kt",
        ANDROID_APP / "com" / "forkmesh" / "nativeapp" / "ui" / "kit" / "FmSafeInput.kt",
        ROOT / "docs" / "security" / "hidden-unicode.md",
    ]
    for path in paths:
        assert path.read_text(encoding="utf-8").isascii(), path


ANDROID_APP = ROOT / "native" / "android" / "app" / "src" / "main" / "kotlin"


def test_android_bundled_vectors_match_the_suite():
    bundled = ROOT / "native" / "android" / "core" / "src" / "test" / "resources" / "hidden-unicode" / "v1" / "vectors.json"
    assert bundled.read_bytes() == (SUITE / "vectors.json").read_bytes()


def test_android_text_fields_go_through_the_guard():
    raw_field = re.compile(r"\b(BasicTextField|OutlinedTextField|TextField|EditText)\(")
    missing = []
    for path in ANDROID_APP.rglob("*.kt"):
        text = path.read_text(encoding="utf-8")
        if raw_field.search(text) and "FmSafeInput" not in text:
            missing.append(str(path.relative_to(ROOT)))
    assert missing == []


def test_every_client_installs_its_guard():
    main_cpp = (ROOT / "desktop" / "src" / "main.cpp").read_text(encoding="utf-8")
    assert "forkmesh::hiddenunicode::installInputGuard(&app);" in main_cpp
    delegate = (ROOT / "native" / "ios" / "ForkMesh" / "App" / "AppDelegate.swift").read_text(encoding="utf-8")
    assert "HiddenUnicodeInputGuard.install()" in delegate
    material = (ANDROID_APP / "com" / "forkmesh" / "nativeapp" / "ui" / "kit" / "FmMaterial.kt").read_text(encoding="utf-8")
    assert "FmSafeInput.typing(onValueChange)" in material


def test_every_web_page_loads_the_browser_guard():
    pages = []
    for base in (ROOT / "app" / "public", ROOT / "www" / "public", ROOT / "world" / "public", ROOT / "office" / "public"):
        for path in base.rglob("*.html"):
            if "partials" in path.parts or path.name == "deck.html":
                continue
            text = path.read_text(encoding="utf-8")
            if "<script" in text and "/safe-input.js" not in text and "/analytics.js" in text:
                pages.append(str(path.relative_to(ROOT)))
    assert pages == []
    assert "/safe-input.js" in (ROOT / "www" / "templates" / "base.html").read_text(encoding="utf-8")
    assert "/safe-input.js" in (ROOT / "office" / "public" / "index.html").read_text(encoding="utf-8")
    assert "/safe-input.js" in (ROOT / "world" / "public" / "world" / "index.html").read_text(encoding="utf-8")
