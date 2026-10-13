#!/usr/bin/env python3
"""Peer mirror-request feature contracts (issue #385).

A repo owner can ask another node to mirror their repo; the target's holder
gets a notification and, on accept, that node starts mirroring. These tests
cover the pure request-lifecycle helpers, the worker route/handler/notification
wiring, the heartbeat delivery + ack rail, and the dashboard UI wiring.
"""
import sys
from pathlib import Path

from _dashboard_bundle import assembled_dashboard_js

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"
if str(SRC) not in sys.path:
    sys.path.insert(0, str(SRC))

import mirrors  # noqa: E402

ENTRY = SRC / "entry.py"
ENTRY_TEXT = (
    ENTRY.read_text(encoding="utf-8") + "\n"
    + (SRC / "schema.py").read_text(encoding="utf-8") + "\n"
    + (SRC / "app_runtime.py").read_text(encoding="utf-8"))
QT_HEARTBEAT = ROOT.parent / "desktop" / "src" / "MainWindowSetup.cpp"
QT_HEADER = ROOT.parent / "desktop" / "src" / "MainWindow.h"


# --- pure helpers -----------------------------------------------------------

def test_request_id_is_stable_and_scoped_to_repo_and_target():
    a = mirrors.mirror_request_id("Alice", "Repo", "Bob")
    b = mirrors.mirror_request_id("alice", "repo", "bob")
    assert a == b == "alice/repo@bob"
    assert mirrors.mirror_request_id("alice", "repo", "carol") != a


def test_add_mirror_request_dedupes_by_id_and_is_bounded():
    reqs = []
    entry = {"id": "alice/r@bob", "owner": "alice", "repo": "r",
             "status": "pending", "ts": 1}
    reqs = mirrors.add_mirror_request(reqs, entry)
    # Re-adding the same id replaces (newest-first) rather than duplicating.
    reqs = mirrors.add_mirror_request(
        reqs, {**entry, "ts": 2, "status": "pending"})
    assert len(reqs) == 1
    assert reqs[0]["ts"] == 2
    # Bounded to MAX_MIRROR_REQUESTS newest entries.
    for i in range(mirrors.MAX_MIRROR_REQUESTS + 10):
        reqs = mirrors.add_mirror_request(
            reqs, {"id": "alice/r%d@bob" % i, "owner": "alice",
                   "repo": "r%d" % i, "status": "pending", "ts": 100 + i})
    assert len(reqs) == mirrors.MAX_MIRROR_REQUESTS


def test_set_status_and_accepted_and_ack_roundtrip():
    reqs = mirrors.add_mirror_request(
        [], {"id": "alice/r@bob", "owner": "alice", "repo": "r",
             "status": "pending", "ts": 1})
    # Pending requests are NOT delivered to the node.
    assert mirrors.accepted_mirror_requests(reqs) == []
    reqs, updated = mirrors.set_mirror_request_status(
        reqs, "alice/r@bob", "accepted", ts=5)
    assert updated["status"] == "accepted" and updated["resolvedAt"] == 5
    delivered = mirrors.accepted_mirror_requests(reqs)
    assert delivered == [{"id": "alice/r@bob", "owner": "alice", "repo": "r"}]
    # Acking the delivered id drops it so it isn't redelivered.
    reqs = mirrors.ack_mirror_requests(reqs, ["alice/r@bob"])
    assert mirrors.accepted_mirror_requests(reqs) == []
    # A rejected request is never delivered and is untouched by acks.
    reqs2 = mirrors.add_mirror_request(
        [], {"id": "alice/r@carol", "owner": "alice", "repo": "r",
             "status": "pending", "ts": 1})
    reqs2, _ = mirrors.set_mirror_request_status(
        reqs2, "alice/r@carol", "rejected", ts=9)
    assert mirrors.accepted_mirror_requests(reqs2) == []
    assert mirrors.ack_mirror_requests(reqs2, ["alice/r@carol"]) == reqs2


# --- worker wiring ----------------------------------------------------------

