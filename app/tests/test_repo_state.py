#!/usr/bin/env python3
"""Repo-state attestation: ref-advertisement parsing regression checks.

advertised_refs_canonical() must reduce a `git upload-pack --advertise-refs`
body to exactly the same canonical string the desktop node hashes and signs
(git for-each-ref --sort=refname over refs/heads/* + refs/tags/*, "<sha>
<refname>" lines, sorted by refname, joined by "\\n"). If the two ever drift,
every clone of an attested repo would fail the relay's integrity gate, so pin
the behaviour here. The fixtures deliberately use refs whose refname order
differs from their object-id order, so a whole-line (sha-first) sort fails.
"""

import ast
import hashlib
from pathlib import Path


SRC = Path(__file__).resolve().parents[1] / "src"
ENTRY = SRC / "entry.py"
_SOURCES = (ENTRY, SRC / "git_http.py")


def _load(name):
    """Load a single stdlib-only helper from the worker without the JS modules.

    The helper may live in entry.py or one of its extracted sibling modules
    (e.g. git_http.py); search each in turn for the named FunctionDef.
    """
    for source in _SOURCES:
        tree = ast.parse(source.read_text(encoding="utf-8"), filename=str(source))
        function = next(
            (
                node
                for node in tree.body
                if isinstance(node, ast.FunctionDef) and node.name == name
            ),
            None,
        )
        if function is not None:
            module = ast.fix_missing_locations(
                ast.Module(body=[function], type_ignores=[]))
            namespace = {}
            exec(compile(module, str(source), "exec"), namespace)
            return namespace[name]
    raise AssertionError("function not found: %s" % name)


advertised_refs_canonical = _load("advertised_refs_canonical")


def _pkt(payload: bytes) -> bytes:
    return ("%04x" % (len(payload) + 4)).encode() + payload


SHA_MAIN = "1111111111111111111111111111111111111111"
SHA_DEV = "2222222222222222222222222222222222222222"
SHA_TAG = "3333333333333333333333333333333333333333"
SHA_PEELED = "4444444444444444444444444444444444444444"


def _client_canonical(refs):
    # Mirror PublicMirrorRuntime::refsSha256FromForEachRef (what the desktop's
    # mirrorStateHash signs) and the mirror tools' refs_canonical(): "<sha>
    # <refname>" lines sorted BY REFNAME (`for-each-ref --sort=refname`),
    # joined by \n.
    return "\n".join(
        "%s %s" % (sha, name) for name, sha in sorted(refs.items()))


def test_canonical_matches_client_form():
    refs = {
        "refs/heads/main": SHA_MAIN,
        "refs/heads/dev": SHA_DEV,
        "refs/tags/v1": SHA_TAG,
    }
    # A realistic v0 advertisement: first ref carries NUL-delimited capabilities,
    # HEAD and a peeled tag line are present and must be dropped.
    body = (
        _pkt(("%s HEAD\x00multi_ack symref=HEAD:refs/heads/main\n" % SHA_MAIN).encode())
        + _pkt(("%s refs/heads/dev\n" % SHA_DEV).encode())
        + _pkt(("%s refs/heads/main\n" % SHA_MAIN).encode())
        + _pkt(("%s refs/tags/v1\n" % SHA_TAG).encode())
        + _pkt(("%s refs/tags/v1^{}\n" % SHA_PEELED).encode())
        + b"0000"
    )
    assert advertised_refs_canonical(body) == _client_canonical(refs)


def test_hash_round_trips():
    refs = {"refs/heads/main": SHA_MAIN, "refs/tags/v1": SHA_TAG}
    body = (
        _pkt(("%s HEAD\x00caps\n" % SHA_MAIN).encode())
        + _pkt(("%s refs/heads/main\n" % SHA_MAIN).encode())
        + _pkt(("%s refs/tags/v1\n" % SHA_TAG).encode())
        + b"0000"
    )
    canonical = advertised_refs_canonical(body)
    relay_hash = hashlib.sha256(canonical.encode()).hexdigest()
    client_hash = hashlib.sha256(_client_canonical(refs).encode()).hexdigest()
    assert relay_hash == client_hash


def test_non_served_refs_are_ignored():
    # refs/remotes/* and tool refs (refs/codex/*) are not served, so they must
    # not influence the fingerprint even if a mirror somehow advertises them.
    body = (
        _pkt(("%s HEAD\x00caps\n" % SHA_MAIN).encode())
        + _pkt(("%s refs/heads/main\n" % SHA_MAIN).encode())
        + _pkt(("%s refs/remotes/origin/main\n" % SHA_DEV).encode())
        + _pkt(("%s refs/codex/work\n" % SHA_TAG).encode())
        + b"0000"
    )
    assert advertised_refs_canonical(body) == "%s refs/heads/main" % SHA_MAIN


def test_empty_repo_is_stable():
    # An empty repo advertises only the zero-id capabilities line; both sides
    # then hash the empty string identically.
    body = _pkt(b"0" * 40 + b" capabilities^{}\x00caps\n") + b"0000"
    assert advertised_refs_canonical(body) == ""


def test_malformed_body_does_not_raise():
    assert advertised_refs_canonical(b"") == ""
    assert advertised_refs_canonical(b"zzzz garbage") == ""
    # Truncated length header (claims more bytes than present) stops cleanly.
    assert advertised_refs_canonical(b"00ff" + b"short") == ""
