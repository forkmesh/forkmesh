"""Cluster membership, placement and the internal mesh for Durable Objects.

Enabled when ``FORKMESH_NODE_ID`` and ``FORKMESH_MESH_LISTEN`` are set and
the D1 binding is rqlite (``FORKMESH_HOST_DB=rqlite:URL``); otherwise the
host keeps the single-process ``LocalDurableHost``.

Configuration (process environment):

* ``FORKMESH_NODE_ID``: this node's id (``n1``...). Also its rqlite node id
  unless ``FORKMESH_RQLITE_NODE_MAP`` (``rqliteid=nodeid,...``) says
  otherwise (only used to display which node holds the rqlite leader).
* ``FORKMESH_HOST_CRON_MINUTE_MS``: length of a cron minute (tests only;
  default 60000). ``FORKMESH_HOST_HEARTBEAT_MS`` / ``FORKMESH_HOST_STALE_MS``
  shorten the heartbeat and the liveness window the same way.
* ``FORKMESH_MESH_LISTEN``: ``host:port`` of the internal mesh listener (the
  WireGuard address in production).
* ``FORKMESH_MESH_URL``: advertised base URL (default ``http://<listen>``).
* ``FORKMESH_MESH_SECRET``: shared HMAC-SHA256 key for mesh requests.
* ``FORKMESH_CLUSTER_RQLITE_URL``: the local rqlite HTTP API (default: the
  ``rqlite:`` URL of ``FORKMESH_HOST_DB``).
* ``FORKMESH_MESH_PEERS``: the static fleet map ``n1=http://10.77.0.1:8792,
  n2=...`` rendered from ``deploy/fleet.toml``. It is the **only** source of
  member ids and mesh addresses (audit H3): a ``cluster_members`` row whose
  node id is not in the map is ignored, and a row's ``mesh_url`` is never
  used for routing (the map's address is). Without a map, a ``vps`` host
  (``FORKMESH_PLATFORM``) routes only to itself; other platforms (tests,
  dev) keep trusting the rows.

Membership lives in rqlite ``cluster_members``. It is leaderless: each node
upserts only its **own** row every 5 s (heartbeat_ms, and status ``active``
or ``sealed``) and nobody writes anyone else's row. Every node reads the
whole table from the rqlite leader (``level=weak``, so all nodes see the same
rows) once a second and derives liveness itself: a member is live when its
heartbeat is at most 15 s old and it is not ``sealed`` (vault.py). The
status column is for display only.

Placement, computed identically on every node from that live set:

* ``hash``: rendezvous hashing, the live node with the highest
  ``sha256(ns:name:node_id)`` (rooms, world, notes, the snapshot/mirror-wake
  doorbells, DiscordGate);
* ``rotate``: the per-minute cron coordinator (CronRunner
  ``scheduled-runner-v1``) and CronWatchdog move round-robin across the live
  nodes, ``live_sorted[(h(ns:name) + index) mod N]``, where ``index`` counts
  cron minutes whose hand-off instant is 45 s into the previous minute (a
  tick is bounded to 35 s, so a hand-off never lands inside one);
* ``local``: the fetch-only ingress objects run on every node.

rqlite's own Raft leader is only used as rqlite uses it (writes and weak
reads are forwarded to it); no host-level role depends on it. A node that
has not reached an rqlite leader for 10 s loses quorum: it stops hosting and
``/readyz`` answers 503.

Mesh requests: ``<mesh_url>/_do/<ns>/<quoted name>/<original path>`` (HTTP
and WebSocket) and ``/_rpc/<ns>/<quoted name>/<method>``, each signed with
``x-forkmesh-mesh-auth: v1:<ms>:<nonce>:<hex hmac>`` over
``method\\npath\\nms\\nnonce\\nsha256(body)``; 30 s skew window and a
nonce replay cache.
"""

from __future__ import annotations

import asyncio
import base64
import contextlib
import hashlib
import hmac
import json
import os
import time
import urllib.parse

import httpx

from . import d1_caller

