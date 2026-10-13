"""Validated public API snapshot rows served by the App Worker."""

import json


# Every row is accepted only after the publisher validates the complete
# path-specific schema. Serving first checks bounded D1 metadata, then repeats
# this validator inside the App ingress Durable Object so syntactically valid
# but corrupt rows still fail closed without using stateless CPU.
SNAPSHOT_SCHEMA_VERSION = 1
SNAPSHOT_KIND = "forkmesh-public-api-snapshot"
SNAPSHOT_TOMBSTONE_KIND = "forkmesh-public-api-snapshot-tombstone"
SNAPSHOT_TOMBSTONE_MS = 2 * 60 * 1000
SNAPSHOT_FREQUENT_CADENCE_MINUTES = 3
SNAPSHOT_SLOW_CADENCE_MINUTES = 5
# D1 rows have a 2 MB limit. Keep a conservative 1.5 MiB payload ceiling so
# fixed metadata stays clear of it. A broad-incident status projection can
# legitimately exceed this cap; publication rejects it and the App uses the
# truthful no-store live fallback after the prior row expires.
SNAPSHOT_MAX_PAYLOAD_BYTES = 1536 * 1024
SNAPSHOT_PATH_MAX_BYTES = {
    "/api/repositories": SNAPSHOT_MAX_PAYLOAD_BYTES,
    "/api/network/overview": 512 * 1024,
    "/api/repo/forkmesh/forkmesh/mirrors": 1024 * 1024,
    "/api/status": SNAPSHOT_MAX_PAYLOAD_BYTES,
    "/api/status/world": 512 * 1024,
}
SNAPSHOT_PATHS = tuple(SNAPSHOT_PATH_MAX_BYTES)
SNAPSHOT_PATH_FRESH_MS = {
    "/api/repositories": 6 * 60 * 1000,
    "/api/network/overview": 6 * 60 * 1000,
    "/api/repo/forkmesh/forkmesh/mirrors": 4 * 60 * 1000,
    "/api/status": 4 * 60 * 1000,
    "/api/status/world": 4 * 60 * 1000,
}
SNAPSHOT_FREQUENT_PATHS = (
    "/api/repo/forkmesh/forkmesh/mirrors",
    "/api/status",
    "/api/status/world",
)
SNAPSHOT_STATUS_PATHS = ("/api/status", "/api/status/world")
SNAPSHOT_SLOW_PATHS = (
    "/api/repositories",
    "/api/network/overview",
)

