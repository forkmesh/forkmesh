#!/usr/bin/env python3
"""Keep the desktop's organization proofs byte-identical to the relay's.

Settings -> Organizations signs each request with a canonical string the
worker rebuilds from scratch (`_org_admin_signed_session` in app/src/entry.py).
If either side renames a proof, reorders a bound body field, or teaches one
surface an operation the other does not know, the signature simply fails to
verify — and the panel reports "sign in" to an operator who plainly is signed
in. Nothing at build or run time catches that, so the two tables are pinned
against each other here.
"""

from __future__ import annotations

import ast
import pathlib
import re
import sys


ROOT = pathlib.Path(__file__).resolve().parents[2]
PANEL = (ROOT / "desktop/src/MainWindowOrganizations.cpp").read_text(
    encoding="utf-8")
INTERNAL = (ROOT / "desktop/src/MainWindowInternal.h").read_text(
    encoding="utf-8")
ENTRY_PATH = ROOT / "app/src/entry.py"
ENTRY = ENTRY_PATH.read_text(encoding="utf-8")

# The desktop path family <-> the relay regex it mirrors.
PATH_FAMILIES = {
    "kOrgsCollectionPattern": "ORGS_RE",
    "kOrgItemPattern": "ORG_RE",
    "kOrgMembersPattern": "ORG_MEMBERS_RE",
    "kOrgTeamsPattern": "ORG_TEAMS_RE",
    "kOrgTeamMembersPattern": "ORG_TEAM_MEMBERS_RE",
    "kOrgReposPattern": "ORG_REPOS_RE",
}


def failed(message: str) -> None:
    print("org-admin proof contract: " + message, file=sys.stderr)
    sys.exit(1)


def relay_proof_name(cpp_name: str) -> str:
    """kOrgAdminMemberSetProof -> ORG_ADMIN_MEMBER_SET_PROOF."""
    return "_".join(
        word.upper() for word in re.findall(r"[A-Z][a-z0-9]*", cpp_name[1:]))


def relay_proof_values() -> dict[str, str]:
    values = {}
    for node in ast.parse(ENTRY, filename=str(ENTRY_PATH)).body:
        if not isinstance(node, ast.Assign) or len(node.targets) != 1:
            continue
        target = node.targets[0]
        if not isinstance(target, ast.Name):
            continue
        if not target.id.startswith("ORG_ADMIN_") or not isinstance(
                node.value, ast.Constant):
            continue
        values[target.id] = node.value.value
    return values


def relay_operations() -> dict:
    """(relay regex name, method) -> (proof constant, ordered fields)."""
    tree = ast.parse(ENTRY, filename=str(ENTRY_PATH))
    function = next(
        (node for node in tree.body
         if isinstance(node, ast.FunctionDef)
         and node.name == "_org_admin_operation"), None)
    if function is None:
        failed("_org_admin_operation not found in app/src/entry.py")
    operations = {}

    def literal(node) -> tuple:
        if isinstance(node, ast.Tuple):
            return tuple(element.value for element in node.elts)
        return ()

    for family_branch in function.body:
        if not isinstance(family_branch, ast.If):
            continue
        call = family_branch.test
        if not isinstance(call, ast.Call) or not isinstance(
                call.func, ast.Attribute):
            continue
        family = call.func.value.id
        for method_branch in family_branch.body:
            if not isinstance(method_branch, ast.If):
                continue
            method = method_branch.test.comparators[0].value
            returned = method_branch.body[0].value
            if isinstance(returned, ast.Name):
                operations[(family, method)] = (returned.id, ())
            elif isinstance(returned, ast.Tuple):
                operations[(family, method)] = (
                    returned.elts[0].id, literal(returned.elts[1]))
    return operations


DESKTOP_ENTRY = re.compile(
    r"\{(k\w+Pattern),\s*\"(\w+)\",\s*(kOrgAdmin\w+Proof),\s*"
    r"\{([^}]*)\}\}", re.S)


def desktop_operations() -> dict:
    body = PANEL[PANEL.index("organizationAdminOperations()"):]
    body = body[:body.index("return table;")]
    operations = {}
    for pattern, method, proof, fields in DESKTOP_ENTRY.findall(body):
        if pattern not in PATH_FAMILIES:
            failed("unknown desktop path family %s" % pattern)
        operations[(PATH_FAMILIES[pattern], method)] = (
            relay_proof_name(proof), tuple(re.findall(r'"(\w+)"', fields)))
    return operations


def main() -> None:
    cpp_values = dict(
        re.findall(
            r'const QString (kOrgAdmin\w+Proof)\s*=\s*(?:QStringLiteral\()?\s*'
            r'"([^"]+)"', INTERNAL, re.S))
    relay_values = relay_proof_values()
    if not relay_values:
        failed("no ORG_ADMIN_*_PROOF constants found in app/src/entry.py")

    for name, value in relay_values.items():
        if name == "ORG_ADMIN_FIELD_MAX":
            continue
        twin = next(
            (cpp for cpp in cpp_values if relay_proof_name(cpp) == name), None)
        if twin is None:
            failed("%s has no desktop twin" % name)
        if cpp_values[twin] != value:
            failed("%s is %r on the relay but %r in the desktop"
                   % (name, value, cpp_values[twin]))
    for cpp in cpp_values:
        if relay_proof_name(cpp) not in relay_values:
            failed("%s has no relay twin in app/src/entry.py" % cpp)

    # The signed form of every body field is bounded, so a longer description
    # would be truncated differently on the two sides.
    desktop_max = re.search(
        r"constexpr int kOrgAdminFieldMax = (\d+);", INTERNAL)
    if not desktop_max:
        failed("kOrgAdminFieldMax not found in MainWindowInternal.h")
    if str(relay_values.get("ORG_ADMIN_FIELD_MAX")) != desktop_max.group(1):
        failed("field bound differs: relay %s, desktop %s"
               % (relay_values.get("ORG_ADMIN_FIELD_MAX"),
                  desktop_max.group(1)))

    relay = relay_operations()
    desktop = desktop_operations()
    for key in sorted(set(relay) | set(desktop)):
        if relay.get(key) != desktop.get(key):
            failed("%s: relay %r, desktop %r"
                   % (key, relay.get(key), desktop.get(key)))

    print("org-admin proof contract: %d proofs, %d operations in lockstep"
          % (len(relay_values) - 1, len(relay)))


if __name__ == "__main__":
    main()
