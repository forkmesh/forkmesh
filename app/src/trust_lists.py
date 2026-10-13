"""Scoped, community-operated trust lists.

This module is the dependency-free reference implementation of
``forkmesh-trust-list-v1`` (www/docs/design/scoped-trust-lists.md).  A client
hands it raw signed documents -- list versions, revocations, and appeals --
plus its *local* follows and overrides, and receives one deterministic
resolution.  The JavaScript (app/public/trust-lists.js), Swift
(native/ios/ForkMeshCore/Sources/FMAccount/TrustLists.swift), Kotlin
(native/android/core/src/main/kotlin/com/forkmesh/nativeapp/security/TrustLists.kt),
and desktop C++ (desktop/src/TrustLists.cpp) implementations must produce byte-identical
resolutions for every case in conformance/trust-lists/v1/vectors.json.

Rules that matter for safety:

* nothing is trusted unless the user follows a list, and trust never
  extends past the role and repository/organization/community scope it
  was issued for -- there is no global scope;
* equivocation, a forked version history, or a revocation cuts the list
  back to its last good version and says so (``recovered``) instead of
  quietly following the newest document;
* disagreeing lists produce a visible ``conflict``; only the user's local
  override picks a side;
* revocations are monotonic, so no later document can undo one.
"""

from __future__ import annotations

import base64
import hashlib
import json
import re
from typing import Any, Iterable, Mapping, Sequence

from collaboration import _GROUP_L, ed25519_verify


LIST_SCHEMA = "forkmesh-trust-list-v1"
REVOCATION_SCHEMA = "forkmesh-trust-list-revocation-v1"
APPEAL_SCHEMA = "forkmesh-trust-list-appeal-v1"
RESOLUTION_SCHEMA = "forkmesh-trust-resolution-v1"
LIST_ID_DOMAIN = "forkmesh-trust-list-id-v1"

ROLES = (
    "maintainer",
    "moderator",
    "mirror-operator",
    "organization",
    "reviewer",
    "security-researcher",
)
STANCES = ("distrust", "trust")
OVERRIDE_STANCES = ("distrust", "ignore", "trust")
REVOCATION_REASONS = ("compromised", "malicious", "retired", "superseded")
LIST_STATES = ("active", "conflicted", "expired", "missing", "recovered", "revoked")

SAFE_INTEGER = 2**53 - 1
MAX_DOCUMENT_BYTES = 262_144
MAX_ENTRIES = 2000
MAX_EDGES = 16
MAX_COMMUNITIES = 64
MAX_REVOCATION_KEYS = 4
MAX_NOTE = 280
MAX_STATEMENT = 1000
MAX_LIFETIME_MS = 400 * 24 * 60 * 60 * 1000
MAX_PATH_LISTS = 4
FUTURE_SKEW_MS = 300_000
MAX_NESTING = 8

_LIST_KEYS = frozenset(
    {
        "schema", "issuer", "name", "version", "previous", "issuedAt",
        "expiresAt", "roles", "communities", "revocationKeys", "entries",
        "edges", "signature",
    }
)
_REVOCATION_KEYS = frozenset(
    {
        "schema", "list", "issuer", "name", "fromVersion", "revokedBy",
        "revokedAt", "reason", "successor", "signature",
    }
)
_APPEAL_KEYS = frozenset(
    {
        "schema", "subject", "list", "version", "role", "scope", "statement",
        "issuedAt", "signature",
    }
)
_ENTRY_KEYS = frozenset({"subject", "role", "scope", "stance", "note"})
_EDGE_KEYS = frozenset({"kind", "issuer", "name", "roles", "communities", "expiresAt"})
_SUCCESSOR_KEYS = frozenset({"issuer", "name"})

_TEXT_RE = re.compile(r'^[\x20-\x21\x23-\x5b\x5d-\x7e]*$')
_KEY_TEXT_RE = re.compile(r"^[A-Za-z0-9_-]{43}$")
_SIG_TEXT_RE = re.compile(r"^[A-Za-z0-9_-]{86}$")
_SLUG_RE = re.compile(r"^[a-z0-9][a-z0-9-]{0,63}$")
_OWNER_RE = re.compile(r"^[a-z0-9][a-z0-9-]{0,38}$")
_REPO_RE = re.compile(r"^[A-Za-z0-9._-]{1,100}$")
_DIGEST_RE = re.compile(r"^sha256:[0-9a-f]{64}$")
_LIST_URN_RE = re.compile(r"^urn:forkmesh:trust-list:sha256:[0-9a-f]{64}$")