AUTH_HEADER = "x-forkmesh-mesh-auth"
SKEW_MS = 30_000
# A heartbeat more than this far in the future is forged or from a broken
# clock; such a row is never live (a far-future row would stay live forever).
FUTURE_SKEW_MS = 60_000


def _env_int(name, default):
    try:
        value = int(os.environ.get(name) or 0)
    except ValueError:
        value = 0
    return value if value > 0 else default


HEARTBEAT_SECONDS = _env_int("FORKMESH_HOST_HEARTBEAT_MS", 5_000) / 1000.0
STALE_MS = _env_int("FORKMESH_HOST_STALE_MS", 15_000)
QUORUM_LOSS_SECONDS = 10.0
JOIN_WINDOW_SECONDS = 60.0
DRAIN_SECONDS = _env_int("FORKMESH_HOST_DRAIN_MS", 2_500) / 1000.0
_QUARANTINE_REFRESH_SECONDS = 5.0
VIEW_SECONDS = 1.0
ALARM_SCAN_SECONDS = 10.0
# The cron minute and the instant, inside it, where rotating objects change
# owner: 45 s into the minute, after the tick that started at +1.5 s has hit
# its 35 s dispatch bound, and well before the next alarm at +61.5 s.
CRON_MINUTE_MS = _env_int("FORKMESH_HOST_CRON_MINUTE_MS", 60_000)
CRON_HANDOFF_MS = CRON_MINUTE_MS * 3 // 4

# Round-robin per cron minute (see the module docstring). The coordinator is
# the one CronRunner name that owns the per-minute alarm; the other named
# CronRunner singletons (snapshot-republish, mirror-wake) are doorbells and
# are hashed like everything else.
ROTATING_CLASSES = frozenset({"ForkMeshCronWatchdog"})
ROTATING_NAMED = {"ForkMeshCronRunner": frozenset({"scheduled-runner-v1"})}
CRON_COORDINATOR = ("ForkMeshCronRunner", "scheduled-runner-v1")
# ForkMeshCronRunner also carries the fetch-only ingress objects
# ("app-ingress-v1:<hex>", "app-git-ingress-v1:<hex>"): they own no storage
# or alarms, so every node runs its own copy instead of funnelling all app
# traffic through one node.
LOCAL_NAME_PREFIXES = {
    "ForkMeshCronRunner": ("app-ingress-v1:", "app-git-ingress-v1:"),
}

_MESH_STRIP = frozenset({
    "host", "connection", "upgrade", "keep-alive", "transfer-encoding",
    "content-length", "te", "trailer", "proxy-connection",
    "sec-websocket-key", "sec-websocket-version", "sec-websocket-extensions",
    "sec-websocket-protocol", "sec-websocket-accept", AUTH_HEADER,
    d1_caller.HEADER, "x-fm-do-url", "x-fm-do-id", "x-fm-do-cf"})


def now_ms():
    return time.time_ns() // 1_000_000


def placement(ns, name):
    """``local`` | ``rotate`` | ``hash`` for one object."""
    prefixes = LOCAL_NAME_PREFIXES.get(ns)
    if prefixes and name.startswith(prefixes):
        return "local"
    if ns in ROTATING_CLASSES or name in ROTATING_NAMED.get(ns, ()):
        return "rotate"
    return "hash"


def rendezvous(ns, name, node_ids):
    best = None
    best_score = b""
    for node_id in node_ids:
        score = hashlib.sha256(
            ("%s:%s:%s" % (ns, name, node_id)).encode()).digest()
        if best is None or score > best_score:
            best, best_score = node_id, score
    return best


def rendezvous_order(key, node_ids):
    """Every node, best first, for one key (used for fail-over order)."""
    return sorted(node_ids, key=lambda node_id: hashlib.sha256(
        ("%s:%s" % (key, node_id)).encode()).digest(), reverse=True)


def rotation_index(now=None, minute_ms=None, handoff_ms=None):
    """The cron minute a rotating object currently serves.

    Index ``s`` runs from ``handoff`` into minute ``s-1`` until ``handoff``
    into minute ``s``, so it covers the whole tick of minute ``s`` (which
    starts at +1.5 s and is bounded to 35 s)."""
    minute_ms = minute_ms or CRON_MINUTE_MS
    handoff_ms = CRON_HANDOFF_MS if handoff_ms is None else handoff_ms
    now = now_ms() if now is None else int(now)
    return (now + minute_ms - handoff_ms) // minute_ms


