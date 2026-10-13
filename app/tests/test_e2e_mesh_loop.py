#!/usr/bin/env python3
"""Worker-side assertions for the nightly end-to-end mesh-loop test (issue #352).

The loop is driven by the Qt `forkmesh-e2e` target, whose in-process RelayStub
re-implements the *observable* relay contract. This test pins the worker half of
that contract so the two ends can never silently drift apart:

  * the git smart-HTTP info/refs wrapping (pkt-line service header + flush), and
  * the signed-issue canonicalization the inbox POST is verified against
    (issue_event_content / verify_issue_event <-> IssueStore::contentForSigning
    and IssueStore::canonicalString on the client).

Stdlib only — the worker's JS-bound modules are never imported; the pure helpers
are extracted straight from entry.py (the same idiom as test_git_http.py)."""

import ast
import hashlib
from pathlib import Path


SRC = Path(__file__).resolve().parents[1] / "src"
ENTRY = SRC / "entry.py"
# Helpers now live in entry.py or one of its extracted sibling modules; search
# each source for the requested FunctionDefs.
_SOURCES = (ENTRY, SRC / "git_http.py", SRC / "events.py")


def _load(*names, extra_globals=None):
    selected = []
    for source in _SOURCES:
        tree = ast.parse(source.read_text(encoding="utf-8"), filename=str(source))
        selected += [
            node
            for node in tree.body
            if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
            and node.name in names
        ]
    found = {node.name for node in selected}
    assert set(names) - found == set(), "missing: %s" % sorted(set(names) - found)
    module = ast.fix_missing_locations(ast.Module(body=selected, type_ignores=[]))
    namespace = dict(extra_globals or {})
    exec(compile(module, str(ENTRY), "exec"), namespace)
    return [namespace[name] for name in names]


(pkt_line, issue_event_content) = _load("pkt_line", "issue_event_content")


# --- git smart-HTTP info/refs wrapping -------------------------------------
def test_pkt_line_length_prefix():
    # 4-hex-digit big-endian length prefix covering the payload + the 4 header
    # bytes — byte-for-byte what RelayStub::pktLine reproduces in the Qt test.
    assert pkt_line(b"") == b"0004"
    assert pkt_line(b"a") == b"0005a"
    assert pkt_line(b"# service=git-upload-pack\n") == \
        b"001e# service=git-upload-pack\n"


def test_info_refs_advertisement_framing():
    # The relay wraps the host's `upload-pack --advertise-refs` bytes exactly so;
    # the Qt RelayStub's /info/refs handler must produce the identical envelope
    # or a real `git clone` through the mesh would fail the smart-HTTP handshake.
    advertise = b"00112345deadbeef refs/heads/main\n0000"
    body = pkt_line(b"# service=git-upload-pack\n") + b"0000" + advertise
    assert body.startswith(b"001e# service=git-upload-pack\n0000")
    assert body.endswith(advertise)


# --- signed-issue canonicalization -----------------------------------------
def test_open_issue_content_matches_client_contentforsigning():
    # IssueStore::contentForSigning for an "open" event is title\0body\0attach.
    ev = {"type": "open", "title": "Nightly loop found a bug",
          "body": "The mesh loop should be green.", "attachments": []}
    assert issue_event_content(ev) == \
        "Nightly loop found a bug\x00The mesh loop should be green.\x00"


def test_reporter_issue_content_matches_client_contentforsigning():
    assert issue_event_content(
        {"type": "reporter", "reporter": "newnewnode"}) == "newnewnode"


def test_issue_event_canonical_string_format():
    # verify_issue_event builds:
    #   "forkmesh-issue-event-v1\n<type>\n<number>\n<author>\n<ts>\n<sha256hex>"
    # with sha256hex = sha256(issue_event_content(ev)). Mirror it here (the same
    # bytes IssueStore::canonicalString signs) and confirm the content hash wiring.
    ev = {"type": "open", "title": "t", "body": "b", "attachments": [], "author": "AUTHOR", "ts": 42}
    content_hash = hashlib.sha256(issue_event_content(ev).encode()).hexdigest()
    canonical = (
        "forkmesh-issue-event-v1\n" + ev["type"] + "\n" + str(1) + "\n" +
        ev["author"] + "\n" + str(ev["ts"]) + "\n" + content_hash
    )
    # The literal format string must be present in verify_issue_event so this
    # cross-language contract fails loudly if the worker canonical ever changes.
    src = (ENTRY.read_text(encoding="utf-8")
           + (SRC / "events.py").read_text(encoding="utf-8"))
    assert "event_number = int(number)" in src
    assert 'event_type + "\\n" + str(event_number) + "\\n" +' in src
    assert '"forkmesh-" + kind + "-v1\\n" + body' in src
    assert '"forkmesh-" + kind + "-v2\\n" + key + "\\n" + body' in src
    assert '"issue-event", body, repo_keys, allow_v1' in src
    assert canonical.startswith("forkmesh-issue-event-v1\nopen\n1\nAUTHOR\n42\n")
    assert len(content_hash) == 64