# This is deliberately an exact schema, not merely a deny-list. New catalog
# response fields must be reviewed before the anonymous projection can persist
# them in the main database's public snapshot table.
CATALOG_REPOSITORY_KEYS = frozenset({
    "owner", "name", "visibility", "mirrorEncryption", "opaqueRepoId",
    "keyEpoch", "encryptedManifestHash", "encryptedManifestSig", "sizeBytes",
    "description", "website", "logoMetadata", "cloneUrl", "mirrorUrl",
    "solana", "channel",
    "hostedSince", "lastSync", "updatedAt", "rootCommit", "source", "commit",
    "branch", "issueCount", "issueMaxNumber", "commitCount", "mergeCount",
    "branchCount", "pullCount", "discussionCount", "activityWeeks",
    "mergeActivityWeeks", "worktreeCount",
    "artifactCount", "platform", "version", "nodeId", "clonesServed",
    "websiteServed", "maintainer", "signature", "stateHash", "stateSig",
    "actionsEnabled", "actionsState", "changedFiles", "cpuPercent",
    "memUsedBytes", "memTotalBytes", "diskUsedBytes", "diskTotalBytes",
    "machineName", "runtimeMode", "commitSubject", "commitAuthorName",
    "commitAt", "cloneServedAt", "websiteServedAt", "cloneServedAgent",
    "websiteServedAgent", "agentProviders", "liveHost", "canonicalNodeId",
    "sourceAuthority", "isPrivate", "sharedWithMe", "termsFlagged",
    "termsCategory", "sshUrl", "reportedSource", "ownerKind", "logicalOwner",
    "logicalOwners", "cloneOnline",
    "dashboardUrl",
})
CATALOG_LOGO_KEYS = frozenset({
    "description", "languages", "topics", "fileStructure", "frameworks",
    "projectCategory",
})
CATALOG_LOGICAL_OWNER_KEYS = frozenset({"kind", "owner"})
CATALOG_RESPONSE_KEYS = frozenset({
    "ok", "repositories", "sshGatewayConfigured", "sshTransport",
    "payoutCustody", "payoutNotice",
})
NETWORK_RESPONSE_KEYS = frozenset({"ok", "stats", "leaderboards", "history"})
MIRROR_RESPONSE_KEYS = frozenset({
    "ok", "owner", "repo", "groupKey", "generatedAt", "summary", "mirrors",
    "sourceOfTruth",
})
# Public verdicts the mirrors handler adds on top of the base shape: the
# availability proof (issue #492) and the shared sync status (issue #494).
# Each is admitted only as an object carrying its own schema id. Before these
# were listed, every republish since 2026-09-14 11:55 failed as "an unsafe
# public payload" and the error log turned the website and errors rows red.
MIRROR_OPTIONAL_RESPONSE_SCHEMAS = {
    "availability": "forkmesh-availability-proof-v1",
    "syncStatus": "forkmesh-sync-status-v1",
}
MIRROR_SUMMARY_KEYS = frozenset({
    "mirrors", "online", "cloneAvailable", "dataHostedBytes",
    "longestHostedSince", "freshestSync",
})
MIRROR_ITEM_KEYS = frozenset({
    "node", "owner", "ownerUser", "machineName", "repo", "status",
    "lastSeen", "hostedSince", "syncAgeMs", "lastSync", "behind",
    "cloneAvailable", "mirrorUrl", "sizeBytes", "source", "commit", "branch",
    "lastCommitMessage", "lastCommitAuthorName", "lastCommitAt", "changedFiles",
    "issueCount", "commitCount", "branchCount", "pullCount", "discussionCount",
    "worktreeCount", "artifactCount", "platform", "runtimeMode", "version",
    "agentProviders", "actionsEnabled", "actionsState", "id", "clonesServed",
    "websiteServed", "cloneServedAt", "cloneServedAgent", "websiteServedAt",
    "websiteServedAgent", "cpuPercent", "memUsedBytes", "memTotalBytes",
    "diskUsedBytes", "diskTotalBytes", "integrity", "activity",
    "activityUpdatedAt", "endpoint", "checkedAt", "latencyMs", "region",
    "endpointHealthy", "endpointIntegrity", "endpointFresh", "abuseBlocked",
    "operations", "directVerified", "directReachable", "directIntegrity",
    "directRefsSha256", "directCheckedAt", "directLatencyMs", "directStatus",
    "directReason",
    "dashboardUrl", "replicaScope",
})
MIRROR_SOURCE_KEYS = MIRROR_ITEM_KEYS | frozenset({
    "kind", "isSourceOfTruth", "referenceForMirrors",
})
STATUS_RESPONSE_KEYS = frozenset({"ok", "now", "systems", "current"})
MAX_SAFE_INTEGER = 9_007_199_254_740_991


def _safe_integer(value, minimum=0):
    return bool(
        not isinstance(value, bool)
        and isinstance(value, int)
        and minimum <= value <= MAX_SAFE_INTEGER
    )


def _catalog_record_schema_is_safe(repository):
    if not set(repository).issubset(CATALOG_REPOSITORY_KEYS):
        return False
    logo = repository.get("logoMetadata")
    if "logoMetadata" in repository and (
        not isinstance(logo, dict)
        or not set(logo).issubset(CATALOG_LOGO_KEYS)
        or not isinstance(logo.get("languages"), dict)
        or any(not isinstance(key, str) for key in logo["languages"])
        or any(
            not _safe_integer(value)
            for value in logo["languages"].values()
        )
        or any(
            not isinstance(logo.get(key), list)
            or any(not isinstance(value, str) for value in logo[key])
            for key in ("topics", "fileStructure", "frameworks")
            if key in logo
        )
        or any(
            not isinstance(logo.get(key), str)
            for key in ("description", "projectCategory")
            if key in logo
        )
    ):
        return False
    owners = repository.get("logicalOwners")
    if "logicalOwners" not in repository:
        return True
    if not isinstance(owners, list) or len(owners) > 20:
        return False
    return all(
        isinstance(owner, dict)
        and set(owner).issubset(CATALOG_LOGICAL_OWNER_KEYS)
        and owner.get("kind") in ("user", "organization")
        and isinstance(owner.get("owner"), str)
        for owner in owners
    )