def next_handoff_ms(now=None, minute_ms=None, handoff_ms=None):
    minute_ms = minute_ms or CRON_MINUTE_MS
    handoff_ms = CRON_HANDOFF_MS if handoff_ms is None else handoff_ms
    index = rotation_index(now, minute_ms, handoff_ms)
    return index * minute_ms + handoff_ms


def rotation_owner(ns, name, node_ids, index):
    """``sorted(node_ids)[(h(ns:name) + index) mod N]``: round-robin, the
    same answer on every node that sees the same live set."""
    nodes = sorted(node_ids)
    if not nodes:
        return None
    offset = int.from_bytes(hashlib.sha256(
        ("%s:%s" % (ns, name)).encode()).digest()[:8], "big")
    return nodes[(offset + int(index)) % len(nodes)]


def live_members(rows, now=None, stale_ms=None, exclude=()):
    """Live node ids from ``cluster_members`` rows: fresh heartbeat, not
    sealed and not quarantined (``exclude``: the verified quarantine set,
    quarantine.py; docs/IDS.md). Pure, so every node derives the same set
    from the same rows."""
    now = now_ms() if now is None else now
    cutoff = now - (STALE_MS if stale_ms is None else stale_ms)
    exclude = set(exclude or ())
    return tuple(sorted(
        row["node_id"] for row in rows
        if cutoff <= int(row.get("heartbeat_ms") or 0) <= now + FUTURE_SKEW_MS
        and row.get("status") not in ("sealed", "quarantined", "leaving")
        and row["node_id"] not in exclude))


def parse_peers(text):
    """``n1=http://10.77.0.1:8792,n2=...`` -> {node: base url} (the same
    format as vault.parse_peers; only ``http://`` mesh URLs)."""
    peers = {}
    for item in str(text or "").split(","):
        name, sep, url = item.partition("=")
        name, url = name.strip(), url.strip().rstrip("/")
        if sep and name and url.startswith("http://"):
            peers[name] = url
    return peers


def trusted_rows(rows, peers, self_id, strict, now=None, log=None):
    """Keep only rows of nodes in the static map (strict mode) whose
    heartbeat is not in the future. Returns {node_id: row}; in strict mode
    each row's ``mesh_url`` is replaced by the map's address."""
    now = now_ms() if now is None else now
    out = {}
    for row in rows or ():
        node = str((row or {}).get("node_id") or "")
        if not node:
            continue
        if strict and node != self_id and node not in peers:
            if log:
                log("unknown_node", node)
            continue
        try:
            beat = int(row.get("heartbeat_ms") or 0)
        except (TypeError, ValueError):
            beat = 0
        if beat > now + FUTURE_SKEW_MS:
            if log:
                log("future_heartbeat", node)
            continue
        if strict:
            row = dict(row)
            stored = str(row.get("mesh_url") or "").rstrip("/")
            expected = peers.get(node, "")
            if stored and expected and stored != expected and log:
                log("mesh_url_mismatch", node)
            row["mesh_url"] = expected
        out[node] = row
    return out


def member_state(row, now=None, stale_ms=None):
    """Display status derived from a row: live / stale / sealed."""
    now = now_ms() if now is None else now
    cutoff = now - (STALE_MS if stale_ms is None else stale_ms)
    if int((row or {}).get("heartbeat_ms") or 0) < cutoff:
        return "stale"
    status = (row or {}).get("status")
    return status if status in ("sealed", "leaving") else "live"


# -- HMAC -------------------------------------------------------------------

def sign(secret, method, path, body=b"", ts=None, nonce=None):
    ts = int(ts if ts is not None else now_ms())
    nonce = nonce or os.urandom(9).hex()
    digest = hashlib.sha256(body or b"").hexdigest()
    message = "\n".join((method.upper(), path, str(ts), nonce, digest))
    mac = hmac.new(secret.encode(), message.encode(), hashlib.sha256)
    return "v1:%d:%s:%s" % (ts, nonce, mac.hexdigest())


