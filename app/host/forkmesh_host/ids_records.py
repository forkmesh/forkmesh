"""Signed intrusion-detection records and the quarantine they imply.

Shared, unchanged, by three programs (docs/IDS.md):

* the root IDS on every node (``deploy/core/ids.py``; the renderer installs
  this file next to it as ``/opt/forkmesh-next/bin/ids_records.py``), which
  signs its own ``self`` quarantine and its ``vote`` rows;
* the app host (``forkmesh_host.quarantine``), which derives from the rows
  which nodes are quarantined (no peer unseal, no mesh, no DNS vote, never
  live for placement);
* the operator desktop (``tools/next_ids.py``), which signs ``release`` rows.

Stdlib plus ``cryptography`` (Ed25519) only, no relative imports.

Rows live in the rqlite table ``cluster_quarantine`` (migration 0197). A row
is trusted only through its signature, never through who wrote it: rqlite has
one user for every node, so any node (or anyone holding RQLITE_PASSWORD) can
insert or delete rows. That is why

* a ``self`` row must be signed by the quarantined node's own IDS key (a node
  can only quarantine itself);
* ``vote`` rows (a peer saw the node's IDS heartbeat stop) count only from a
  strict majority of the core nodes, the target excluded, each signed by the
  voter's key: one compromised node cannot quarantine a healthy peer;
* a ``release`` row must be signed by the operator key, which exists only on
  the desktop (``config/ids-operator.key``): a compromised node cannot
  release itself;
* a verifier keeps what it has seen (``merge_sticky``): deleting rows does not
  lift a quarantine; only a valid release does.

Canonical message: ``fmnext-ids-v1\\n<kind>\\n<JSON body, sorted keys, no
spaces>``; signature: Ed25519, base64url without padding.
"""

from __future__ import annotations

import base64
import json
import os
import re

DOMAIN = "fmnext-ids-v1"
KINDS = ("self", "vote", "release")
OPERATOR = "operator"
NODE_RE = re.compile(r"^[a-z][a-z0-9-]{0,31}$")
ID_RE = re.compile(r"^[A-Za-z0-9_-]{8,64}$")
PUB_RE = re.compile(r"^[A-Za-z0-9_-]{43}$")
SIG_RE = re.compile(r"^[A-Za-z0-9_-]{86}$")
MAX_TEXT = 300
MAX_CLEARS = 256


class RecordError(ValueError):
    pass


def b64u(data: bytes) -> str:
    return base64.urlsafe_b64encode(data).decode("ascii").rstrip("=")


def unb64u(text) -> bytes:
    text = str(text or "")
    return base64.urlsafe_b64decode(text + "=" * (-len(text) % 4))


def new_id() -> str:
    return b64u(os.urandom(12))


def canonical(kind, body) -> bytes:
    if kind not in KINDS:
        raise RecordError("unknown record kind %r" % (kind,))
    return ("%s\n%s\n%s" % (DOMAIN, kind, json.dumps(
        body, sort_keys=True, separators=(",", ":"),
        ensure_ascii=True))).encode("ascii")


# -- keys -------------------------------------------------------------------

def generate_private() -> bytes:
    from cryptography.hazmat.primitives.asymmetric.ed25519 import \
        Ed25519PrivateKey
    from cryptography.hazmat.primitives import serialization
    return Ed25519PrivateKey.generate().private_bytes(
        serialization.Encoding.Raw, serialization.PrivateFormat.Raw,
        serialization.NoEncryption())


def public_key(private_raw: bytes) -> str:
    from cryptography.hazmat.primitives.asymmetric.ed25519 import \
        Ed25519PrivateKey
    from cryptography.hazmat.primitives import serialization
    key = Ed25519PrivateKey.from_private_bytes(bytes(private_raw))
    return b64u(key.public_key().public_bytes(
        serialization.Encoding.Raw, serialization.PublicFormat.Raw))


def load_private(path) -> bytes:
    """A key file holds the 32-byte raw seed, base64url, one line."""
    with open(path, "r", encoding="ascii") as handle:
        raw = unb64u(handle.read().strip())
    if len(raw) != 32:
        raise RecordError("%s is not an Ed25519 seed" % path)
    return raw


