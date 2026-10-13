#!/usr/bin/env python3

import ast
import importlib.util
from pathlib import Path

SRC = Path(__file__).resolve().parents[1] / "src"
ENTRY = (SRC / "entry.py").read_text(encoding="utf-8")


def _rbac():
    spec = importlib.util.spec_from_file_location(
        "organization_rbac_roster", SRC / "organization_rbac.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _function(name):
    for node in ast.parse(ENTRY).body:
        if getattr(node, "name", None) == name:
            return ast.get_source_segment(ENTRY, node)
    raise AssertionError(name)


def test_team_roster_reads_need_the_roster_capability():
    handler = _function("org_team_members_handler")
    read = handler[handler.index('if method == "GET":'):
                   handler.index('if method not in ("POST", "DELETE"):')]
    assert '"organization.roster.read"' in read
    assert "_org_rbac.org_role_allows(" in read


def test_guests_lack_the_roster_capability():
    rbac = _rbac()
    assert not rbac.org_role_allows("guest", "organization.roster.read")
    assert rbac.org_role_allows("member", "organization.roster.read")