class TrustListError(ValueError):
    """A document failed verification; ``code`` is the stable reason."""

    def __init__(self, code: str) -> None:
        super().__init__(code)
        self.code = code


def _reject(code: str) -> None:
    raise TrustListError(code)


# ---------------------------------------------------------------------------
# Canonical encoding.  Strings are restricted to printable ASCII without
# quote or backslash, so the encoding needs no escapes and every language
# produces identical bytes with a plain sorted-key writer.


def canonical_json(value: Any) -> str:
    if value is None:
        return "null"
    if value is True:
        return "true"
    if value is False:
        return "false"
    if isinstance(value, int):
        if abs(value) > SAFE_INTEGER:
            _reject("noncanonical")
        return str(value)
    if isinstance(value, str):
        if not _TEXT_RE.fullmatch(value):
            _reject("noncanonical")
        return '"' + value + '"'
    if isinstance(value, list):
        return "[" + ",".join(canonical_json(item) for item in value) + "]"
    if isinstance(value, dict):
        parts = []
        for key in sorted(value):
            if not isinstance(key, str) or not _TEXT_RE.fullmatch(key):
                _reject("noncanonical")
            parts.append('"' + key + '":' + canonical_json(value[key]))
        return "{" + ",".join(parts) + "}"
    _reject("noncanonical")
    return ""