def sign(private_raw: bytes, kind, body) -> str:
    from cryptography.hazmat.primitives.asymmetric.ed25519 import \
        Ed25519PrivateKey
    key = Ed25519PrivateKey.from_private_bytes(bytes(private_raw))
    return b64u(key.sign(canonical(kind, body)))


def verify(public_b64, kind, body, signature) -> bool:
    if not PUB_RE.fullmatch(str(public_b64 or "")) or \
            not SIG_RE.fullmatch(str(signature or "")):
        return False
    try:
        from cryptography.exceptions import InvalidSignature
        from cryptography.hazmat.primitives.asymmetric.ed25519 import \
            Ed25519PublicKey
        key = Ed25519PublicKey.from_public_bytes(unb64u(public_b64))
        key.verify(unb64u(signature), canonical(kind, body))
        return True
    except (InvalidSignature, ValueError, RecordError):
        return False


def parse_pubkeys(text):
    """``n1=<b64u>,n2=...`` -> {node: public key}; malformed entries dropped."""
    keys = {}
    for item in str(text or "").split(","):
        name, sep, key = item.partition("=")
        name, key = name.strip(), key.strip()
        if sep and NODE_RE.fullmatch(name) and PUB_RE.fullmatch(key):
            keys[name] = key
    return keys


# -- record bodies ------------------------------------------------------------

def _text(value):
    return "".join(c for c in str(value or "") if c.isprintable())[:MAX_TEXT]


def self_body(node, ts, incident, reason):
    return {"node": node, "ts": int(ts), "incident": _text(incident),
            "reason": _text(reason)}


def vote_body(voter, target, ts, reason):
    return {"voter": voter, "target": target, "ts": int(ts),
            "reason": _text(reason)}


def release_body(node, ts, clears, nonce, note=""):
    return {"node": node, "ts": int(ts),
            "clears": sorted(str(c) for c in clears)[:MAX_CLEARS],
            "nonce": str(nonce), "note": _text(note)}


def make_row(kind, node_id, signer, body, signature, record_id=None):
    """A ``cluster_quarantine`` row (the table's column names)."""
    return {"id": record_id or new_id(), "node_id": node_id, "kind": kind,
            "signer": signer, "created_ms": int(body["ts"]),
            "body": json.dumps(body, sort_keys=True, separators=(",", ":")),
            "sig": signature}


INSERT_SQL = ("INSERT OR IGNORE INTO cluster_quarantine(id, node_id, kind, "
              "signer, created_ms, body, sig) VALUES(?, ?, ?, ?, ?, ?, ?)")
SELECT_SQL = ("SELECT id, node_id, kind, signer, created_ms, body, sig, "
              "reunsealed_ms FROM cluster_quarantine ORDER BY created_ms "
              "LIMIT 2000")


def insert_statement(row):
    return [INSERT_SQL, row["id"], row["node_id"], row["kind"],
            row["signer"], row["created_ms"], row["body"], row["sig"]]


# -- verification -------------------------------------------------------------

def verified(row, node_keys, operator_key):
    """(kind, body) of a row whose signature and fields check out, else None."""
    try:
        kind = str(row.get("kind") or "")
        node_id = str(row.get("node_id") or "")
        signer = str(row.get("signer") or "")
        body = json.loads(row.get("body") or "")
        rid = str(row.get("id") or "")
    except (ValueError, TypeError, AttributeError):
        return None
    if kind not in KINDS or not isinstance(body, dict) or \
            not ID_RE.fullmatch(rid) or not NODE_RE.fullmatch(node_id):
        return None
    if not isinstance(body.get("ts"), int) or body["ts"] <= 0:
        return None
    if kind == "self":
        if body.get("node") != node_id or signer != node_id:
            return None
        key = node_keys.get(node_id)
    elif kind == "vote":
        voter = body.get("voter")
        if body.get("target") != node_id or signer != voter or \
                voter == node_id or voter not in node_keys:
            return None
        key = node_keys.get(voter)
    else:
        if signer != OPERATOR or body.get("node") != node_id or \
                not isinstance(body.get("clears"), list):
            return None
        key = operator_key
    if not key or not verify(key, kind, body, row.get("sig")):
        return None
    return kind, body


