"""Normalized remote ActivityPub thread records.

This module is deliberately separate from ForkMesh's native issue, pull, and
discussion event stores.  ActivityPub HTTP signatures authenticate delivery;
they are not ForkMesh Ed25519 event signatures and must never be inserted into
the repository's signed event log.

The helpers are stdlib-only so the Cloudflare Worker and the normal Python test
suite can share the same Lemmy/Mastodon interoperability rules.
"""

import hashlib
import html
import re
from urllib.parse import urlparse


THREAD_SCHEMA = "forkmesh-federated-thread-v1"
STORAGE_NAMESPACE = "federatedReplies"
RESOURCE_KINDS = ("issue", "pull", "discussion")
VISIBLE_STATES = ("active", "edited")
_NOTE_TYPES = ("Note", "Page", "Article")
_DROP_BLOCK_RE = re.compile(
    r"<\s*(script|style)[^>]*>.*?<\s*/\s*\1\s*>",
    re.IGNORECASE | re.DOTALL,
)
_BREAK_RE = re.compile(r"<\s*(?:br\s*/?|/p|/div|/li)\s*>", re.IGNORECASE)
_TAG_RE = re.compile(r"<[^>]{0,500}>")
_WS_RE = re.compile(r"[ \t\r\f\v]+")


def _object_id(value):
    if isinstance(value, str):
        return value
    if isinstance(value, dict):
        raw = value.get("id") or value.get("href")
        return str(raw or "")
    return ""


def _https_url(value):
    value = str(value or "").strip()
    parsed = urlparse(value)
    if parsed.scheme != "https" or not parsed.netloc or parsed.username:
        return ""
    return value


def instance_for_url(value):
    """Return a lowercase ActivityPub instance hostname, never a raw address."""
    parsed = urlparse(_https_url(value))
    return (parsed.hostname or "").lower().rstrip(".")


def normalize_instance(value):
    value = str(value or "").strip().lower().rstrip(".")
    if "://" in value:
        return instance_for_url(value)
    if not value or "/" in value or "@" in value or ":" in value:
        return ""
    return value


def instance_is_blocked(instance, blocked_instances):
    """Match an exact host or a subdomain of a blocked host."""
    host = normalize_instance(instance)
    for raw in blocked_instances or ():
        blocked = normalize_instance(raw)
        if blocked and (host == blocked or host.endswith("." + blocked)):
            return True
    return False


def sanitize_remote_content(value, max_len=4000):
    """Convert untrusted remote HTML to bounded plain text for every client."""
    text = _DROP_BLOCK_RE.sub(" ", str(value or ""))
    text = _BREAK_RE.sub("\n", text)
    text = _TAG_RE.sub("", text)
    text = html.unescape(text)
    text = _WS_RE.sub(" ", text)
    lines = [line.strip() for line in text.split("\n")]
    text = "\n".join(line for line in lines if line).strip()
    if len(text) > max_len:
        return text[:max_len].rstrip() + "…"
    return text


def normalize_context(value):
    """Normalize a local thread context supplied by the verified inbox path."""
    value = value if isinstance(value, dict) else {}
    kind = str(value.get("kind", "") or "").strip().lower()
    if kind in ("pull-request", "pull_request", "pr"):
        kind = "pull"
    if kind not in RESOURCE_KINDS:
        return {}
    owner = str(value.get("owner", "") or "").strip().lower()
    repo = str(value.get("repo", "") or "").strip().lower()
    ref = str(value.get("ref", "") or value.get("number", "") or "").strip()
    if not owner or not repo or not ref:
        return {}
    return {"owner": owner, "repo": repo, "kind": kind, "ref": ref}


def dedupe_key(remote_id):
    remote_id = _https_url(remote_id)
    if not remote_id:
        return ""
    return hashlib.sha256(("activitypub-thread:" + remote_id).encode()).hexdigest()


def _actor_record(remote_actor, fallback_actor):
    actor = remote_actor if isinstance(remote_actor, dict) else {}
    actor_id = _https_url(
        actor.get("id") or actor.get("actor_id") or fallback_actor
    )
    actor_url = _https_url(actor.get("url")) or actor_id
    handle = str(
        actor.get("handle")
        or actor.get("preferredUsername")
        or actor.get("name")
        or actor_id
        or ""
    )
    name = str(actor.get("display_name") or actor.get("name") or "")
    software = str(actor.get("software") or actor.get("platform") or "").lower()
    if "lemmy" in software:
        software = "lemmy"
    elif not software:
        software = "activitypub"
    return {
        "id": actor_id,
        "url": actor_url,
        "handle": handle,
        "name": name,
        "software": software,
        "instance": instance_for_url(actor_id or actor_url),
    }


