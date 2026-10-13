#!/usr/bin/env python3
"""The Network panel may never contradict itself (issue #623).

A production report showed "5 Nodes" sitting directly above "5 online · 5
offline", and "0 Active relays" next to a relay card reading "relay healthy"
with "Active" uptime. Both came from the same mistake: a tile rendered from a
different source than the thing it summarises — the headline total from
``stats.hosts`` (live nodes only) over a list that also carries offline nodes,
and "Active relays" from the flagship chat room's client count, which says
nothing about whether a relay is up.

The real dashboard functions are lifted out of the assembled bundle and run in
Node against a stub DOM, so the arithmetic and the writes are exercised as
shipped rather than only grepped for.
"""

import json
import os
import shutil
import subprocess
from pathlib import Path

import pytest

from _dashboard_bundle import assembled_dashboard_js


ROOT = Path(__file__).resolve().parents[1]
VIEW = ROOT / "public" / "dashboard" / "partials" / "views" / "network.html"
DASHBOARD_JS = assembled_dashboard_js()
VIEW_HTML = VIEW.read_text(encoding="utf-8")

# Lifted verbatim from the bundle; every other name they touch is stubbed below.
LIFTED = (
    "formatCount",
    "formatSize",
    "escapeHtml",
    "nodeDotClass",
    "nodeMetaLabel",
    "nodeDetailChips",
    "networkOnlineOnly",
    "networkRowsMatchingPageFilter",
    "renderNetworkRows",
    "renderRelayState",
    "renderNetwork",
)

HARNESS = r"""
// --- stub DOM ---------------------------------------------------------------
class Element {
  constructor() {
    this.textContent = "";
    this.innerHTML = "";
    this.attributes = {};
    this.classes = new Set();
    this.classList = {
      toggle: (name, on) => {
        if (on) this.classes.add(name);
        else this.classes.delete(name);
      },
    };
  }
  setAttribute(name, value) { this.attributes[name] = value; }
  querySelector() { return new Element(); }
  get text() {
    // What a reader sees: the pill's label lives in its re-baked markup.
    return this.textContent || this.innerHTML.replace(/<[^>]*>/g, "").trim();
  }
}

const NODES = {};
for (const selector of [
  "[data-network-hosts]", "[data-network-repos]", "[data-network-relays]",
  "[data-network-uptime]", "[data-network-relay-health]",
  "[data-network-node-count]", "[data-network-node-list]",
  "[data-network-online-only]",
]) NODES[selector] = new Element();

const $ = (selector) => NODES[selector] || null;
const state = {};
const window = { lucide: { createIcons() {} } };

// --- stubbed collaborators --------------------------------------------------
let PAGE_QUERIES = [];
const pageFilterQueries = () => PAGE_QUERIES;
const emptyListHtml = (fallback) => escapeHtml(fallback);
let OVERVIEW = null;
const fetchJson = async () => {
  if (OVERVIEW instanceof Error) throw OVERVIEW;
  return OVERVIEW;
};

__LIFTED__

// --- driver -----------------------------------------------------------------
const snapshot = () => ({
  nodes: NODES["[data-network-hosts]"].text,
  breakdown: NODES["[data-network-node-count]"].text,
  relays: NODES["[data-network-relays]"].text,
  uptime: NODES["[data-network-uptime]"].text,
  health: NODES["[data-network-relay-health]"].text,
  healthClasses: [...NODES["[data-network-relay-health]"].classes].sort(),
  listed: (NODES["[data-network-node-list]"].innerHTML.match(/font-medium truncate/g) || []).length,
});

const out = {};
const INPUT = JSON.parse(process.env.PANEL_INPUT);

// The reported production shape: five nodes live, ten seen in the uptime window.
OVERVIEW = INPUT.overview;
await renderNetwork();
out.reported = snapshot();

// The same load with the header search narrowing the list.
PAGE_QUERIES = ["vm"];
await renderNetwork();
out.filtered = snapshot();
PAGE_QUERIES = [];

// Offline nodes revealed by the "Online only" toggle: the totals must hold.
state.networkOnlineOnly = false;
renderNetworkRows(state.networkNodeRows);
out.showingOffline = snapshot();
state.networkOnlineOnly = true;

// A relay that does not answer must not keep claiming health.
OVERVIEW = new Error("HTTP 503");
await renderNetwork();
out.relayDown = snapshot();
out.relayDownList = NODES["[data-network-node-list]"].innerHTML;

console.log(JSON.stringify(out));
"""


