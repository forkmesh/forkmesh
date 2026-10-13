#!/usr/bin/env python3
"""Guests and World visitors are forgotten after 10 idle minutes (adhoc #404).

A browser tab that opens /chat or /world joins the room as "Guest 1667" /
"World Guest f49ab8" (or, from older clients, "World visitor · jett"). That id
dies with the tab, so an offline row for one can never come back — yet both the
web people pane and the desktop users column retained them like nodes, ending up
with dozens of dead guests next to a handful of live people.

Contracts pinned here:

  * chat.js has a 10-minute visitor idle window, a predicate that recognises
    guests/World visitors (but not accounts or nodes), and a sweep that deletes
    them from the roster on every people-pane paint.
  * The desktop shares one definition of "transient visitor" + idle window
    (ChatVisitorPresence.h), applies it when retaining peers that dropped off
    the roster (MainWindowMessages.cpp), and reaps visitor peers on the shorter
    window instead of the hour a real node gets (ServerNode.cpp).
"""

import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
QT_SRC = ROOT.parent / "desktop" / "src"
CHAT = (ROOT / "public" / "chat.js").read_text(encoding="utf-8")
VISITOR_H = (QT_SRC / "ChatVisitorPresence.h").read_text(encoding="utf-8")
SERVER_NODE = (QT_SRC / "ServerNode.cpp").read_text(encoding="utf-8")
MESSAGES = (QT_SRC / "MainWindowMessages.cpp").read_text(encoding="utf-8")


def _js_function(source: str, name: str) -> str:
    start = source.index(f"function {name}(")
    depth = 0
    for index in range(start, len(source)):
        char = source[index]
        if char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return source[start:index + 1]
    raise AssertionError(f"chat.js must define {name}()")


def test_web_chat_has_ten_minute_visitor_window():
    assert "const VISITOR_IDLE_FORGET_MS = 10 * 60 * 1000;" in CHAT


def test_web_chat_recognises_guests_and_world_visitors():
    predicate = _js_function(CHAT, "personIsTransientVisitor")
    # Anonymous guests are flagged by their advertised kind…
    assert 'person.kind === "guest"' in predicate
    # …and legacy/older clients by the name they assert.
    assert "world\\s+visitor" in predicate
    assert "world\\s+guest" in predicate
    # Never our own row: the local visitor must not delete itself.
    assert "person.id === selfId" in predicate


def test_web_chat_sweeps_idle_visitors_on_every_paint():
    sweep = _js_function(CHAT, "forgetIdleVisitors")
    assert "Date.now() - VISITOR_IDLE_FORGET_MS" in sweep
    assert "personIsTransientVisitor(person)" in sweep
    assert "roster.delete(id)" in sweep
    people = _js_function(CHAT, "renderPeople")
    assert "forgetIdleVisitors();" in people
    # The 30s repaint timer doubles as the sweep timer in a silent room.
    assert re.search(r"setInterval\(renderPeople,\s*30000\)", CHAT)


def test_desktop_shares_one_visitor_definition():
    assert "constexpr qint64 kVisitorIdleMs = 600000;" in VISITOR_H
    assert "isTransientVisitor" in VISITOR_H and "visitorIsIdle" in VISITOR_H
    for source in (SERVER_NODE, MESSAGES):
        assert '#include "ChatVisitorPresence.h"' in source


def test_desktop_drops_idle_visitors_from_the_users_column():
    start = MESSAGES.index("void MainWindow::setRoster(")
    body = MESSAGES[start:start + 6000]
    assert "ChatVisitorPresence::isTransientVisitor(prev.accountKind" in body
    assert "ChatVisitorPresence::visitorIsIdle(lastSeen, nowMs)" in body
    # Real nodes/accounts still keep their offline row for the Node dropdown.
    assert "offline.online = false;" in body


def test_desktop_reaps_visitor_peers_on_the_short_window():
    start = SERVER_NODE.index("void ServerNode::reapStalePeers()")
    body = SERVER_NODE[start:start + 1200]
    assert "ChatVisitorPresence::kVisitorIdleMs" in body
    assert "kPeerReapMs" in body


if __name__ == "__main__":
    for name, fn in sorted(globals().items()):
        if name.startswith("test_") and callable(fn):
            fn()
            print(f"ok {name}")