def _base_record(remote_id, actor, context, received_ms):
    return {
        "schema": THREAD_SCHEMA,
        "storageNamespace": STORAGE_NAMESPACE,
        "remoteId": remote_id,
        "dedupeKey": dedupe_key(remote_id),
        "context": context,
        "parentRemoteId": "",
        "threadRoot": "",
        "body": "",
        "published": "",
        "updated": "",
        "receivedAt": int(received_ms or 0),
        "author": actor["handle"],
        "authorName": actor["name"],
        "authorId": actor["id"],
        "authorUrl": actor["url"],
        "backlink": remote_id,
        "sourceInstance": actor["instance"],
        "sourceSoftware": actor["software"],
        "lifecycle": "active",
        "tombstone": False,
        "moderation": {
            "state": "visible",
            "reason": "",
            "actor": "",
        },
        "provenance": {
            "source": "activitypub",
            "protocol": "ActivityPub",
            "software": actor["software"],
            "instance": actor["instance"],
            "remoteId": remote_id,
            "backlink": remote_id,
            "nativeEvent": False,
            "nativeEventId": "",
            "signatureVerified": False,
            "separationNotice": (
                "Remote ActivityPub reply; not part of ForkMesh's signed "
                "native event log."
            ),
        },
    }


def normalize_activity(activity, remote_actor=None, context=None, received_ms=0):
    """Normalize one Lemmy-compatible activity into a lifecycle operation.

    ``context`` comes from the already-resolved local ActivityPub object.  A
    remote activity cannot self-assert which private or public ForkMesh thread
    it belongs to.
    """
    if not isinstance(activity, dict):
        return {"ok": False, "reason": "invalid_activity"}
    activity_type = str(activity.get("type", "") or "")
    if activity_type not in ("Create", "Update", "Delete", "Remove", "Undo"):
        return {"ok": False, "reason": "unsupported_activity"}
    obj = activity.get("object")
    if activity_type == "Undo" and isinstance(obj, dict):
        inner_type = str(obj.get("type", "") or "")
        if inner_type != "Remove":
            return {"ok": False, "reason": "unsupported_undo"}
        target = _object_id(obj.get("object"))
        return {
            "ok": bool(_https_url(target)),
            "operation": "restore",
            "remoteId": _https_url(target),
            "moderator": _object_id(activity.get("actor")),
        }
    if activity_type in ("Delete", "Remove"):
        remote_id = _https_url(_object_id(obj))
        return {
            "ok": bool(remote_id),
            "operation": "delete" if activity_type == "Delete" else "moderate",
            "remoteId": remote_id,
            "moderator": _object_id(activity.get("actor")),
            "reason": sanitize_remote_content(activity.get("summary", ""), 240),
        }
    if not isinstance(obj, dict) or str(obj.get("type", "")) not in _NOTE_TYPES:
        return {"ok": False, "reason": "unsupported_object"}
    remote_id = _https_url(obj.get("id"))
    fallback_actor = _object_id(obj.get("attributedTo")) or _object_id(
        activity.get("actor")
    )
    actor = _actor_record(remote_actor, fallback_actor)
    if not remote_id or not actor["id"]:
        return {"ok": False, "reason": "invalid_identity"}
    if actor["id"].rstrip("/") != _https_url(fallback_actor).rstrip("/"):
        return {"ok": False, "reason": "author_mismatch"}
    local_context = normalize_context(context)
    if not local_context:
        return {"ok": False, "reason": "unresolved_context"}
    record = _base_record(remote_id, actor, local_context, received_ms)
    parent = _https_url(_object_id(obj.get("inReplyTo")))
    root = _https_url(_object_id(obj.get("context")))
    backlink = _https_url(_object_id(obj.get("url"))) or remote_id
    record.update(
        {
            "parentRemoteId": parent,
            "threadRoot": root or parent,
            "body": sanitize_remote_content(obj.get("content", "")),
            "published": str(obj.get("published", "") or ""),
            "updated": str(obj.get("updated", "") or ""),
            "backlink": backlink,
            "lifecycle": "edited" if activity_type == "Update" else "active",
        }
    )
    record["provenance"].update(
        {"backlink": backlink, "remoteId": remote_id}
    )
    if not record["body"]:
        return {"ok": False, "reason": "empty_content"}
    return {
        "ok": True,
        "operation": "update" if activity_type == "Update" else "create",
        "remoteId": remote_id,
        "record": record,
    }


def _records_by_id(records):
    if isinstance(records, dict):
        return {
            str(key): dict(value)
            for key, value in records.items()
            if isinstance(value, dict)
        }
    result = {}
    for value in records or ():
        if isinstance(value, dict) and value.get("remoteId"):
            result[str(value["remoteId"])] = dict(value)
    return result


