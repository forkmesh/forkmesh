#!/usr/bin/env python3
"""Anonymous install-diagnostics validation checks (stdlib only).

Loads the pure helpers (_sanitize_diag_field, _install_diag_fields) and the
INSTALL_DIAG_STEPS constant straight out of src/entry.py without importing the
Workers-only JS runtime, the same way test_private_repos.py isolates pure code.
Verifies the unauthenticated /api/install-diag endpoint can't be used to store
unknown steps, identifying junk, or markup that would break the admin HTML.
"""

import ast
from pathlib import Path


ENTRY = Path(__file__).resolve().parents[1] / "src" / "entry.py"

_WANT_FUNCS = ("_sanitize_diag_field", "_install_diag_fields")
_WANT_CONSTS = ("INSTALL_DIAG_STEPS",)


def _load():
    tree = ast.parse(ENTRY.read_text(encoding="utf-8"), filename=str(ENTRY))
    body = []
    for node in tree.body:
        if isinstance(node, ast.FunctionDef) and node.name in _WANT_FUNCS:
            body.append(node)
        elif isinstance(node, ast.Assign) and any(
            isinstance(t, ast.Name) and t.id in _WANT_CONSTS for t in node.targets
        ):
            body.append(node)
    module = ast.fix_missing_locations(ast.Module(body=body, type_ignores=[]))
    namespace = {}
    exec(compile(module, str(ENTRY), "exec"), namespace)
    return namespace


_NS = _load()
_fields = _NS["_install_diag_fields"]
_sanitize = _NS["_sanitize_diag_field"]
STEPS = _NS["INSTALL_DIAG_STEPS"]


def _payload(**overrides):
    data = {"run": "abc123", "step": "build", "ok": 1, "os": "Linux",
            "arch": "x86_64", "pm": "apt", "distro": "debian",
            "version": "0.7.0", "detail": "none"}
    data.update(overrides)
    return data


def test_valid_event_is_normalized():
    out = _fields(_payload())
    assert out == ("abc123", "build", 1, "Linux", "x86_64", "apt", "debian",
                   "0.7.0", "none")


def test_every_known_step_is_accepted():
    for step in STEPS:
        out = _fields(_payload(step=step))
        assert out is not None and out[1] == step


def test_unknown_step_is_rejected():
    assert _fields(_payload(step="rm-rf")) is None
    assert _fields(_payload(step="")) is None


def test_non_dict_and_missing_run_are_rejected():
    assert _fields(None) is None
    assert _fields("nope") is None
    assert _fields(_payload(run="")) is None
    assert _fields(_payload(run="!!!@@@")) is None  # no safe chars -> empty


def test_ok_is_coerced_to_zero_or_one():
    for truthy in (1, True, "1", "true", "True"):
        assert _fields(_payload(ok=truthy))[2] == 1
    for falsy in (0, False, "0", "false", "", "yes", None):
        assert _fields(_payload(ok=falsy))[2] == 0


def test_markup_is_stripped_from_fields():
    # A crafted detail must not be able to inject HTML into the admin dashboard.
    out = _fields(_payload(detail="<script>alert(1)</script>"))
    assert "<" not in out[8] and ">" not in out[8]


def test_fields_are_length_clamped():
    out = _fields(_payload(os="L" * 100, detail="d" * 500))
    assert len(out[3]) <= 32
    assert len(out[8]) <= 200


def test_sanitize_keeps_safe_platform_tokens():
    assert _sanitize("openssl@3 (3.2)") == "openssl3 (3.2)"  # '@' dropped
    assert _sanitize("  arm64  ") == "arm64"