def majority(nodes) -> int:
    return len(set(nodes)) // 2 + 1


def quarantine_state(rows, node_keys, operator_key, nodes=None):
    """{node: state} from ``cluster_quarantine`` rows, verified.

    ``state``: ``quarantined`` (bool), ``evidence`` ({row id: ts} of the
    records that quarantine it), ``reasons``, ``release`` (latest valid
    release: id, ts, reunsealed_ms) and ``invalid`` (rows ignored).
    ``nodes``: every core node (the vote majority is over them)."""
    nodes = sorted(set(nodes or node_keys))
    need = majority(nodes)
    out = {}
    releases = {}
    records = []
    for row in rows or ():
        checked = verified(row, node_keys, operator_key)
        node = str((row or {}).get("node_id") or "")
        if checked is None:
            if node:
                out.setdefault(node, _empty())["invalid"] += 1
            continue
        kind, body = checked
        if kind == "release":
            best = releases.get(node)
            if best is None or body["ts"] > best["ts"]:
                releases[node] = {"id": row["id"], "ts": body["ts"],
                                  "clears": set(body["clears"]),
                                  "reunsealed_ms": row.get("reunsealed_ms")}
        else:
            records.append((node, kind, body, str(row["id"])))
    for node, release in releases.items():
        out.setdefault(node, _empty())["release"] = {
            "id": release["id"], "ts": release["ts"],
            "clears": sorted(release["clears"]),
            "reunsealed_ms": release["reunsealed_ms"]}
    votes = {}
    for node, kind, body, rid in records:
        release = releases.get(node)
        if release and (body["ts"] <= release["ts"] or rid in release["clears"]):
            continue
        state = out.setdefault(node, _empty())
        if kind == "self":
            state["quarantined"] = True
            state["evidence"][rid] = body["ts"]
            state["reasons"].append("self: %s" % body.get("reason", ""))
        else:
            if body["voter"] in nodes:
                votes.setdefault(node, {})[body["voter"]] = (rid, body)
    for node, by_voter in votes.items():
        if len(by_voter) >= need:
            state = out.setdefault(node, _empty())
            state["quarantined"] = True
            for rid, body in by_voter.values():
                state["evidence"][rid] = body["ts"]
            state["reasons"].append("votes: %s" % ",".join(sorted(by_voter)))
    return out


def _empty():
    return {"quarantined": False, "evidence": {}, "reasons": [],
            "release": None, "invalid": 0}


def merge_sticky(state, sticky):
    """Keep quarantines whose rows have vanished until a valid release covers
    them. ``sticky``: {node: {row id: ts}} remembered by this verifier;
    returns (state, new sticky). A release covers a record when the record
    is not newer than the release or the release names it in ``clears``."""
    new_sticky = {}
    for node, evidence in (sticky or {}).items():
        current = state.setdefault(node, _empty())
        release = current.get("release")
        clears = set((release or {}).get("clears") or ())
        left = {rid: int(ts) for rid, ts in (evidence or {}).items()
                if not (release and (int(ts) <= release["ts"]
                                     or rid in clears))}
        if left and not current["quarantined"]:
            current["quarantined"] = True
            current["reasons"].append("sticky: records removed without a "
                                      "release")
            current["evidence"].update(left)
        if left:
            new_sticky[node] = left
    for node, current in state.items():
        if current["quarantined"]:
            merged = dict(new_sticky.get(node) or {})
            merged.update(current["evidence"])
            new_sticky[node] = merged
    return state, new_sticky


def quarantined_nodes(state):
    return {node for node, s in state.items() if s.get("quarantined")}


def peer_unseal_blocked(state, node):
    """True when peers must not hand ``node`` the vault key: it is
    quarantined, or it was released but the operator has not re-unsealed it
    from the desktop yet (docs/IDS.md "Release")."""
    current = (state or {}).get(node)
    if not current:
        return False
    if current.get("quarantined"):
        return True
    release = current.get("release")
    return bool(release) and not release.get("reunsealed_ms")