def apply_activity(
    records,
    activity,
    remote_actor=None,
    context=None,
    received_ms=0,
    blocked_instances=(),
):
    """Apply one activity idempotently and return a new record mapping."""
    state = _records_by_id(records)
    normalized = normalize_activity(
        activity,
        remote_actor=remote_actor,
        context=context,
        received_ms=received_ms,
    )
    if not normalized.get("ok"):
        return {
            "ok": False,
            "changed": False,
            "reason": normalized.get("reason", "invalid_activity"),
            "records": state,
        }
    incoming = normalized.get("record")
    instance = ""
    if incoming:
        instance = incoming.get("sourceInstance", "")
    else:
        existing = state.get(normalized["remoteId"], {})
        instance = existing.get("sourceInstance", "")
    if instance_is_blocked(instance, blocked_instances):
        return {
            "ok": False,
            "changed": False,
            "reason": "instance_blocked",
            "records": state,
        }
    operation = normalized["operation"]
    remote_id = normalized["remoteId"]
    existing = state.get(remote_id)
    if operation == "create":
        if existing:
            return {
                "ok": True,
                "changed": False,
                "reason": "duplicate",
                "record": existing,
                "records": state,
            }
        state[remote_id] = incoming
    elif operation == "update":
        if not existing:
            return {
                "ok": False,
                "changed": False,
                "reason": "missing_original",
                "records": state,
            }
        if existing.get("authorId") != incoming.get("authorId"):
            return {
                "ok": False,
                "changed": False,
                "reason": "author_mismatch",
                "records": state,
            }
        merged = dict(existing)
        merged.update(
            {
                "body": incoming["body"],
                "updated": incoming["updated"],
                "backlink": incoming["backlink"],
                "lifecycle": "edited",
                "tombstone": False,
            }
        )
        merged["provenance"] = incoming["provenance"]
        state[remote_id] = merged
    elif operation == "delete":
        if not existing:
            return {
                "ok": True,
                "changed": False,
                "reason": "unknown_tombstone",
                "records": state,
            }
        tombstone = dict(existing)
        tombstone.update(
            {
                "body": "",
                "lifecycle": "tombstoned",
                "tombstone": True,
            }
        )
        state[remote_id] = tombstone
    elif operation == "moderate":
        if not existing:
            return {
                "ok": True,
                "changed": False,
                "reason": "unknown_moderation_target",
                "records": state,
            }
        moderated = dict(existing)
        moderated["body"] = ""
        moderated["lifecycle"] = "moderated"
        moderated["moderation"] = {
            "state": "removed",
            "reason": normalized.get("reason", ""),
            "actor": normalized.get("moderator", ""),
        }
        state[remote_id] = moderated
    elif operation == "restore":
        if not existing or existing.get("lifecycle") != "moderated":
            return {
                "ok": True,
                "changed": False,
                "reason": "nothing_to_restore",
                "records": state,
            }
        restored = dict(existing)
        # A Remove deliberately erased the public projection.  Undo makes the
        # record eligible for a refetch/redelivery instead of resurrecting
        # content from a moderation-hidden cache.
        restored["lifecycle"] = "awaiting-redelivery"
        restored["moderation"] = {
            "state": "restored",
            "reason": "",
            "actor": normalized.get("moderator", ""),
        }
        state[remote_id] = restored
    return {
        "ok": True,
        "changed": True,
        "reason": operation,
        "record": state.get(remote_id),
        "records": state,
    }


def thread_projection(records, blocked_instances=()):
    """Build a client-safe, parent-before-child projection.

    Blocked instances are omitted. Moderated and tombstoned entries remain as
    content-free placeholders so nested replies do not lose their shape.
    """
    state = _records_by_id(records)
    allowed = {
        key: value
        for key, value in state.items()
        if not instance_is_blocked(
            value.get("sourceInstance", ""), blocked_instances
        )
    }
    depth_cache = {}

    def depth_for(remote_id, seen=None):
        if remote_id in depth_cache:
            return depth_cache[remote_id]
        seen = set(seen or ())
        if remote_id in seen:
            depth_cache[remote_id] = 0
            return 0
        seen.add(remote_id)
        parent = str(allowed.get(remote_id, {}).get("parentRemoteId", "") or "")
        depth = 0 if parent not in allowed else min(8, 1 + depth_for(parent, seen))
        depth_cache[remote_id] = depth
        return depth

    projected = []
    for remote_id, record in allowed.items():
        lifecycle = str(record.get("lifecycle", "active") or "active")
        visible = lifecycle in VISIBLE_STATES
        projected.append(
            {
                "id": remote_id,
                "remoteId": remote_id,
                "parentRemoteId": record.get("parentRemoteId", ""),
                "depth": depth_for(remote_id),
                "author": record.get("author", ""),
                "authorName": record.get("authorName", ""),
                "authorUrl": record.get("authorUrl", ""),
                "body": record.get("body", "") if visible else "",
                "url": record.get("backlink", ""),
                "published": record.get("published", ""),
                "updated": record.get("updated", ""),
                "ts": int(record.get("receivedAt", 0) or 0),
                "lifecycle": lifecycle,
                "edited": lifecycle == "edited",
                "tombstone": lifecycle == "tombstoned",
                "moderated": lifecycle in ("moderated", "awaiting-redelivery"),
                "federated": True,
                "nativeEvent": False,
                "sourceInstance": record.get("sourceInstance", ""),
                "sourceSoftware": record.get("sourceSoftware", "activitypub"),
                "provenance": dict(record.get("provenance") or {}),
            }
        )
    projected.sort(
        key=lambda item: (
            int(item.get("ts", 0) or 0),
            str(item.get("published", "")),
            str(item.get("remoteId", "")),
        )
    )
    return projected
