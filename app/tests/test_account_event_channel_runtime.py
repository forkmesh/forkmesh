#!/usr/bin/env python3
"""Runtime behaviour of the shared account event channel (adhoc #1604).

public/account-events.js is the browser half of the per-account ForkMeshNodes
push channel, and it is what lets /chat and /world read their unread counts
once on open instead of on a timer. That makes its liveness rules load-bearing,
so they are exercised here rather than only asserted as source text:

  * it trades the session for a ticket and puts only the ticket in the URL,
  * a signed-out or stale session goes dark (retrying would be a poll),
  * a transient relay failure keeps retrying with bounded backoff,
  * every (re)connect fires exactly one catch-up,
  * only {"type":"event"} frames reach the page, and only their topic.
"""

import json
import shutil
import subprocess
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[1]
CHANNEL = ROOT / "public" / "account-events.js"

# A fake WebSocket plus fake timers, so the whole reconnect ladder runs
# deterministically with no sockets, no clock and no network.
HARNESS = r'''
import { readFileSync } from "node:fs";
const source = readFileSync(process.argv[2], "utf8");
const url = "data:text/javascript;base64,"
  + Buffer.from(source).toString("base64");
const { createAccountEventChannel } = await import(url);

const log = [];
let timers = [];
let nextTimer = 1;
const setTimeoutImpl = (fn, ms) => {
  const id = nextTimer++;
  timers.push({ id, fn, ms });
  return id;
};
const clearTimeoutImpl = (id) => {
  timers = timers.filter((t) => t.id !== id);
};
// Fire every pending timeout once, in order, recording the delays used.
const drain = async () => {
  const pending = timers;
  timers = [];
  for (const t of pending) {
    log.push({ kind: "backoff", ms: t.ms });
    t.fn();
  }
  await new Promise((r) => setImmediate(r));
  await new Promise((r) => setImmediate(r));
};

const sockets = [];
class FakeSocket {
  constructor(href) {
    this.href = href;
    this.readyState = 0;
    this.sent = [];
    this.listeners = {};
    sockets.push(this);
    log.push({ kind: "open-attempt", href });
  }
  addEventListener(name, fn) {
    (this.listeners[name] ||= []).push(fn);
  }
  emit(name, event) {
    for (const fn of this.listeners[name] || []) fn(event);
  }
  send(payload) {
    this.sent.push(payload);
  }
  close() {
    this.readyState = 3;
    this.emit("close", {});
  }
  accept() {
    this.readyState = 1;
    this.emit("open", {});
  }
}

let ticketMode = process.argv[3];
let token = process.argv[4];
const fetchImpl = async (path) => {
  log.push({ kind: "ticket-request", path });
  if (ticketMode === "network-error") throw new Error("offline");
  if (ticketMode === "server-error") {
    return { ok: false, status: 503, json: async () => ({}) };
  }
  if (ticketMode === "unauthorized") {
    return { ok: false, status: 401, json: async () => ({}) };
  }
  if (ticketMode === "guest") {
    return {
      ok: true,
      status: 200,
      json: async () => ({ ok: true, authenticated: false, ticket: "" }),
    };
  }
  return {
    ok: true,
    status: 200,
    json: async () => ({ ok: true, authenticated: true, ticket: "TICKET-1" }),
  };
};

const channel = createAccountEventChannel({
  sessionToken: () => token,
  onTopic: (topic, frame) => log.push({
    kind: "topic", topic, repo: String(frame?.repo || ""),
  }),
  onConnected: () => log.push({ kind: "connected" }),
  onSignedOut: () => log.push({ kind: "signed-out" }),
  fetchImpl,
  WebSocketImpl: FakeSocket,
  locationLike: { protocol: "https:", host: "forkmesh.test" },
  setTimeoutImpl,
  clearTimeoutImpl,
  setIntervalImpl: (fn, ms) => {
    log.push({ kind: "keepalive", ms });
    return 99;
  },
  clearIntervalImpl: () => {},
});

const settle = async () => {
  for (let i = 0; i < 6; i += 1) {
    await new Promise((r) => setImmediate(r));
  }
};

channel.start();
await settle();

const scenario = process.argv[5];
if (scenario === "frames") {
  sockets[0].accept();
  // Only relay-originated event frames reach the page.
  sockets[0].emit("message", { data: JSON.stringify({ type: "pong" }) });
  sockets[0].emit("message", { data: "not json" });
  sockets[0].emit("message", { data: JSON.stringify({ type: "event" }) });
  sockets[0].emit("message", {
    data: JSON.stringify({ type: "event", topic: "pings" }),
  });
  sockets[0].emit("message", {
    data: JSON.stringify({ type: "event", topic: "direct-messages" }),
  });
  sockets[0].emit("message", {
    data: JSON.stringify({
      type: "event", topic: "catalog", repo: "alice/widgets",
    }),
  });
  await settle();
} else if (scenario === "reconnect") {
  sockets[0].accept();
  await settle();
  sockets[0].close();
  await settle();
  await drain();
  sockets[1].accept();
  await settle();
} else if (scenario === "retry-twice") {
  await drain();
  await drain();
} else if (scenario === "signin-later") {
  token = "session-abc";
  ticketMode = "ok";
  channel.restart();
  await settle();
}

process.stdout.write(JSON.stringify({
  log,
  sockets: sockets.map((s) => ({ href: s.href, sent: s.sent })),
  connected: channel.connected,
}));
'''


