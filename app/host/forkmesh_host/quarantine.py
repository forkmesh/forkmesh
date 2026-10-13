"""Which core nodes are quarantined, as this app host sees it (docs/IDS.md).

The root IDS on a node that detects an intrusion (``deploy/core/ids.py``)
writes a ``self`` row to ``cluster_quarantine`` signed with its own IDS key;
peers whose IDS sees another node's IDS heartbeat stop write signed ``vote``
rows; the operator lifts a quarantine with a ``release`` row signed by the
desktop-only operator key. ``ids_records`` verifies all of them.

What a quarantine changes here:

* the node is never live (``cluster.live_members``): no Durable Objects, no
  cron coordinator, and its DNS votes are ignored (the DNS reconciler counts
  live voters only);
* peers refuse it peer unseal (``vault.serve_peer``) and its mesh requests
  (``durable_cluster.MeshASGI``);
* on the quarantined node itself the app seals (DATA_KEY leaves memory),
  refuses every unseal, heart-beats ``quarantined`` and ``/readyz`` reports
  ``quarantined``;
* after a release, peers still refuse it peer unseal until the operator has
  unsealed it once from the desktop (``next_ids.py release`` does both); that
  socket unseal stamps ``reunsealed_ms`` on the release row.

Configuration (unit environment, rendered from deploy/fleet.toml):
``FORKMESH_IDS_PUBKEYS`` (``n1=<b64u>,...``), ``FORKMESH_IDS_OPERATOR_PUBKEY``,
and ``FORKMESH_HOST_DATA_DIR`` (the sticky file ``ids-quarantine.json``: rows
deleted from rqlite never lift a quarantine this host has seen). Without
node keys the view is off and nothing changes.
"""

from __future__ import annotations

import json
import os
import time
from pathlib import Path

from . import ids_records

REFRESH_SECONDS = 5.0
STICKY_FILE = "ids-quarantine.json"


class QuarantineView:
    def __init__(self, node_id, nodes, node_keys, operator_key, rqlite=None,
                 data_dir=None):
        self.node_id = node_id
        self.nodes = sorted(set(nodes or ()) | set(node_keys or ()))
        self.node_keys = dict(node_keys or {})
        self.operator_key = operator_key or ""
        self.rqlite = rqlite
        self.sticky_path = (Path(data_dir) / STICKY_FILE) if data_dir else None
        self.state: dict = {}
        self.sticky: dict = self._load_sticky()
        self.refreshed_at = 0.0
        self._error_logged = 0.0
        self._last_logged: dict = {}
        self.stats = {"refreshes": 0, "errors": 0, "invalid_rows": 0}
        # Apply the sticky set before the first read: a restart of this host
        # never forgets a quarantine it has seen.
        self.apply_rows([])

    @property
    def enabled(self):
        return bool(self.node_keys)

    # -- state -------------------------------------------------------------
    def _load_sticky(self):
        if self.sticky_path is None:
            return {}
        try:
            data = json.loads(self.sticky_path.read_text("utf-8"))
        except (OSError, ValueError):
            return {}
        return {str(k): {str(r): int(t) for r, t in (v or {}).items()}
                for k, v in (data or {}).items() if isinstance(v, dict)}

    def _save_sticky(self):
        if self.sticky_path is None:
            return
        try:
            tmp = self.sticky_path.with_name(self.sticky_path.name + ".tmp")
            tmp.write_text(json.dumps(self.sticky, sort_keys=True), "utf-8")
            os.replace(tmp, self.sticky_path)
        except OSError as error:
            print("ids: cannot write %s: %s" % (self.sticky_path,
                                                type(error).__name__),
                  flush=True)

    def apply_rows(self, rows):
        state = ids_records.quarantine_state(
            rows, self.node_keys, self.operator_key, self.nodes)
        state, sticky = ids_records.merge_sticky(state, self.sticky)
        self.stats["invalid_rows"] = sum(s.get("invalid", 0)
                                         for s in state.values())
        if sticky != self.sticky:
            self.sticky = sticky
            self._save_sticky()
        for node, current in state.items():
            flag = bool(current.get("quarantined"))
            if self._last_logged.get(node) != flag and (
                    flag or node in self._last_logged):
                print("ids: node %s %s (%s)" % (
                    node, "QUARANTINED" if flag else "no longer quarantined",
                    "; ".join(current.get("reasons") or ()) or "released"),
                    flush=True)
            self._last_logged[node] = flag
        self.state = state
        return state

    async def refresh(self):
        if not self.enabled or self.rqlite is None:
            return self.state
        try:
            rows = await self.rqlite.query(ids_records.SELECT_SQL,
                                           level="weak")
        except Exception as error:
            self.stats["errors"] += 1
            if time.monotonic() - self._error_logged > 60:
                self._error_logged = time.monotonic()
                print("ids: quarantine read failed: %s" % (
                    type(error).__name__,), flush=True)
            return self.state
        self.stats["refreshes"] += 1
        self.refreshed_at = time.monotonic()
        return self.apply_rows(rows)

    # -- queries -----------------------------------------------------------
    def quarantined(self):
        return ids_records.quarantined_nodes(self.state)

    def is_quarantined(self, node):
        return bool((self.state.get(node) or {}).get("quarantined"))

    def peer_unseal_blocked(self, node):
        return ids_records.peer_unseal_blocked(self.state, node)

    @property
    def self_quarantined(self):
        return self.is_quarantined(self.node_id)

    @property
    def self_needs_reunseal(self):
        current = self.state.get(self.node_id) or {}
        release = current.get("release")
        return (not current.get("quarantined") and bool(release)
                and not release.get("reunsealed_ms"))

    async def mark_reunsealed(self, now_ms=None):
        """The operator unsealed this released node from the desktop: from
        now on peers may unseal it again (ids_records.peer_unseal_blocked)."""
        release = (self.state.get(self.node_id) or {}).get("release")
        if not release or self.rqlite is None:
            return False
        stamp = int(now_ms or time.time() * 1000)
        await self.rqlite.execute([[
            "UPDATE cluster_quarantine SET reunsealed_ms=? WHERE id=? AND "
            "node_id=? AND kind='release' AND reunsealed_ms IS NULL",
            stamp, release["id"], self.node_id]])
        release["reunsealed_ms"] = stamp
        print("ids: released node %s unsealed from the desktop; peers may "
              "unseal it again" % self.node_id, flush=True)
        return True

    def snapshot(self):
        return {"enabled": self.enabled,
                "quarantined": sorted(self.quarantined()),
                "self": {"quarantined": self.self_quarantined,
                         "needsDesktopUnseal": self.self_needs_reunseal},
                "stats": dict(self.stats)}


def from_environ(environ, node_id, nodes, rqlite=None):
    """The view for a cluster host, or None when no IDS keys are rendered."""
    environ = environ or {}

    def get(name):
        return str(environ.get(name) or os.environ.get(name) or "")
    keys = ids_records.parse_pubkeys(get("FORKMESH_IDS_PUBKEYS"))
    if not keys:
        return None
    operator = get("FORKMESH_IDS_OPERATOR_PUBKEY").strip()
    if operator and not ids_records.PUB_RE.fullmatch(operator):
        print("ids: FORKMESH_IDS_OPERATOR_PUBKEY is malformed; releases "
              "cannot be verified", flush=True)
        operator = ""
    data_dir = get("FORKMESH_HOST_DATA_DIR") or None
    return QuarantineView(node_id, nodes, keys, operator, rqlite, data_dir)