def _sha256_hex(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def document_id(raw: str) -> str:
    return "sha256:" + hashlib.sha256(raw.encode("utf-8", "surrogatepass")).hexdigest()


def list_id(issuer: str, name: str) -> str:
    return "urn:forkmesh:trust-list:sha256:" + _sha256_hex(
        LIST_ID_DOMAIN + "\n" + issuer + "\n" + name
    )


def signing_text(document: Mapping[str, Any]) -> str:
    unsigned = {key: value for key, value in document.items() if key != "signature"}
    return str(document["schema"]) + "\n" + canonical_json(unsigned)


# ---------------------------------------------------------------------------
# Scopes.


def valid_scope(value: Any) -> bool:
    if not isinstance(value, str):
        return False
    if value.startswith("repo:"):
        owner, slash, repo = value[5:].partition("/")
        return (
            bool(slash)
            and bool(_OWNER_RE.fullmatch(owner))
            and bool(_REPO_RE.fullmatch(repo))
            and repo not in (".", "..")
        )
    if value.startswith("org:"):
        return bool(_OWNER_RE.fullmatch(value[4:]))
    if value.startswith("community:"):
        return bool(_SLUG_RE.fullmatch(value[10:]))
    return False


def covers(outer: str, inner: str) -> bool:
    """True when trust in ``outer`` includes ``inner``."""

    if outer == inner:
        return True
    return outer.startswith("org:") and inner.startswith("repo:" + outer[4:] + "/")


def _covered(scopes: Iterable[str], scope: str) -> bool:
    return any(covers(outer, scope) for outer in scopes)


def _normalize_scopes(scopes: Iterable[str]) -> list[str]:
    unique = sorted(set(scopes))
    return [s for s in unique if not any(o != s and covers(o, s) for o in unique)]


def intersect_scopes(left: Sequence[str], right: Sequence[str]) -> list[str]:
    kept = [a for a in left if _covered(right, a)]
    kept += [b for b in right if _covered(left, b)]
    return _normalize_scopes(kept)


# ---------------------------------------------------------------------------
# Field checks.


def _safe_int(value: Any, minimum: int = 0) -> bool:
    return (
        isinstance(value, int)
        and not isinstance(value, bool)
        and minimum <= value <= SAFE_INTEGER
    )


def _key_bytes(value: Any) -> bytes | None:
    if not isinstance(value, str) or not _KEY_TEXT_RE.fullmatch(value):
        return None
    decoded = base64.urlsafe_b64decode(value + "=")
    if base64.urlsafe_b64encode(decoded).decode("ascii").rstrip("=") != value:
        return None
    return decoded


def _signature_bytes(value: Any) -> bytes | None:
    if not isinstance(value, str) or not _SIG_TEXT_RE.fullmatch(value):
        return None
    decoded = base64.urlsafe_b64decode(value + "==")
    if base64.urlsafe_b64encode(decoded).decode("ascii").rstrip("=") != value:
        return None
    return decoded


def _sorted_unique(values: Any, check, *, maximum: int, empty: bool) -> bool:
    if not isinstance(values, list) or len(values) > maximum:
        return False
    if not values and not empty:
        return False
    if not all(check(item) for item in values):
        return False
    return all(values[i] < values[i + 1] for i in range(len(values) - 1))


def _valid_roles(values: Any) -> bool:
    return _sorted_unique(
        values, lambda r: isinstance(r, str) and r in ROLES, maximum=len(ROLES), empty=False
    )


def _valid_communities(values: Any) -> bool:
    return (
        _sorted_unique(values, valid_scope, maximum=MAX_COMMUNITIES, empty=False)
        and _normalize_scopes(values) == values
    )


def _verify_signature(document: Mapping[str, Any], signer: str) -> None:
    signature = _signature_bytes(document["signature"])
    if signature is None:
        _reject("invalid_signature_encoding")
    if int.from_bytes(signature[32:], "little") >= _GROUP_L:
        _reject("invalid_signature")
    public = _key_bytes(signer)
    message = signing_text(document).encode("ascii")
    if public is None or not ed25519_verify(public, signature, message):
        _reject("invalid_signature")


def _validate_list(doc: Mapping[str, Any]) -> None:
    if _key_bytes(doc["issuer"]) is None:
        _reject("invalid_issuer")
    if not isinstance(doc["name"], str) or not _SLUG_RE.fullmatch(doc["name"]):
        _reject("invalid_name")
    if not _safe_int(doc["version"], 1):
        _reject("invalid_version")
    previous = doc["previous"]
    if doc["version"] == 1:
        if previous is not None:
            _reject("invalid_previous")
    elif not isinstance(previous, str) or not _DIGEST_RE.fullmatch(previous):
        _reject("invalid_previous")
    issued, expires = doc["issuedAt"], doc["expiresAt"]
    if (
        not _safe_int(issued)
        or not _safe_int(expires)
        or expires <= issued
        or expires - issued > MAX_LIFETIME_MS
    ):
        _reject("invalid_time")
    if not _valid_roles(doc["roles"]) or not _valid_communities(doc["communities"]):
        _reject("invalid_scope")
    keys = doc["revocationKeys"]
    if not _sorted_unique(
        keys, lambda k: _key_bytes(k) is not None, maximum=MAX_REVOCATION_KEYS, empty=True
    ) or doc["issuer"] in keys:
        _reject("invalid_revocation_keys")

    entries = doc["entries"]
    if not isinstance(entries, list) or len(entries) > MAX_ENTRIES:
        _reject("invalid_entry")
    previous_key = None
    for entry in entries:
        if (
            not isinstance(entry, dict)
            or set(entry) != _ENTRY_KEYS
            or _key_bytes(entry["subject"]) is None
            or entry["subject"] == doc["issuer"]
            or entry["role"] not in ROLES
            or not valid_scope(entry["scope"])
            or entry["stance"] not in STANCES
            or not isinstance(entry["note"], str)
            or len(entry["note"]) > MAX_NOTE
        ):
            _reject("invalid_entry")
        if entry["role"] not in doc["roles"] or not _covered(doc["communities"], entry["scope"]):
            _reject("entry_outside_scope")
        key = entry["subject"] + "\n" + entry["role"] + "\n" + entry["scope"]
        if previous_key is not None and key <= previous_key:
            _reject("unsorted_entries")
        previous_key = key

    edges = doc["edges"]
    if not isinstance(edges, list) or len(edges) > MAX_EDGES:
        _reject("invalid_edge")
    previous_key = None
    for edge in edges:
        if (
            not isinstance(edge, dict)
            or set(edge) != _EDGE_KEYS
            or edge["kind"] not in ("delegate", "include")
            or _key_bytes(edge["issuer"]) is None
            or not isinstance(edge["name"], str)
            or not _SLUG_RE.fullmatch(edge["name"])
            or not _valid_roles(edge["roles"])
            or not _valid_communities(edge["communities"])
            or (edge["kind"] == "delegate" and not _safe_int(edge["expiresAt"]))
            or (edge["kind"] == "include" and edge["expiresAt"] is not None)
            or (edge["issuer"] == doc["issuer"] and edge["name"] == doc["name"])
        ):
            _reject("invalid_edge")
        if not set(edge["roles"]) <= set(doc["roles"]) or not all(
            _covered(doc["communities"], scope) for scope in edge["communities"]
        ):
            _reject("edge_outside_scope")
        key = edge["issuer"] + "\n" + edge["name"]
        if previous_key is not None and key <= previous_key:
            _reject("unsorted_edges")
        previous_key = key

    _verify_signature(doc, doc["issuer"])


def _validate_revocation(doc: Mapping[str, Any]) -> None:
    if _key_bytes(doc["issuer"]) is None:
        _reject("invalid_issuer")
    if not isinstance(doc["name"], str) or not _SLUG_RE.fullmatch(doc["name"]):
        _reject("invalid_name")
    if doc["list"] != list_id(doc["issuer"], doc["name"]):
        _reject("list_mismatch")
    if not _safe_int(doc["fromVersion"], 1):
        _reject("invalid_version")
    if _key_bytes(doc["revokedBy"]) is None:
        _reject("invalid_revoker")
    if not _safe_int(doc["revokedAt"]):
        _reject("invalid_time")
    if doc["reason"] not in REVOCATION_REASONS:
        _reject("invalid_reason")
    successor = doc["successor"]
    if successor is not None and (
        not isinstance(successor, dict)
        or set(successor) != _SUCCESSOR_KEYS
        or _key_bytes(successor["issuer"]) is None
        or not isinstance(successor["name"], str)
        or not _SLUG_RE.fullmatch(successor["name"])
        or (successor["issuer"] == doc["issuer"] and successor["name"] == doc["name"])
    ):
        _reject("invalid_successor")
    _verify_signature(doc, doc["revokedBy"])


def _validate_appeal(doc: Mapping[str, Any]) -> None:
    if _key_bytes(doc["subject"]) is None:
        _reject("invalid_subject")
    if not isinstance(doc["list"], str) or not _LIST_URN_RE.fullmatch(doc["list"]):
        _reject("invalid_list")
    if not _safe_int(doc["version"], 1):
        _reject("invalid_version")
    if doc["role"] not in ROLES or not valid_scope(doc["scope"]):
        _reject("invalid_scope")
    statement = doc["statement"]
    if not isinstance(statement, str) or not 1 <= len(statement) <= MAX_STATEMENT:
        _reject("invalid_statement")
    if not _safe_int(doc["issuedAt"]):
        _reject("invalid_time")
    _verify_signature(doc, doc["subject"])


_SCHEMAS = {
    LIST_SCHEMA: (_LIST_KEYS, _validate_list),
    REVOCATION_SCHEMA: (_REVOCATION_KEYS, _validate_revocation),
    APPEAL_SCHEMA: (_APPEAL_KEYS, _validate_appeal),
}


def _nesting_depth(raw: str) -> int:
    """Deepest bracket nesting outside strings, found before any parser runs."""

    depth = deepest = 0
    in_string = escaped = False
    for char in raw:
        if in_string:
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == '"':
                in_string = False
        elif char == '"':
            in_string = True
        elif char in "[{":
            depth += 1
            deepest = max(deepest, depth)
        elif char in "]}":
            depth -= 1
    return deepest


def _no_constant(name: str) -> None:
    raise ValueError(name)


def verify_document(raw: str) -> dict[str, Any]:
    """Parse and verify one raw document, raising ``TrustListError``."""

    if not isinstance(raw, str) or len(raw.encode("utf-8", "surrogatepass")) > MAX_DOCUMENT_BYTES:
        _reject("too_large")
    if _nesting_depth(raw) > MAX_NESTING:
        _reject("invalid_json")
    try:
        document = json.loads(raw, parse_constant=_no_constant)
    except (ValueError, RecursionError):
        _reject("invalid_json")
    if canonical_json(document) != raw:
        _reject("noncanonical")
    if not isinstance(document, dict):
        _reject("unsupported_schema")
    schema = document.get("schema")
    if not isinstance(schema, str) or schema not in _SCHEMAS:
        _reject("unsupported_schema")
    keys, validate = _SCHEMAS[schema]
    if set(document) != keys:
        _reject("invalid_keys")
    validate(document)
    return document


# ---------------------------------------------------------------------------
# Resolution.


def _join(*parts: str) -> str:
    return "\n".join(parts)


def _list_group_state(docs: list[tuple[str, dict]], revocations: list[tuple[str, dict]], now: int):
    by_version: dict[int, list[tuple[str, dict]]] = {}
    for doc_id, doc in docs:
        by_version.setdefault(doc["version"], []).append((doc_id, doc))
    cutoffs: list[int] = []
    reasons: set[str] = set()
    conflicts: list[dict[str, Any]] = []
    listed = docs[0][1] if docs else None
    lid = list_id(listed["issuer"], listed["name"]) if listed else None
    for version in sorted(by_version):
        if len(by_version[version]) > 1:
            cutoffs.append(version)
            reasons.add("equivocation")
            conflicts.append({"kind": "equivocation", "listId": lid, "version": version})
            continue
        prior = by_version.get(version - 1)
        if prior is not None and len(prior) == 1:
            if by_version[version][0][1]["previous"] != prior[0][0]:
                cutoffs.append(version)
                reasons.add("history_fork")
                conflicts.append({"kind": "history_fork", "listId": lid, "version": version})
    successor = None
    if revocations:
        reasons.add("revoked")
        ordered = sorted(revocations, key=lambda item: (item[1]["fromVersion"], item[0]))
        cutoffs.append(ordered[0][1]["fromVersion"])
        for _doc_id, revocation in ordered:
            if revocation["successor"] is not None:
                successor = dict(revocation["successor"])
                break
    cutoff = min(cutoffs) if cutoffs else None
    head_id, head = None, None
    for version in sorted(by_version, reverse=True):
        if cutoff is not None and version >= cutoff:
            continue
        if len(by_version[version]) == 1:
            head_id, head = by_version[version][0]
            break
    if head is None:
        state = "revoked" if revocations else ("conflicted" if docs else "missing")
    elif now >= head["expiresAt"]:
        state = "expired"
    elif cutoff is not None:
        state = "recovered"
    else:
        state = "active"
    return {
        "state": state,
        "head": head,
        "headId": head_id,
        "cutoff": cutoff,
        "reasons": sorted(reasons),
        "successor": successor,
        "conflicts": conflicts,
        "heldVersions": sorted(by_version),
    }


def resolve(
    documents: Sequence[str],
    *,
    now: int,
    follows: Sequence[Mapping[str, Any]] = (),
    overrides: Sequence[Mapping[str, Any]] = (),
) -> dict[str, Any]:
    """Verify, compose, and merge trust lists for one local client."""

    unique: dict[str, str] = {}
    for raw in documents:
        unique.setdefault(document_id(raw), raw)
    duplicates = len(documents) - len(unique)

    rejected: list[dict[str, str]] = []
    lists: list[tuple[str, dict]] = []
    revocations: list[tuple[str, dict]] = []
    appeals: list[tuple[str, dict]] = []
    for doc_id in sorted(unique):
        try:
            doc = verify_document(unique[doc_id])
        except TrustListError as error:
            rejected.append({"documentId": doc_id, "code": error.code})
            continue
        schema = doc["schema"]
        if schema != REVOCATION_SCHEMA and doc["issuedAt"] > now + FUTURE_SKEW_MS:
            rejected.append({"documentId": doc_id, "code": "issued_in_future"})
        elif schema == LIST_SCHEMA:
            lists.append((doc_id, doc))
        elif schema == REVOCATION_SCHEMA:
            revocations.append((doc_id, doc))
        else:
            appeals.append((doc_id, doc))

    docs_by_list: dict[str, list[tuple[str, dict]]] = {}
    names: dict[str, tuple[str, str]] = {}
    for doc_id, doc in lists:
        lid = list_id(doc["issuer"], doc["name"])
        docs_by_list.setdefault(lid, []).append((doc_id, doc))
        names[lid] = (doc["issuer"], doc["name"])

    revocations_by_list: dict[str, list[tuple[str, dict]]] = {}
    for doc_id, revocation in revocations:
        lid = revocation["list"]
        authorized = revocation["revokedBy"] == revocation["issuer"] or any(
            doc["version"] < revocation["fromVersion"]
            and revocation["revokedBy"] in doc["revocationKeys"]
            for _id, doc in docs_by_list.get(lid, [])
        )
        if not authorized:
            rejected.append({"documentId": doc_id, "code": "unauthorized_revoker"})
            continue
        revocations_by_list.setdefault(lid, []).append((doc_id, revocation))
        names.setdefault(lid, (revocation["issuer"], revocation["name"]))

    groups: dict[str, dict[str, Any]] = {}

    def group(lid: str) -> dict[str, Any]:
        if lid not in groups:
            groups[lid] = _list_group_state(
                docs_by_list.get(lid, []), revocations_by_list.get(lid, []), now
            )
        return groups[lid]

    for lid in set(docs_by_list) | set(revocations_by_list):
        group(lid)

    def usable(lid: str) -> dict | None:
        info = group(lid)
        return info["head"] if info["state"] in ("active", "recovered") else None

    followed: set[str] = set()
    reached: set[str] = set()
    expired_delegations = depth_limited = cycles = 0
    # listId -> best (path length, joined path, path) per admitted tuple key.
    sources: dict[str, dict[str, dict[str, Any]]] = {}

    def admit(lid: str, path: list[str], roles: list[str], communities: list[str]) -> None:
        nonlocal expired_delegations, depth_limited, cycles
        reached.add(lid)
        head = usable(lid)
        if head is None:
            return
        for entry in head["entries"]:
            if entry["role"] not in roles or not _covered(communities, entry["scope"]):
                continue
            tuple_key = _join(entry["subject"], entry["role"], entry["scope"])
            candidate = {
                "listId": lid,
                "version": head["version"],
                "stance": entry["stance"],
                "note": entry["note"],
                "path": list(path),
            }
            per_list = sources.setdefault(tuple_key, {})
            current = per_list.get(lid)
            rank = (len(path), _join(*path))
            if current is None or rank < (len(current["path"]), _join(*current["path"])):
                per_list[lid] = candidate
        for edge in head["edges"]:
            if edge["kind"] == "delegate" and now >= edge["expiresAt"]:
                expired_delegations += 1
                continue
            child = list_id(edge["issuer"], edge["name"])
            names.setdefault(child, (edge["issuer"], edge["name"]))
            if child in path:
                cycles += 1
                continue
            if len(path) >= MAX_PATH_LISTS:
                depth_limited += 1
                continue
            child_roles = sorted(set(roles) & set(edge["roles"]))
            child_communities = intersect_scopes(communities, edge["communities"])
            child_head = usable(child)
            if child_head is not None:
                child_roles = sorted(set(child_roles) & set(child_head["roles"]))
                child_communities = intersect_scopes(child_communities, child_head["communities"])
            if not child_roles or not child_communities:
                reached.add(child)
                continue
            admit(child, path + [child], child_roles, child_communities)

    follow_roots = []
    for follow in follows:
        lid = list_id(follow["issuer"], follow["name"])
        names.setdefault(lid, (follow["issuer"], follow["name"]))
        follow_roots.append((lid, follow))
    for lid, follow in sorted(follow_roots, key=lambda item: item[0]):
        followed.add(lid)
        head = usable(lid)
        if head is None:
            reached.add(lid)
            continue
        roles = list(head["roles"])
        communities = list(head["communities"])
        if follow.get("roles") is not None:
            roles = sorted(set(roles) & set(follow["roles"]))
        if follow.get("communities") is not None:
            communities = intersect_scopes(communities, follow["communities"])
        if roles and communities:
            admit(lid, [lid], roles, communities)
        else:
            reached.add(lid)

    override_map = {
        _join(o["subject"], o["role"], o["scope"]): o["stance"] for o in overrides
    }
    appeal_status_counts = {"open": 0, "resolved": 0, "unattached": 0}
    assertions = []
    conflicts: list[dict[str, Any]] = []
    for tuple_key in sorted(set(sources) | set(override_map)):
        subject, role, scope = tuple_key.split("\n")
        tuple_sources = [sources.get(tuple_key, {})[k] for k in sorted(sources.get(tuple_key, {}))]
        stances = {source["stance"] for source in tuple_sources}
        stance = "none" if not stances else (stances.pop() if len(stances) == 1 else "conflict")
        override = override_map.get(tuple_key)
        if stance == "conflict":
            conflicts.append(
                {"kind": "stance", "subject": subject, "role": role, "scope": scope,
                 "overridden": override is not None}
            )
        assertions.append(
            {
                "subject": subject,
                "role": role,
                "scope": scope,
                "stance": stance,
                "override": override,
                "effective": override if override is not None else stance,
                "sources": tuple_sources,
                "appeals": [],
            }
        )

    by_tuple = {_join(a["subject"], a["role"], a["scope"]): a for a in assertions}
    for doc_id, appeal in appeals:
        assertion = by_tuple.get(_join(appeal["subject"], appeal["role"], appeal["scope"]))
        if assertion is None or not any(s["listId"] == appeal["list"] for s in assertion["sources"]):
            appeal_status_counts["unattached"] += 1
            continue
        head = usable(appeal["list"])
        status = "open"
        if head is not None and head["version"] > appeal["version"] and not any(
            entry["subject"] == appeal["subject"]
            and entry["role"] == appeal["role"]
            and entry["scope"] == appeal["scope"]
            and entry["stance"] == "distrust"
            for entry in head["entries"]
        ):
            status = "resolved"
        appeal_status_counts[status] += 1
        assertion["appeals"].append(
            {"appealId": doc_id, "listId": appeal["list"], "version": appeal["version"], "status": status}
        )

    list_rows = []
    state_counts = {state: 0 for state in LIST_STATES}
    for lid in sorted(set(groups) | followed | reached):
        info = group(lid)
        conflicts.extend(info["conflicts"])
        issuer, name = names[lid]
        state_counts[info["state"]] += 1
        head = info["head"]
        list_rows.append(
            {
                "listId": lid,
                "issuer": issuer,
                "name": name,
                "state": info["state"],
                "followed": lid in followed,
                "reachable": lid in reached,
                "headVersion": head["version"] if head else None,
                "headDocumentId": info["headId"],
                "expiresAt": head["expiresAt"] if head else None,
                "cutoffVersion": info["cutoff"],
                "cutoffReasons": info["reasons"],
                "successor": info["successor"],
                "heldVersions": info["heldVersions"],
            }
        )

    conflicts.sort(key=canonical_json)
    rejected.sort(key=lambda item: _join(item["documentId"], item["code"]))
    rejection_codes: dict[str, int] = {}
    for item in rejected:
        rejection_codes[item["code"]] = rejection_codes.get(item["code"], 0) + 1

    return {
        "schema": RESOLUTION_SCHEMA,
        "now": now,
        "lists": list_rows,
        "assertions": assertions,
        "conflicts": conflicts,
        "rejected": rejected,
        "telemetry": {
            "documents": len(unique),
            "duplicateDocuments": duplicates,
            "rejectedDocuments": len(rejected),
            "rejectionCodes": rejection_codes,
            "revocations": sum(len(v) for v in revocations_by_list.values()),
            "listStates": state_counts,
            "followedLists": len(followed),
            "reachableLists": len(reached),
            "assertions": len(assertions),
            "stanceConflicts": sum(1 for c in conflicts if c["kind"] == "stance"),
            "overrides": len(override_map),
            "appeals": appeal_status_counts,
            "expiredDelegations": expired_delegations,
            "depthLimitedEdges": depth_limited,
            "cycleEdges": cycles,
        },
    }


def sign_document(unsigned: Mapping[str, Any], signer) -> str:
    """Return the raw canonical document signed by ``signer(message_bytes)``.

    ``signer`` returns a base64url Ed25519 signature; it never receives or
    exposes key material through this module.
    """

    document = dict(unsigned)
    document["signature"] = signer(signing_text(document).encode("ascii"))
    return canonical_json(document)