def _run(ticket_mode="ok", token="session-abc", scenario="none"):
    completed = subprocess.run(
        [
            "node", "--input-type=module", "-",
            str(CHANNEL), ticket_mode, token, scenario,
        ],
        input=HARNESS,
        text=True,
        capture_output=True,
        check=True,
    )
    return json.loads(completed.stdout)


pytestmark = pytest.mark.skipif(
    shutil.which("node") is None, reason="Node.js is unavailable")


def test_the_upgrade_url_carries_the_ticket_and_never_the_session_token():
    result = _run()
    # One session exchange, then one upgrade — in that order.
    assert [e["kind"] for e in result["log"] if e["kind"] != "keepalive"] == [
        "ticket-request", "open-attempt",
    ]
    attempts = [e for e in result["log"] if e["kind"] == "open-attempt"]
    assert len(attempts) == 1
    href = attempts[0]["href"]
    assert href == "wss://forkmesh.test/api/nodes/events?ticket=TICKET-1"
    assert "session-abc" not in href
    requests = [e for e in result["log"] if e["kind"] == "ticket-request"]
    assert [e["path"] for e in requests] == ["/api/accounts/event-ticket"]


def test_only_relay_event_frames_reach_the_page():
    result = _run(scenario="frames")
    topics = [e["topic"] for e in result["log"] if e["kind"] == "topic"]
    # pong, malformed JSON and a topic-less event frame are all dropped.
    assert topics == ["pings", "direct-messages", "catalog"]
    catalog = next(e for e in result["log"] if e.get("topic") == "catalog")
    assert catalog["repo"] == "alice/widgets"
    assert result["connected"] is True
    # The keepalive matches the desktop node's cadence.
    keepalive = [e for e in result["log"] if e["kind"] == "keepalive"]
    assert keepalive == [{"kind": "keepalive", "ms": 4 * 60 * 1000}]


def test_every_reconnect_fires_exactly_one_catch_up():
    result = _run(scenario="reconnect")
    kinds = [e["kind"] for e in result["log"]]
    assert kinds.count("connected") == 2
    assert kinds.count("open-attempt") == 2
    # Each reconnect re-mints a ticket rather than reusing the expired one.
    assert kinds.count("ticket-request") == 2
    # ...and it waits before retrying.
    assert [e["ms"] for e in result["log"] if e["kind"] == "backoff"] == [2000]


def test_a_transient_relay_failure_retries_with_bounded_backoff():
    result = _run(ticket_mode="server-error", scenario="retry-twice")
    assert [e["ms"] for e in result["log"] if e["kind"] == "backoff"] == [
        2000, 4000,
    ]
    assert not [e for e in result["log"] if e["kind"] == "open-attempt"]
    # Same for a hard network failure.
    offline = _run(ticket_mode="network-error", scenario="retry-twice")
    assert [e["ms"] for e in offline["log"] if e["kind"] == "backoff"] == [
        2000, 4000,
    ]


def test_a_signed_out_or_stale_session_goes_dark_instead_of_polling():
    # No token at all: not even one request.
    empty = _run(token="")
    assert empty["log"] == []

    # A stale token the relay answers 200 + authenticated:false. Exactly one
    # request, then silence — retrying this forever would be a poll. The one
    # onSignedOut callback is not a request; it is how the page learns.
    guest = _run(ticket_mode="guest", scenario="retry-twice")
    assert [e["kind"] for e in guest["log"]] == ["ticket-request", "signed-out"]

    # And the same for an outright 401.
    denied = _run(ticket_mode="unauthorized", scenario="retry-twice")
    assert [e["kind"] for e in denied["log"]] == ["ticket-request", "signed-out"]


def test_a_refused_ticket_reports_signed_out_exactly_once():
    """The revocation signal that replaced the dashboard's 20s session poll.

    Every (re)connect trades the stored session for a fresh ticket, so a session
    revoked anywhere surfaces here for no extra request. It must fire once and
    stay dark: a callback that re-armed the channel would be the poll again.
    """
    for mode in ("unauthorized", "guest"):
        result = _run(ticket_mode=mode, scenario="retry-twice")
        kinds = [e["kind"] for e in result["log"]]
        assert kinds.count("signed-out") == 1, mode
        # Dark afterwards: no socket, and no second ticket request from the
        # two drained backoff rounds.
        assert kinds.count("ticket-request") == 1, mode
        assert not [e for e in result["log"] if e["kind"] == "open-attempt"], mode

    # A transient failure must NOT be reported as a sign-out — that would boot
    # a signed-in user off the site whenever the relay hiccuped.
    for mode in ("server-error", "network-error"):
        result = _run(ticket_mode=mode, scenario="retry-twice")
        assert not [e for e in result["log"] if e["kind"] == "signed-out"], mode


def test_signing_in_later_brings_the_channel_up():
    result = _run(ticket_mode="guest", token="", scenario="signin-later")
    kinds = [e["kind"] for e in result["log"]]
    # Nothing while signed out, then a ticket and a socket after restart().
    assert kinds == ["ticket-request", "open-attempt"]
    assert result["sockets"][0]["href"].endswith("ticket=TICKET-1")