class Verifier:
    def __init__(self, secret, skew_ms=SKEW_MS):
        self.secret = secret or ""
        self.skew_ms = skew_ms
        self._seen: dict = {}

    def precheck(self, header, now=None):
        """Cheap checks before any body is read (audit M4): the header is
        well-formed, the timestamp is inside the window and the nonce is
        unused. Consumes nothing; ``verify`` still decides."""
        if not self.secret or not header:
            return False
        try:
            version, ts, nonce, mac = str(header).split(":", 3)
            ts = int(ts)
        except ValueError:
            return False
        now = now_ms() if now is None else now
        return (version == "v1" and abs(now - ts) <= self.skew_ms
                and 0 < len(nonce) <= 64 and nonce not in self._seen
                and len(mac) == 64
                and all(c in "0123456789abcdef" for c in mac))

    def verify(self, header, method, path, body=b""):
        if not self.secret or not header:
            return False
        try:
            version, ts, nonce, mac = str(header).split(":", 3)
            ts = int(ts)
        except ValueError:
            return False
        now = now_ms()
        if version != "v1" or abs(now - ts) > self.skew_ms or not nonce \
                or len(nonce) > 64:
            return False
        expected = sign(self.secret, method, path, body, ts, nonce)
        if not hmac.compare_digest(expected.rsplit(":", 1)[1], mac):
            return False
        if len(self._seen) > 4096:
            self._seen = {k: v for k, v in self._seen.items() if v > now}
        if nonce in self._seen:
            return False
        self._seen[nonce] = ts + 2 * self.skew_ms
        return True


# -- value codec (storage rows and RPC) -------------------------------------

def encode_value(value):
    return json.dumps(_tag(value), separators=(",", ":"))


def decode_value(text):
    return _untag(json.loads(text))


def _tag(value):
    from . import js as _js
    from .ffi import JsNull
    if value is None or isinstance(value, JsNull):
        return None
    if isinstance(value, (bool, int, float, str)):
        return value
    if isinstance(value, _js.Uint8Array):
        return {"__fm__": "u8", "b": base64.b64encode(
            _js._as_bytes(value)).decode()}
    if isinstance(value, _js.ArrayBuffer):
        return {"__fm__": "ab", "b": base64.b64encode(
            bytes(value._data)).decode()}
    if isinstance(value, (bytes, bytearray, memoryview)):
        return {"__fm__": "u8", "b": base64.b64encode(bytes(value)).decode()}
    if isinstance(value, _js.JsDate):
        return {"__fm__": "date", "ms": value.getTime()}
    if isinstance(value, dict):
        return {str(k): _tag(v) for k, v in value.items()}
    if isinstance(value, (list, tuple)):
        return [_tag(v) for v in value]
    to_py = getattr(value, "to_py", None)
    if callable(to_py):
        return _tag(to_py())
    raise TypeError("value is not storable: %r" % type(value).__name__)


def _untag(value):
    from . import js as _js
    from .ffi import JsArray, JsObject
    if isinstance(value, dict):
        kind = value.get("__fm__")
        if kind in ("u8", "ab") and len(value) == 2:
            raw = base64.b64decode(value["b"])
            return (_js.Uint8Array.new(raw) if kind == "u8"
                    else _js.ArrayBuffer(raw))
        if kind == "date" and len(value) == 2:
            return _js.JsDate(value["ms"])
        return JsObject((k, _untag(v)) for k, v in value.items())
    if isinstance(value, list):
        return JsArray(_untag(v) for v in value)
    return value


# -- rqlite ------------------------------------------------------------------

class RqliteError(RuntimeError):
    pass