def _object(payload, key):
    return isinstance(payload.get(key), dict)


def _list(payload, key, maximum):
    value = payload.get(key)
    return isinstance(value, list) and len(value) <= maximum


def project_world_status_payload(payload):
    """Derive the compact World body from one already-built full response."""
    projected = []
    for system in payload.get("systems", []):
        days = list(system.get("days") or [])
        compact = {
            key: value for key, value in system.items()
            if key not in (
                "days", "minutes", "checkDescription", "checks", "nextCheck")
        }
        # World draws only state strips, not the verbose monitoring reasons or
        # coverage explanations. Preserve the canonical day counts so its
        # client derives the colour from the same full-response facts.
        compact["days"] = [
            {
                "dayTs": day.get("dayTs"),
                "checks": day.get("checks"),
                "failures": day.get("failures"),
            }
            for day in days
        ]
        compact["hours"] = [
            {
                "hourTs": hour.get("hourTs"),
                "status": hour.get("status"),
            }
            for day in days
            for hour in (day.get("hours") or [])
        ][-24:]
        compact["minutes"] = [
            {
                "minuteTs": minute.get("minuteTs"),
                "status": minute.get("status"),
            }
            for minute in list(system.get("minutes") or [])[-60:]
        ]
        projected.append(compact)
    return {
        "ok": payload.get("ok"),
        "now": payload.get("now"),
        "systems": projected,
        "current": payload.get("current"),
    }


def validate_snapshot_payload(path, payload, generated_at=None):
    """Fail closed unless a payload has the expected public response shape."""
    if not isinstance(payload, dict) or payload.get("ok") is not True:
        return False
    if path == "/api/repositories":
        if (
            set(payload) != CATALOG_RESPONSE_KEYS
            or not _list(payload, "repositories", 10_000)
        ):
            return False
        for repository in payload["repositories"]:
            if not isinstance(repository, dict):
                return False
            if (
                not _catalog_record_schema_is_safe(repository)
                or
                repository.get("visibility") != "public"
                or (
                    "isPrivate" in repository
                    and repository.get("isPrivate") is not False
                )
                or (
                    "sharedWithMe" in repository
                    and repository.get("sharedWithMe") is not False
                )
                or "privateAccessId" in repository
                or "privateAccessPath" in repository
                or repository.get("opaqueRepoId") not in (None, "")
                or repository.get("encryptedManifestHash") not in (None, "")
                or repository.get("encryptedManifestSig") not in (None, "")
                or repository.get("mirrorEncryption") not in (None, "")
                or (
                    repository.get("keyEpoch") is not None
                    and not (
                        _safe_integer(repository.get("keyEpoch"))
                        and repository.get("keyEpoch") == 0
                    )
                )
            ):
                return False
        return True
    if path == "/api/network/overview":
        return bool(
            set(payload) == NETWORK_RESPONSE_KEYS
            and all(
                _object(payload, key) and payload[key].get("ok") is True
                for key in ("stats", "leaderboards", "history")
            )
        )
    if path == "/api/repo/forkmesh/forkmesh/mirrors":
        if not (
            MIRROR_RESPONSE_KEYS
            <= set(payload)
            <= MIRROR_RESPONSE_KEYS | set(MIRROR_OPTIONAL_RESPONSE_SCHEMAS)
            and payload.get("owner") == "forkmesh"
            and payload.get("repo") == "forkmesh"
            and _object(payload, "sourceOfTruth")
            and set(payload["sourceOfTruth"]).issubset(MIRROR_SOURCE_KEYS)
            and payload["sourceOfTruth"].get("owner") == "forkmesh"
            and payload["sourceOfTruth"].get("repo") == "forkmesh"
            and payload["sourceOfTruth"].get("kind") == "working-copy"
            and payload["sourceOfTruth"].get("isSourceOfTruth") is True
            and _object(payload, "summary")
            and _list(payload, "mirrors", 1_000)
            and all(isinstance(item, dict) for item in payload["mirrors"])
        ):
            return False
        for key, schema in MIRROR_OPTIONAL_RESPONSE_SCHEMAS.items():
            if key in payload and not (
                isinstance(payload[key], dict)
                and payload[key].get("schema") == schema
            ):
                return False
        payload_generated_at = payload.get("generatedAt")
        if not _safe_integer(payload_generated_at, minimum=1):
            return False
        if generated_at is not None and abs(
            payload_generated_at - generated_at
        ) > 60_000:
            return False
        for mirror in payload["mirrors"]:
            if not (
                set(mirror).issubset(MIRROR_ITEM_KEYS)
                and isinstance(mirror.get("endpointFresh"), bool)
                and isinstance(mirror.get("endpointHealthy"), bool)
                and mirror.get("status") in ("online", "offline")
            ):
                return False
            checked_at = mirror.get("checkedAt")
            if not _safe_integer(checked_at):
                return False
            if generated_at is not None and checked_at > generated_at + 60_000:
                return False
        return set(payload["summary"]) == MIRROR_SUMMARY_KEYS
    if path in SNAPSHOT_STATUS_PATHS:
        if not (
            set(payload) == STATUS_RESPONSE_KEYS
            and _list(payload, "systems", 100)
            and _object(payload, "current")
            and all(isinstance(item, dict) for item in payload["systems"])
        ):
            return False
        payload_now = payload.get("now")
        if not _safe_integer(payload_now, minimum=1):
            return False
        if generated_at is not None and abs(payload_now - generated_at) > 60_000:
            return False
        last_sample = payload["current"].get("lastCronSampleTs")
        return bool(
            last_sample is None
            or (
                _safe_integer(last_sample, minimum=1)
                and 0 < last_sample <= payload_now + 60_000
            )
        )
    return False


