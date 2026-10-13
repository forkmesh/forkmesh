#!/usr/bin/env python3
"""Web chat must fail *legibly* when the room key can't be derived.

Both chat surfaces (the standalone /chat page and the dashboard rail/full chat)
derive the shared room key with WebCrypto after fetching a passphrase. Two real
failures used to collapse into one dead-end status:

  * WebCrypto (crypto.subtle) is undefined outside a secure context, so a chat
    opened over plain HTTP (a self-hosted node / LAN IP on mobile) can never
    encrypt — yet the UI only said "Encryption unavailable", reading like a
    transient glitch.
  * An expired/absent session token 401s the room-key fetch — an auth problem,
    not a crypto one.

This pins the specific, actionable statuses so the chat "not showing up" is
diagnosable instead of silent.
"""

from pathlib import Path


PUBLIC = Path(__file__).resolve().parents[1] / "public"
CHAT = (PUBLIC / "chat.js").read_text(encoding="utf-8")
DASH_CHAT = (PUBLIC / "dashboard-chat.js").read_text(encoding="utf-8")


def test_both_surfaces_guard_secure_context():
    # A missing secure context (HTTP) is reported as needing HTTPS, up front,
    # before attempting key derivation.
    for source in (CHAT, DASH_CHAT):
        assert "window.isSecureContext" in source
        assert "window.crypto && window.crypto.subtle" in source
        assert "Chat needs a secure (HTTPS) connection" in source


def test_both_surfaces_distinguish_auth_failure():
    # The room-key fetch tags a 401/403 as an auth failure so connect() can tell
    # the user to sign in again rather than blaming encryption. The standalone
    # chat (also embedded in the World office) words this via its session
    # banner; the dashboard rail keeps the short status.
    for source in (CHAT, DASH_CHAT):
        assert 'res.status === 401 || res.status === 403 ? "auth" : "server"' in source
    assert "Log in again to use authorized channels" in CHAT
    assert "Sign in again to join chat" in DASH_CHAT