class Rqlite:
    """Tiny async client for the local rqlite node (cluster bookkeeping)."""

    def __init__(self, url, username=None, password=None, timeout=3.0):
        self.url = url.rstrip("/")
        auth = (username, password) if username else None
        self._client = httpx.AsyncClient(
            timeout=timeout, auth=auth,
            limits=httpx.Limits(max_connections=8,
                                max_keepalive_connections=4))
        self._leader_route = True

    async def status(self):
        response = await self._client.get(self.url + "/status")
        response.raise_for_status()
        return response.json()

    async def leader(self):
        if self._leader_route:
            response = await self._client.get(self.url + "/leader")
            if response.status_code == 503:
                return ""
            if response.status_code != 404:
                response.raise_for_status()
                return str(response.json().get("id") or "")
            self._leader_route = False
        status = await self.status()
        return str(((status.get("store") or {}).get("leader") or {})
                   .get("node_id") or "")

    async def execute(self, statements, transaction=True):
        params = "?transaction" if transaction else ""
        response = await self._client.post(
            self.url + "/db/execute" + params, json=statements)
        response.raise_for_status()
        results = response.json().get("results") or []
        for result in results:
            if result.get("error"):
                raise RqliteError(result["error"])
        return results

    async def query(self, sql, *params, level="none"):
        response = await self._client.post(
            self.url + "/db/query?associative&level=" + level,
            json=[[sql, *params]])
        response.raise_for_status()
        results = response.json().get("results") or [{}]
        if results[0].get("error"):
            raise RqliteError(results[0]["error"])
        return results[0].get("rows") or []

    async def aclose(self):
        await self._client.aclose()


# -- configuration -----------------------------------------------------------

class ClusterConfig:
    def __init__(self, node_id, mesh_listen, mesh_url, secret, rqlite_url,
                 rqlite_node_map=None, username=None, password=None,
                 peers=None, strict=False, ids_env=None, replica=False):
        self.node_id = node_id
        self.replica = bool(replica)
        self.mesh_listen = mesh_listen
        self.mesh_url = mesh_url.rstrip("/")
        self.secret = secret
        self.rqlite_url = rqlite_url
        self.rqlite_node_map = rqlite_node_map or {}
        self.username = username
        self.password = password
        # Static fleet map (FORKMESH_MESH_PEERS); this node always maps to
        # its own advertised URL.
        self.peers = dict(peers or {})
        if self.peers or strict:
            self.peers[node_id] = self.mesh_url
        self.strict = bool(strict or peers)
        # IDS quarantine (quarantine.py): node public keys, the operator key
        # and the data dir for the sticky set. Empty: quarantine off.
        self.ids_env = dict(ids_env or {})

    @classmethod
    def from_environ(cls, environ, values=None):
        values = values or {}

        def get(name):
            return environ.get(name) or values.get(name) or ""

        node_id = get("FORKMESH_NODE_ID")
        listen = get("FORKMESH_MESH_LISTEN")
        if not node_id or not listen:
            return None
        rqlite_url = get("FORKMESH_CLUSTER_RQLITE_URL")
        if not rqlite_url:
            spec = get("FORKMESH_HOST_DB")
            if spec.startswith("rqlite:"):
                rqlite_url = spec[len("rqlite:"):]
            elif spec.startswith(("http://", "https://")):
                rqlite_url = spec
        if not rqlite_url:
            raise RuntimeError("cluster mode needs an rqlite database "
                               "(FORKMESH_HOST_DB=rqlite:URL)")
        secret = get("FORKMESH_MESH_SECRET")
        if len(secret) < 16:
            raise RuntimeError("FORKMESH_MESH_SECRET must be set "
                               "(16+ characters) in cluster mode")
        mapping = {}
        for item in get("FORKMESH_RQLITE_NODE_MAP").split(","):
            left, sep, right = item.partition("=")
            if sep and left.strip() and right.strip():
                mapping[left.strip()] = right.strip()
        peers = parse_peers(get("FORKMESH_MESH_PEERS"))
        strict = bool(peers) or (
            get("FORKMESH_PLATFORM").strip().lower() == "vps")
        replica = get("FORKMESH_CLUSTER_ROLE").strip().lower() == "replica"
        if replica and not peers:
            raise RuntimeError("a replica needs FORKMESH_MESH_PEERS "
                               "(the core nodes it routes to)")
        ids_env = {name: get(name) for name in (
            "FORKMESH_IDS_PUBKEYS", "FORKMESH_IDS_OPERATOR_PUBKEY",
            "FORKMESH_HOST_DATA_DIR")}
        return cls(node_id, listen,
                   get("FORKMESH_MESH_URL") or "http://" + listen,
                   secret, rqlite_url, mapping,
                   get("RQLITE_USERNAME") or None,
                   get("RQLITE_PASSWORD") or None,
                   peers=peers, strict=strict, ids_env=ids_env,
                   replica=replica)