def build_snapshot_rows(payloads, generated_at, deploy_fingerprint=""):
    """Build a validated endpoint group for one D1 batch transaction."""
    if not payloads or not set(payloads).issubset(SNAPSHOT_PATHS):
        raise ValueError("public snapshot set is invalid")
    try:
        generated_at = int(generated_at)
    except (TypeError, ValueError, OverflowError) as error:
        raise ValueError("public snapshot timestamp is invalid") from error
    if generated_at <= 0:
        raise ValueError("public snapshot timestamp is invalid")
    fingerprint = str(deploy_fingerprint or "")
    if fingerprint and not (
        len(fingerprint) == 64
        and all(character in "0123456789abcdef" for character in fingerprint)
    ):
        raise ValueError("public snapshot deploy fingerprint is invalid")

    rows = []
    for path in SNAPSHOT_PATHS:
        if path not in payloads:
            continue
        payload = payloads[path]
        if not validate_snapshot_payload(path, payload, generated_at):
            raise ValueError("public snapshot payload is invalid: " + path)
        body = json.dumps(
            payload, ensure_ascii=True, separators=(",", ":"),
            allow_nan=False,
        )
        length = len(body.encode("utf-8"))
        if length <= 1 or length > SNAPSHOT_PATH_MAX_BYTES[path]:
            raise ValueError("public snapshot payload exceeds its cap: " + path)
        rows.append({
            "path": path,
            "schemaVersion": SNAPSHOT_SCHEMA_VERSION,
            "kind": SNAPSHOT_KIND,
            "generatedAt": generated_at,
            "freshUntil": generated_at + SNAPSHOT_PATH_FRESH_MS[path],
            # Stale status/mirror verdicts can lie. Every path fails closed at
            # its freshness deadline rather than serving a last-good body.
            "staleUntil": generated_at + SNAPSHOT_PATH_FRESH_MS[path],
            "deployFingerprint": fingerprint,
            "contentLength": length,
            "payload": body,
        })
    return rows
