#!/usr/bin/env python3
"""Every call inside entry.py must fit the signature it targets.

adhoc #225: /api/accounts/users returned a 500 for every visitor with
TypeError('_account_chat_user_payload() takes from 1 to 3 positional
arguments but 4 were given'). Nothing was wrong with either edit that led
there. One branch grew the payload builder a fourth `email_activity`
parameter — signature, body and call site together — while main
independently rewrote that same signature line to carry an activity
bucket. Git merged both cleanly: main's three-parameter `def` line won,
the branch's four-argument call survived below it, and the mismatch sat in
a file no interpreter reads until a request arrives.

entry.py is one ~40k-line module deployed to a Python Worker, so a bad
call is not a build error, an import error or a lint failure anywhere in
CI — it is a 500 on the first request that reaches that line, and only on
that line. The worker's other tests read entry.py as source text, which
catches renamed helpers and moved call sites but never notices that a call
and its `def` disagree about how many arguments exist.

This test parses the module and checks every call to a function defined at
its top level against that function's own signature. It is deliberately
conservative: `*args` forwarding, `**kwargs` splats and any call it cannot
resolve statically are skipped, so a pass means "no call provably cannot
run", not "every call is correct". That is enough to have caught adhoc
#225 before it shipped, and it costs one AST walk.
"""

import ast
from pathlib import Path


ENTRY_PATH = Path(__file__).resolve().parents[1] / "src" / "entry.py"


def _top_level_signatures(tree):
    """Map each top-level function name to what its signature accepts."""
    out = {}
    for node in tree.body:
        if not isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
            continue
        args = node.args
        positional = args.posonlyargs + args.args
        out[node.name] = {
            # Defaults fill the tail of the positional list, so anything
            # ahead of them has to be supplied by the caller.
            "required": len(positional) - len(args.defaults),
            # None means *args, which swallows any number of extras.
            "max_positional": None if args.vararg else len(positional),
            "names": ({a.arg for a in positional}
                      | {a.arg for a in args.kwonlyargs}),
            "takes_kwargs": bool(args.kwarg),
            "line": node.lineno,
        }
    return out


def _mismatches(source):
    tree = ast.parse(source)
    signatures = _top_level_signatures(tree)
    found = []
    for node in ast.walk(tree):
        if not isinstance(node, ast.Call):
            continue
        # Only bare `name(...)` calls resolve to a known top-level def;
        # attribute calls belong to objects this test cannot reason about.
        if not isinstance(node.func, ast.Name):
            continue
        sig = signatures.get(node.func.id)
        if sig is None:
            continue
        # A splat can contribute any number of arguments, so a call using
        # one is unprovable either way. Skip rather than guess.
        if any(isinstance(a, ast.Starred) for a in node.args):
            continue
        if any(k.arg is None for k in node.keywords):
            continue

        positional = len(node.args)
        keywords = [k.arg for k in node.keywords]
        where = f"line {node.lineno} -> {node.func.id}() def line {sig['line']}"

        if (sig["max_positional"] is not None
                and positional > sig["max_positional"]):
            found.append(
                f"{where}: {positional} positional arguments passed, "
                f"signature takes at most {sig['max_positional']}")
        elif positional + len(keywords) < sig["required"]:
            found.append(
                f"{where}: {positional + len(keywords)} arguments passed, "
                f"signature requires {sig['required']}")
        elif not sig["takes_kwargs"]:
            unknown = sorted(k for k in keywords if k not in sig["names"])
            if unknown:
                found.append(
                    f"{where}: keyword argument(s) {unknown} are not "
                    "parameters of the signature")
    return found


def test_entry_calls_match_their_signatures():
    """No call in entry.py can raise TypeError on arity alone."""
    mismatches = _mismatches(ENTRY_PATH.read_text(encoding="utf-8"))
    assert not mismatches, (
        "entry.py calls a function it defines with arguments that signature "
        "cannot accept. Each of these is a guaranteed 500 the moment the "
        "line is reached in production, and is almost always a merge that "
        "kept one side's `def` and the other side's call:\n  "
        + "\n  ".join(mismatches))


def test_check_catches_the_adhoc_225_regression():
    """The check must fail on the exact shape that shipped the 500.

    Without this the arity test could silently degrade into a no-op — a
    skipped branch or a renamed helper map would make it pass on anything.
    """
    regression = (
        'def _account_chat_user_payload(rec, total_active_ms=0,'
        ' activity_bucket=""):\n'
        "    return {}\n"
        "\n"
        "def _account_users_directory():\n"
        "    return _account_chat_user_payload(rec, 0, '', email_activity)\n"
    )
    mismatches = _mismatches(regression)
    assert len(mismatches) == 1, mismatches
    assert "_account_chat_user_payload()" in mismatches[0]
    assert "4 positional arguments passed" in mismatches[0]
    assert "at most 3" in mismatches[0]


def test_check_tolerates_splats_and_defaults():
    """Legitimate call shapes must not be reported."""
    fine = (
        "def helper(a, b=1, *rest, flag=False, **extra):\n"
        "    return a\n"
        "\n"
        "def plain(a, b):\n"
        "    return a\n"
        "\n"
        "def caller(items, opts):\n"
        "    helper(1)\n"
        "    helper(1, 2, 3, 4, flag=True, anything=1)\n"
        "    helper(*items)\n"
        "    plain(1, b=2)\n"
        "    plain(**opts)\n"
        "    return unknown_external(1, 2, 3)\n"
    )
    assert _mismatches(fine) == []