# -- membership --------------------------------------------------------------

class Membership:
    """Own-row heartbeat, the leaderless member view and the quorum flag."""

    def __init__(self, config: ClusterConfig, on_change=None, clock=None):
        self.config = config
        self.node_id = config.node_id
        self.rqlite = Rqlite(config.rqlite_url, config.username,
                             config.password)
        self.epoch = now_ms()
        self.on_change = on_change
        self.clock = clock or now_ms
        self.members: dict = {}          # node_id -> row (as read)
        self.active: tuple = ()          # live node ids, sorted
        self.leader_node = None          # informational: rqlite's leader
        self._leader_seen = 0.0          # monotonic
        self._write_ok = 0.0
        self._started = time.monotonic()
        self.hosting = False
        self.replica = bool(getattr(config, "replica", False))
        self.routing = False
        # Sealed host (forkmesh_host.vault): heart-beats as 'sealed' and
        # never hosts, so it is never live for placement until unsealed.
        self.paused = False
        self.leaving = False
        self._view_lock = asyncio.Lock()
        self._tasks: list = []
        self._view_ready = asyncio.Event()
        self.stats = {"heartbeats": 0, "heartbeat_failures": 0,
                      "view_changes": 0, "quorum_losses": 0}
        # IDS quarantine view (quarantine.py), or None when no IDS keys are
        # rendered. on_quarantine(True|False) fires when THIS node's own
        # quarantine starts or ends (the host seals itself).
        from . import quarantine as _quarantine
        self.quarantine = _quarantine.from_environ(
            getattr(config, "ids_env", None) or {}, self.node_id,
            list(getattr(config, "peers", None) or {}), self.rqlite)
        self.on_quarantine = None
        self._self_quarantined = False
        self._quarantine_read = 0.0

    # -- queries -------------------------------------------------------
    def owner(self, ns, name, now=None):
        kind = placement(ns, name)
        if kind == "local":
            return self.node_id
        if kind == "rotate":
            return rotation_owner(ns, name, self.active,
                                  rotation_index(self.clock()
                                                 if now is None else now))
        return rendezvous(ns, name, self.active)

    def mesh_url(self, node_id):
        if self.config.strict:
            # Never from the database (audit H3): the static map only.
            return self.config.peers.get(node_id)
        row = self.members.get(node_id)
        return (row or {}).get("mesh_url")

    def accept_rows(self, rows, now=None):
        """{node_id: row} of the rows this node trusts (see trusted_rows)."""
        return trusted_rows(rows, self.config.peers, self.node_id,
                            self.config.strict,
                            self.clock() if now is None else now,
                            self._log_rejected)

    def _log_rejected(self, reason, node):
        self.stats["rejected_rows"] = self.stats.get("rejected_rows", 0) + 1
        key = (reason, node)
        seen = self.__dict__.setdefault("_rejected_logged", {})
        now = time.monotonic()
        if now - seen.get(key, -1e9) > 300:
            seen[key] = now
            print("cluster: ignoring cluster_members row %s (%s)"
                  % (node[:64], reason), flush=True)

    @property
    def quorum(self):
        now = time.monotonic()
        if self.replica:
            return now - self._leader_seen <= QUORUM_LOSS_SECONDS
        return (now - self._leader_seen <= QUORUM_LOSS_SECONDS
                and now - self._write_ok <= QUORUM_LOSS_SECONDS + 5)

    @property
    def read_only(self):
        if self.paused or self.leaving or self.self_quarantined:
            return False
        if time.monotonic() - self._started <= QUORUM_LOSS_SECONDS:
            return False
        return not self.quorum

    def member_states(self):
        now = self.clock()
        return {node: member_state(row, now)
                for node, row in self.members.items()}

    def cron_view(self, now=None):
        """Who runs the per-minute cron now and next (debug snapshot)."""
        now = self.clock() if now is None else now
        index = rotation_index(now)
        ns, name = CRON_COORDINATOR
        return {"minuteMs": CRON_MINUTE_MS, "handoffMs": CRON_HANDOFF_MS,
                "index": index, "nextHandoffAt": next_handoff_ms(now),
                "owner": rotation_owner(ns, name, self.active, index),
                "nextOwner": rotation_owner(ns, name, self.active, index + 1),
                "watchdogOwner": rotation_owner(
                    "ForkMeshCronWatchdog", "scheduled-completion-v1",
                    self.active, index)}

    def snapshot(self):
        return {"node": self.node_id, "rqliteLeader": self.leader_node,
                "quarantine": (self.quarantine.snapshot()
                               if self.quarantine else None),
                "active": list(self.active), "live": list(self.active),
                "hosting": self.hosting, "quorum": self.quorum,
                "readOnly": self.read_only,
                "replica": self.replica, "routing": self.routing,
                "members": self.members,
                "memberStatus": self.member_states(),
                "cron": self.cron_view(), "stats": self.stats}

    async def readiness(self):
        reasons = []
        try:
            if not await asyncio.wait_for(self.rqlite.leader(), 2.0):
                reasons.append("rqlite_no_leader")
        except Exception as error:
            reasons.append("rqlite_unreachable:%s" % type(error).__name__)
        if self.replica:
            if not self.active:
                reasons.append("no_live_core")
            if not self.routing and not self.paused:
                reasons.append("not_routing")
        else:
            if self.node_id not in self.active:
                reasons.append("member_not_live")
            if not self.hosting:
                reasons.append("not_hosting")
        if self.paused:
            reasons.append("sealed")
        if self.self_quarantined:
            reasons.insert(0, "quarantined")
        return not reasons, reasons

    @property
    def self_quarantined(self):
        return self.quarantine is not None and \
            self.quarantine.self_quarantined

    def quarantined(self):
        return self.quarantine.quarantined() if self.quarantine else set()

    def source_quarantined(self, client_ip):
        """True when a mesh peer at ``client_ip`` (its WireGuard address in
        the static map) is quarantined: its requests are refused."""
        if self.quarantine is None or not client_ip:
            return False
        blocked = self.quarantine.quarantined()
        for node in blocked:
            url = self.config.peers.get(node) or ""
            if url and urllib.parse.urlsplit(url).hostname == client_ip:
                return True
        return False

    # -- loops ---------------------------------------------------------
    def start(self):
        loop = asyncio.get_running_loop()
        self._tasks = [loop.create_task(self._heartbeat_loop()),
                       loop.create_task(self._view_loop())]

    async def stop(self):
        for task in self._tasks:
            task.cancel()
        for task in self._tasks:
            with contextlib.suppress(BaseException):
                await task
        await self.rqlite.aclose()

    async def wait_view(self, timeout):
        with contextlib.suppress(asyncio.TimeoutError):
            await asyncio.wait_for(self._view_ready.wait(), timeout)

    async def heartbeat(self):
        """Upsert this node's own row -- the only row it ever writes.
        Sealed (``paused``) it reports ``sealed``: still a known member (a
        peer may hand it the vault key, vault.py) but never live for
        placement; unsealed it reports ``active``."""
        if self.replica:
            return
        status = "sealed" if self.paused else "active"
        if self.leaving:
            status = "leaving"
        if self.self_quarantined:
            status = "quarantined"
        await self.rqlite.execute([[
            "INSERT INTO cluster_members(node_id, mesh_url, status, "
            "heartbeat_ms, epoch) VALUES(?, ?, ?, ?, ?) "
            "ON CONFLICT(node_id) DO UPDATE SET mesh_url=excluded.mesh_url, "
            "heartbeat_ms=excluded.heartbeat_ms, epoch=excluded.epoch, "
            "status=excluded.status",
            self.node_id, self.config.mesh_url, status, now_ms(),
            self.epoch]])

    async def beat(self):
        await self.heartbeat()
        self._write_ok = time.monotonic()
        self.stats["heartbeats"] += 1

    async def _heartbeat_loop(self):
        while True:
            try:
                await self.beat()
            except Exception as error:
                self.stats["heartbeat_failures"] += 1
                print("cluster heartbeat failed: %r" % (error,), flush=True)
            await asyncio.sleep(HEARTBEAT_SECONDS)

    async def _read_status(self):
        leader = await self.rqlite.leader()
        if leader:
            self._leader_seen = time.monotonic()
            self.leader_node = self.config.rqlite_node_map.get(leader, leader)
        return leader

    async def read_members(self):
        """The member rows as the rqlite leader has them (``weak``), so
        every node computes liveness from the same inputs."""
        return await self.rqlite.query(
            "SELECT node_id, mesh_url, status, heartbeat_ms, epoch "
            "FROM cluster_members", level="weak")

    async def _refresh_quarantine(self):
        if self.quarantine is None:
            return
        if time.monotonic() - self._quarantine_read >= \
                _QUARANTINE_REFRESH_SECONDS:
            self._quarantine_read = time.monotonic()
            await self.quarantine.refresh()
        flag = self.quarantine.self_quarantined
        if flag != self._self_quarantined:
            self._self_quarantined = flag
            if self.on_quarantine is not None:
                try:
                    await self.on_quarantine(flag)
                except Exception as error:
                    print("cluster quarantine handler failed: %r"
                          % (type(error).__name__,), flush=True)

    async def wait_hosting(self, timeout):
        if time.monotonic() - self._started > JOIN_WINDOW_SECONDS:
            return self.hosting
        deadline = time.monotonic() + timeout
        while not self.hosting and not self.paused and not self.leaving \
                and time.monotonic() < deadline:
            await asyncio.sleep(0.05)
        return self.hosting

    async def leave(self):
        self.leaving = True
        await self.beat()
        await self.refresh()

    async def refresh(self):
        async with self._view_lock:
            await self._refresh_view()

    async def _view_loop(self):
        while True:
            await self.refresh()
            await asyncio.sleep(VIEW_SECONDS)

    async def _refresh_view(self):
        try:
            await self._read_status()
            rows = await self.read_members()
            self.members = self.accept_rows(rows)
        except Exception as error:
            if time.monotonic() - getattr(self, "_view_err_at", 0) > 10:
                self._view_err_at = time.monotonic()
                print("cluster view refresh failed: %r" % (error,),
                      flush=True)
        await self._refresh_quarantine()
        excluded = set(self.quarantined())
        if self.replica:
            excluded.add(self.node_id)
        active = live_members(self.members.values(), self.clock(),
                              exclude=excluded)
        routing = (self.quorum and not self.paused
                   and not self.self_quarantined)
        self.routing = routing
        hosting = routing and not self.replica
        if hosting != self.hosting and not hosting:
            self.stats["quorum_losses"] += 1
        changed = (active != self.active or hosting != self.hosting)
        self.active = active
        self.hosting = hosting
        if self.members or not hosting:
            self._view_ready.set()
        if changed and self.on_change is not None:
            self.stats["view_changes"] += 1
            try:
                await self.on_change()
            except Exception as error:
                print("cluster view change handler failed: %r"
                      % (error,), flush=True)


# -- mesh paths ---------------------------------------------------------------

def do_path(ns, name, url_path):
    return "/_do/%s/%s/%s" % (
        urllib.parse.quote(ns, safe=""), urllib.parse.quote(name, safe=""),
        url_path.lstrip("/"))


def parse_mesh_path(raw_path):
    """``/_do/<ns>/<name>/<rest>`` -> (kind, ns, name, rest)."""
    parts = raw_path.split("?", 1)[0].split("/", 4)
    if len(parts) < 4 or parts[0] != "" or parts[1] not in ("_do", "_rpc"):
        return None
    ns = urllib.parse.unquote(parts[2])
    name = urllib.parse.unquote(parts[3])
    rest = parts[4] if len(parts) > 4 else ""
    return parts[1], ns, name, rest


def forward_headers(headers):
    """Request headers worth carrying across the mesh."""
    return [(name, value) for name, value in headers.raw()
            if name.lower() not in _MESH_STRIP]