def test_worker_exposes_mirror_requests_route_and_handler():
    assert 'url.path in ("/api/mirror-requests", "/api/mirror-requests/")' in ENTRY_TEXT
    assert "return await mirror_requests_handler(self.env, request)" in ENTRY_TEXT
    assert "async def mirror_requests_handler" in ENTRY_TEXT


def test_mirror_request_notification_kind_is_registered():
    # In the allowlist (else notification_payload coerces it away) and email map.
    assert '"mirror_request",\n})' in ENTRY_TEXT or '"mirror_request"' in ENTRY_TEXT
    assert '"mirror_request": True' in ENTRY_TEXT


def test_create_requires_owner_session_and_public_repo():
    # Only the repo owner (proven by the session, not a body field) may ask,
    # and only for a public, existing repo.
    handler = ENTRY_TEXT.split("async def mirror_requests_handler", 1)[1]
    handler = handler.split("async def subscribe_handler", 1)[0]
    assert "_authed_account_name(env, request, data)" in handler
    assert "if owner != actor:" in handler
    assert "_repo_is_private(env, owner, repo)" in handler
    assert 'enqueue_notification(\n            env, target, "mirror_request"' in handler
    assert "add_mirror_request(" in handler
    # Accept/reject path resolves the parked request and flips its status.
    assert 'action in ("accept", "reject")' in handler
    assert "set_mirror_request_status(" in handler


def test_heartbeat_delivers_accepted_requests_and_consumes_acks():
    hb = ENTRY_TEXT.split("async def _account_heartbeat", 1)[1]
    hb = hb.split("async def ", 2)[0]
    assert "accepted_mirror_requests(rec.get(\"mirror_requests\"))" in hb
    assert 'response["mirrorRequests"] = pending_mirrors' in hb
    assert "ack_mirror_requests(rec.get(\"mirror_requests\"), ack_ids)" in hb
    assert 'data.get("mirrorRequestsAck")' in hb


# --- dashboard wiring -------------------------------------------------------

def test_dashboard_wires_ask_and_accept_reject_controls():
    js = assembled_dashboard_js()
    # Owner-only "ask a node to mirror" control + its POST.
    assert "data-mirror-request-send" in js
    assert "async function askNodeToMirror" in js
    assert "renderMirrorRequestForm" in js
    assert 'fetch("/api/mirror-requests"' in js
    # Accept/Reject on the incoming notification.
    assert "function mirrorRequestActionsHtml" in js
    assert "data-mirror-request-accept" in js
    assert "data-mirror-request-reject" in js
    assert "async function resolveMirrorRequest" in js
    # New notification kind gets an icon (else it falls back to the bell).
    assert "mirror_request:" in js


# --- desktop node wiring ----------------------------------------------------

def test_desktop_node_mirrors_accepted_requests_and_acks_them():
    cpp = QT_HEARTBEAT.read_text(encoding="utf-8")
    assert 'resp.value(QStringLiteral("mirrorRequests")).toArray()' in cpp
    assert "mirrorNetworkRepo(owner, repo, QString(), false)" in cpp
    assert 'body.insert(QStringLiteral("mirrorRequestsAck")' in cpp
    header = QT_HEADER.read_text(encoding="utf-8")
    assert "m_handledMirrorRequests" in header
    assert "m_pendingMirrorRequestAcks" in header


if __name__ == "__main__":
    for test in (
        test_request_id_is_stable_and_scoped_to_repo_and_target,
        test_add_mirror_request_dedupes_by_id_and_is_bounded,
        test_set_status_and_accepted_and_ack_roundtrip,
        test_worker_exposes_mirror_requests_route_and_handler,
        test_mirror_request_notification_kind_is_registered,
        test_create_requires_owner_session_and_public_repo,
        test_heartbeat_delivers_accepted_requests_and_consumes_acks,
        test_dashboard_wires_ask_and_accept_reject_controls,
        test_desktop_node_mirrors_accepted_requests_and_acks_them,
    ):
        test()
        print("PASS", test.__name__)