def _js_function(source: str, name: str) -> str:
    """Source text of one ``function name(...) {...}`` in the bundle."""
    for opener in ("function %s(" % name, "async function %s(" % name):
        start = source.find("  " + opener)
        if start >= 0:
            break
    else:
        raise AssertionError("dashboard.js must define %s()" % name)
    start += 2
    depth = 0
    for index in range(source.index("{", start), len(source)):
        char = source[index]
        if char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return source[start:index + 1]
    raise AssertionError("unbalanced %s()" % name)


def _run_panel(overview) -> dict:
    node = shutil.which("node")
    if not node:  # pragma: no cover - node is present in CI and on dev boxes
        pytest.skip("node is required to run the dashboard panel")
    lifted = "\n\n".join(_js_function(DASHBOARD_JS, name) for name in LIFTED)
    harness = HARNESS.replace("__LIFTED__", lifted)
    result = subprocess.run(
        [node, "--input-type=module", "-e", harness],
        capture_output=True,
        text=True,
        timeout=60,
        env={**os.environ, "PANEL_INPUT": json.dumps({"overview": overview})},
    )
    assert result.returncode == 0, result.stderr
    return json.loads(result.stdout)


def _overview(online, uptime_names):
    return {
        "ok": True,
        "stats": {
            # The live-node count the panel used to print as its headline total.
            "hosts": len(online),
            "repos": 12,
            # The flagship room's chat clients: zero here, exactly as reported.
            "clients": 0,
            "onlineNodes": online,
        },
        "leaderboards": {
            "uptime": [{"name": name, "minutes": 60} for name in uptime_names],
            "nodes": [{"name": name, "platform": "linux"} for name in uptime_names],
        },
        "history": {"hours": [{"nodeMinutes": 300}]},
    }


def _population(breakdown: str) -> int:
    """online + offline, read back off the rendered breakdown line."""
    counts = dict(
        (word, int(number))
        for number, word in (chunk.split(" ") for chunk in breakdown.split(" · "))
    )
    assert set(counts) <= {"online", "offline"}, breakdown
    return counts.get("online", 0) + counts.get("offline", 0)


ONLINE = ["vm1", "vm2", "vm3", "mirror29", "mirror30"]
SEEN = ONLINE + ["vm4", "vm5", "mirror27", "mirror28", "mirror31"]


@pytest.fixture(scope="module")
def panel():
    return _run_panel(_overview(ONLINE, SEEN))


def test_node_total_is_the_online_plus_offline_population(panel):
    # The exact contradiction from the report: five live nodes, five more seen
    # offline inside the uptime window, and a headline that claimed five.
    assert panel["reported"]["breakdown"] == "5 online · 5 offline"
    assert panel["reported"]["nodes"] == "10"
    # The list itself still defaults to the live nodes only.
    assert panel["reported"]["listed"] == 5


def test_node_total_tracks_the_breakdown_through_filter_and_toggle(panel):
    # Whatever narrows the list narrows both numbers together — the tile and
    # the breakdown are written from one row set, so they cannot disagree.
    for shape in (panel["reported"], panel["filtered"], panel["showingOffline"]):
        assert shape["nodes"] == str(_population(shape["breakdown"]))
    assert panel["filtered"]["nodes"] == "5"  # vm1..vm5
    assert panel["filtered"]["breakdown"] == "3 online · 2 offline"
    assert panel["showingOffline"]["listed"] == 10


def test_active_relays_follows_the_relay_card_not_the_chat_client_count(panel):
    # stats.clients is 0 in this payload; a relay that just served the panel is
    # one active relay, and the card next to it says the same thing.
    assert panel["reported"]["relays"] == "1"
    assert panel["reported"]["uptime"] == "Active"
    assert panel["reported"]["health"] == "relay healthy"
    assert "text-foreground" in panel["reported"]["healthClasses"]


def test_an_unreachable_relay_stops_claiming_health(panel):
    assert panel["relayDown"]["relays"] == "0"
    assert panel["relayDown"]["uptime"] == "Idle"
    assert panel["relayDown"]["health"] == "relay unreachable"
    assert "text-muted-foreground" in panel["relayDown"]["healthClasses"]
    assert "Network data is unavailable right now." in panel["relayDownList"]


def test_network_view_binds_the_relay_tiles_to_relay_state():
    assert "data-network-relay-health" in VIEW_HTML
    assert "data-network-relays" in VIEW_HTML
    # The chat-client binding is gone from this panel entirely.
    assert "data-network-clients" not in VIEW_HTML
    assert "stats.clients" not in DASHBOARD_JS[
        DASHBOARD_JS.index("async function renderNetwork()"):
        DASHBOARD_JS.index("function notificationIcon(")
    ]
    # Nothing is asserted about the relay before it answers: the pill ships in
    # its checking state and the tiles ship as placeholders.
    assert "checking relay" in VIEW_HTML
    assert "relay healthy" not in VIEW_HTML
