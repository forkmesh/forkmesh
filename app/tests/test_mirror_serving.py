#!/usr/bin/env python3
"""Group-liveness for in-place mirror serving (adhoc #61): a repo whose own
source-of-truth host is offline is still advertised as cloneable/browsable when a
peer mirroring the same logical repo is online, so the website shows it as
available (served by a live mirror) instead of a bare "host offline"."""

import ast
from pathlib import Path


ENTRY = Path(__file__).resolve().parents[1] / "src" / "entry.py"
MIRRORS = ENTRY.parent / "mirrors.py"
# Mirror grouping / clone-selection helpers were extracted from entry.py into
# mirrors.py; parse both sources so the AST loaders below still find them.
_WORKER_SRC = ENTRY.read_text(encoding="utf-8") + "\n" + MIRRORS.read_text(encoding="utf-8")


def _load(*names, extra_globals=None):
    tree = ast.parse(_WORKER_SRC, filename=str(ENTRY))
    selected = [
        node
        for node in tree.body
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
        and node.name in names
    ]
    found = {node.name for node in selected}
    missing = set(names) - found
    assert not missing, "missing functions: %s" % sorted(missing)
    module = ast.fix_missing_locations(ast.Module(body=selected, type_ignores=[]))
    namespace = dict(extra_globals or {})
    exec(compile(module, str(ENTRY), "exec"), namespace)
    return [namespace[name] for name in names]


(
    served_mirror_groups,
    repo_clone_online,
    repo_mirror_group_key,
) = _load(
    "served_mirror_groups",
    "repo_clone_online",
    "repo_mirror_group_key",
)


def _rec(owner, name, *, root="root1", visibility="public", live=False):
    return {
        "owner": owner,
        "name": name,
        "rootCommit": root,
        "visibility": visibility,
        "liveHost": live,
    }


def test_online_public_member_marks_its_group_served():
    recs = [
        _rec("source", "forkmesh", live=False),
        _rec("mirror", "forkmesh", live=True),
    ]
    served = served_mirror_groups(recs)
    assert repo_mirror_group_key(recs[0]) in served


def test_offline_source_with_online_mirror_is_clone_online():
    recs = [
        _rec("source", "forkmesh", live=False),
        _rec("mirror", "forkmesh", live=True),
    ]
    served = served_mirror_groups(recs)
    # The offline source is reachable in place through its own URL via the mirror.
    assert repo_clone_online(recs[0], served) is True
    # The mirror itself is online directly.
    assert repo_clone_online(recs[1], served) is True


def test_offline_source_without_online_mirror_is_not_clone_online():
    recs = [
        _rec("source", "forkmesh", live=False),
        _rec("mirror", "forkmesh", live=False),
    ]
    served = served_mirror_groups(recs)
    assert repo_clone_online(recs[0], served) is False


def test_live_host_is_always_clone_online():
    rec = _rec("source", "forkmesh", live=True)
    assert repo_clone_online(rec, set()) is True


def test_private_repo_never_served_by_public_group():
    # An online PRIVATE node must not advertise a same-root public peer as served,
    # and a private repo gets no mirror fallback even with an online public peer.
    recs = [
        _rec("source", "secret", visibility="private", live=False),
        _rec("mirror", "secret", visibility="private", live=True),
    ]
    served = served_mirror_groups(recs)
    assert served == set()
    assert repo_clone_online(recs[0], served) is False


def test_offline_private_repo_with_online_public_peer_stays_offline():
    # Same root commit across a public mirror and an offline private repo: the
    # private repo still depends on its own host (no public-mirror fallback).
    recs = [
        _rec("owner", "secret", visibility="private", live=False),
        _rec("mirror", "secret", visibility="public", live=True),
    ]
    served = served_mirror_groups(recs)
    assert repo_clone_online(recs[0], served) is False


def test_different_root_fork_is_not_served():
    recs = [
        _rec("source", "forkmesh", root="rootA", live=False),
        _rec("mirror", "forkmesh", root="rootB", live=True),  # genuine fork
    ]
    served = served_mirror_groups(recs)
    assert repo_clone_online(recs[0], served) is False
