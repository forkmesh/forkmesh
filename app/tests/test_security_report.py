#!/usr/bin/env python3
"""Unit tests for the private vulnerability reporting endpoint.

Loads _validate_security_report and SECURITY_REPORT_COMPONENTS directly from
src/entry.py via AST extraction (no Workers JS runtime required), following
the same pattern used by test_install_diag.py.
"""

import ast
from pathlib import Path

ENTRY = Path(__file__).resolve().parents[1] / "src" / "entry.py"

_WANT_FUNCS = ("_validate_security_report",)
_WANT_CONSTS = ("SECURITY_REPORT_COMPONENTS",)


def _load():
    tree = ast.parse(ENTRY.read_text(encoding="utf-8"), filename=str(ENTRY))
    body = []
    for node in tree.body:
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)) and node.name in _WANT_FUNCS:
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
_validate = _NS["_validate_security_report"]
COMPONENTS = _NS["SECURITY_REPORT_COMPONENTS"]


def _payload(**overrides):
    data = {
        "title": "RCE via crafted git pack",
        "body": "Sending a malformed pack triggers an out-of-bounds read in the relay.",
        "component": "relay",
        "contact": "researcher@example.com",
    }
    data.update(overrides)
    return data


def test_valid_report_is_returned():
    out = _validate(_payload())
    assert out is not None
    assert out["title"] == "RCE via crafted git pack"
    assert out["component"] == "relay"
    assert out["contact"] == "researcher@example.com"


def test_contact_is_optional():
    out = _validate(_payload(contact=""))
    assert out is not None
    assert out["contact"] == ""

    out2 = _validate({k: v for k, v in _payload().items() if k != "contact"})
    assert out2 is not None
    assert out2["contact"] == ""


def test_all_known_components_accepted():
    for comp in COMPONENTS:
        out = _validate(_payload(component=comp))
        assert out is not None, f"component {comp!r} was rejected"
        assert out["component"] == comp


def test_unknown_component_rejected():
    assert _validate(_payload(component="rce")) is None
    assert _validate(_payload(component="")) is None
    assert _validate(_payload(component="rm-rf")) is None


def test_missing_title_rejected():
    assert _validate(_payload(title="")) is None
    assert _validate({k: v for k, v in _payload().items() if k != "title"}) is None


def test_missing_body_rejected():
    assert _validate(_payload(body="")) is None
    assert _validate({k: v for k, v in _payload().items() if k != "body"}) is None


def test_non_dict_rejected():
    assert _validate(None) is None
    assert _validate("string") is None
    assert _validate([]) is None


def test_title_is_length_clamped():
    out = _validate(_payload(title="A" * 500))
    assert out is not None
    assert len(out["title"]) <= 200


def test_body_is_length_clamped():
    out = _validate(_payload(body="B" * 20000))
    assert out is not None
    assert len(out["body"]) <= 16384


def test_contact_is_length_clamped():
    out = _validate(_payload(contact="c" * 300))
    assert out is not None
    assert len(out["contact"]) <= 254


def test_whitespace_is_stripped():
    out = _validate(_payload(title="  XSS via title  ", body="  details  "))
    assert out["title"] == "XSS via title"
    assert out["body"] == "details"
