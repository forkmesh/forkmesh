"""Two-stage, account-based governance for marketplace publication.

Package submissions are immutable and content-addressed. Five distinct active
members must approve the exact manifest digest. A different current owner of
the ``forkmesh/forkmesh`` organization repository then makes the final
publication decision.

Governance never reads a wallet, payout address, balance, token, or payment
history (issue #516, ``www/docs/incentive-independence.md``). The legacy
``wallet_address``/``value_lamports``/``value_checked_at`` approval columns
are retired: new approvals store blanks and nothing reads them.
"""

from __future__ import annotations

import hashlib
import json
import re
from urllib.parse import urlsplit, urlunsplit


MEMBER_APPROVALS_REQUIRED = 5
COMMUNITY_MARKETPLACE_ID = "forkmesh-community"
MAX_BODY_BYTES = 64 * 1024
MAX_MANIFEST_BYTES = 256 * 1024
MAX_PENDING_PER_SUBMITTER = 10
MAX_SUBMISSIONS_PER_DAY = 20
PACKAGE_RE = re.compile(r"^[a-z][a-z0-9]*(?:[.-][a-z0-9]+)*$")
VERSION_RE = re.compile(
    r"^[0-9]+\.[0-9]+\.[0-9]+(?:[-+][0-9A-Za-z.-]+)?$"
)
SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
REF_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._/-]{0,199}$")
SAFE_PATH_RE = re.compile(
    r"^(?!/)(?!.*(?:^|/)\.\.?(?:/|$))[A-Za-z0-9._/-]{1,320}$"
)
HOST_RE = re.compile(
    r"^[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?"
    r"(?:\.[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?)+$"
)
PACKAGE_TYPES = frozenset((
    "extension.native", "extension.vscode", "virtual.world",
    "virtual.avatar", "virtual.object", "virtual.clothing",
    "virtual.animation", "virtual.audio", "agent", "service.agent",
    "service.development", "service.design", "service.hosting",
    "service.compute", "theme",
))
LICENSE_MODELS = frozenset((
    "free-open-source", "one-time", "commercial", "subscription",
    "pay-what-you-want", "limited-edition", "free-with-paid-source",
    "revenue-share",
))


def _clean(value, maximum):
    return " ".join(str(value or "").split())[:maximum]


def _public_https(value):
    raw = str(value or "").strip()
    if not raw or len(raw) > 2048:
        return ""
    try:
        parsed = urlsplit(raw)
        host = str(parsed.hostname or "").lower().rstrip(".")
        if (
            parsed.scheme != "https"
            or not HOST_RE.fullmatch(host)
            or parsed.username
            or parsed.password
            or parsed.port not in (None, 443)
            or host.endswith((".local", ".internal", ".localhost"))
            or host.replace(".", "").isdigit()
        ):
            return ""
        return urlunsplit(("https", host, parsed.path or "/", parsed.query, ""))
    except Exception:
        return ""


def _safe_location(value, *, artifact=False):
    if not isinstance(value, dict):
        return None
    transport = str(value.get("transport") or "").lower()
    if transport not in (("https",) if artifact else ("git", "https")):
        return None
    url = _public_https(value.get("url"))
    ref = str(value.get("ref") or "")
    catalog = str(value.get("catalog") or "catalog.json")
    provider = _clean(value.get("provider"), 40).lower()
    if not url or (ref and not REF_RE.fullmatch(ref)):
        return None
    if transport == "git" and not SAFE_PATH_RE.fullmatch(catalog):
        return None
    result = {"transport": transport, "url": url}
    if ref:
        result["ref"] = ref
    if transport == "git":
        result["catalog"] = catalog
    if provider:
        result["provider"] = provider
    return result


def submission_fields(payload):
    """Normalize one immutable submission request, or return ``None``."""
    if not isinstance(payload, dict):
        return None
    marketplace = _clean(
        payload.get("marketplace") or COMMUNITY_MARKETPLACE_ID, 96
    ).lower()
    package = _clean(payload.get("package"), 96).lower()
    version = _clean(payload.get("version"), 80)
    manifest_url = _public_https(payload.get("manifestUrl"))
    manifest_sha = _clean(payload.get("manifestSha256"), 64).lower()
    source_locations = payload.get("sources")
    artifact = payload.get("artifact")
    if (
        marketplace != COMMUNITY_MARKETPLACE_ID
        or not PACKAGE_RE.fullmatch(package)
        or not VERSION_RE.fullmatch(version)
        or not manifest_url
        or not SHA256_RE.fullmatch(manifest_sha)
        or not isinstance(source_locations, list)
        or not 1 <= len(source_locations) <= 16
        or not isinstance(artifact, dict)
    ):
        return None
    sources = [_safe_location(item) for item in source_locations]
    artifact_locations = [
        _safe_location(item, artifact=True)
        for item in artifact.get("locations", [])
    ] if isinstance(artifact.get("locations"), list) else []
    artifact_sha = _clean(artifact.get("sha256"), 64).lower()
    artifact_size = artifact.get("size")
    if (
        any(item is None for item in sources)
        or not 1 <= len(artifact_locations) <= 16
        or any(item is None for item in artifact_locations)
        or not SHA256_RE.fullmatch(artifact_sha)
        or not isinstance(artifact_size, int)
        or not 1 <= artifact_size <= 512 * 1024 * 1024
    ):
        return None
    return {
        "marketplace": marketplace,
        "package": package,
        "version": version,
        "manifestUrl": manifest_url,
        "manifestSha256": manifest_sha,
        "sources": sources,
        "artifact": {
            "sha256": artifact_sha,
            "size": artifact_size,
            "locations": artifact_locations,
        },
    }


def validate_manifest_text(text, fields):
    """Verify the fetched bytes and the minimum modular package contract."""
    if not isinstance(text, str) or len(text.encode("utf-8")) > MAX_MANIFEST_BYTES:
        return "manifest_too_large"
    digest = hashlib.sha256(text.encode("utf-8")).hexdigest()
    if digest != fields["manifestSha256"]:
        return "manifest_digest_mismatch"
    try:
        manifest = json.loads(text)
    except (TypeError, ValueError):
        return "invalid_manifest_json"
    if not isinstance(manifest, dict) or manifest.get("schemaVersion") != 1:
        return "unsupported_manifest"
    if (
        manifest.get("id") != fields["package"]
        or manifest.get("version") != fields["version"]
    ):
        return "manifest_identity_mismatch"
    name = str(manifest.get("name") or "")
    summary = str(manifest.get("summary") or "")
    creator = manifest.get("creator")
    license_block = manifest.get("license")
    price = manifest.get("price")
    entrypoints = manifest.get("entrypoints")
    permissions = manifest.get("permissions")
    screenshots = manifest.get("screenshots")
    if (
        not 1 <= len(name) <= 80
        or not 1 <= len(summary) <= 240
        or manifest.get("type") not in PACKAGE_TYPES
        or not isinstance(creator, dict)
        or not str(creator.get("id") or "").startswith("did:forkmesh:")
        or not re.fullmatch(r"[a-z0-9][a-z0-9-]*", str(creator.get("handle") or ""))
        or not 1 <= len(str(creator.get("displayName") or "")) <= 80
        or not isinstance(creator.get("verified"), bool)
        or not isinstance(license_block, dict)
        or license_block.get("model") not in LICENSE_MODELS
        or not 1 <= len(str(license_block.get("spdx") or "")) <= 64
        or license_block.get("creatorRetainsOwnership") is not True
        or not isinstance(price, dict)
        or isinstance(price.get("amount"), bool)
        or not isinstance(price.get("amount"), (int, float))
        or price.get("amount") < 0
        or not re.fullmatch(r"[A-Z]{3}", str(price.get("currency") or ""))
    ):
        return "invalid_manifest_metadata"
    if (
        not isinstance(entrypoints, dict)
        or not entrypoints
        or not str(entrypoints.get("settings") or "")
    ):
        return "manifest_entrypoints_required"
    for key, value in entrypoints.items():
        if (
            not re.fullmatch(r"[a-z][a-z0-9.-]{0,63}", str(key))
            or not 1 <= len(str(value or "")) <= 160
        ):
            return "invalid_manifest_entrypoint"
    if not isinstance(permissions, list) or len(permissions) > 32:
        return "invalid_manifest_permissions"
    permission_ids = set()
    for permission in permissions:
        if not isinstance(permission, dict):
            return "invalid_manifest_permissions"
        permission_id = str(permission.get("id") or "")
        if (
            not re.fullmatch(r"[a-z][a-z0-9.]*", permission_id)
            or permission_id in permission_ids
            or not 1 <= len(str(permission.get("label") or "")) <= 120
            or permission.get("risk") not in ("low", "sensitive", "high")
        ):
            return "invalid_manifest_permissions"
        permission_ids.add(permission_id)
    if not isinstance(screenshots, list) or not 1 <= len(screenshots) <= 8:
        return "invalid_manifest_screenshots"
    for screenshot in screenshots:
        path = str((screenshot or {}).get("path") or "") \
            if isinstance(screenshot, dict) else ""
        alt = str((screenshot or {}).get("alt") or "") \
            if isinstance(screenshot, dict) else ""
        if (
            not path.startswith("preview/")
            or not SAFE_PATH_RE.fullmatch(path)
            or not 1 <= len(alt) <= 160
        ):
            return "invalid_manifest_screenshots"
    lifecycle = manifest.get("lifecycle")
    migrations = lifecycle.get("migrations") if isinstance(lifecycle, dict) else None
    if not isinstance(migrations, list) or len(migrations) > 128:
        return "manifest_lifecycle_required"
    migration_ids = set()
    for migration in migrations:
        if not isinstance(migration, dict):
            return "invalid_manifest_migration"
        migration_id = str(migration.get("id") or "")
        up = str(migration.get("up") or "")
        down = str(migration.get("down") or "")
        if (
            not re.fullmatch(r"[0-9]{4}_[a-z][a-z0-9_-]{0,90}", migration_id)
            or migration_id in migration_ids
            or not up.startswith("migrations/")
            or not down.startswith("migrations/")
            or up == down
            or not SAFE_PATH_RE.fullmatch(up)
            or not SAFE_PATH_RE.fullmatch(down)
        ):
            return "invalid_manifest_migration"
        migration_ids.add(migration_id)
    return ""


def _route(path):
    clean = str(path or "").rstrip("/")
    if clean == "/api/market/submissions":
        return "", "collection"
    if clean == "/api/market/catalog.json":
        return "", "catalog"
    match = re.fullmatch(
        r"/api/market/submissions/([0-9a-f]{32})"
        r"(?:/(member-approval|owner-approval|reject))?",
        clean,
    )
    if not match:
        return None
    return match.group(1), match.group(2) or "detail"


def _response(runtime, payload, status=200, allow=""):
    headers = {"x-content-type-options": "nosniff"}
    if allow:
        headers["allow"] = allow
    return runtime.response(
        payload,
        status=status,
        cache_control="no-store, max-age=0, must-revalidate",
        extra_headers=headers,
    )


def _public_approvals(rows):
    return [
        {
            "name": str(row.get("reviewer_name") or ""),
            "approvedAt": max(0, int(row.get("approved_at") or 0)),
        }
        for row in rows or []
    ]


async def _approval_rows(runtime, submission_id):
    return await runtime.d1_all(
        "SELECT reviewer_bi,reviewer_name,approved_at "
        "FROM marketplace_member_approvals WHERE submission_id=? "
        "ORDER BY approved_at ASC,reviewer_name ASC",
        submission_id,
    )


async def _project(runtime, row, viewer_bi="", approval_rows=None):
    submission_id = str(row.get("submission_id") or "")
    if approval_rows is None:
        approval_rows = await _approval_rows(runtime, submission_id)
    approvals = _public_approvals(approval_rows)
    viewer_approval = any(
        str(item.get("reviewer_bi") or "") == str(viewer_bi)
        for item in approval_rows or []
    )
    return {
        "id": submission_id,
        "marketplace": str(row.get("marketplace_id") or ""),
        "package": str(row.get("package_id") or ""),
        "version": str(row.get("version") or ""),
        "manifestUrl": str(row.get("manifest_url") or ""),
        "manifestSha256": str(row.get("manifest_sha256") or ""),
        "sources": json.loads(str(row.get("sources_json") or "[]")),
        "artifact": {
            "sha256": str(row.get("artifact_sha256") or ""),
            "size": int(row.get("artifact_size") or 0),
            "locations": json.loads(
                str(row.get("artifact_locations_json") or "[]")
            ),
        },
        "submittedBy": str(row.get("submitted_by_name") or ""),
        "state": str(row.get("state") or ""),
        "createdAt": int(row.get("created_at") or 0),
        "updatedAt": int(row.get("updated_at") or 0),
        "publishedAt": int(row.get("published_at") or 0),
        "memberApprovalsRequired": MEMBER_APPROVALS_REQUIRED,
        "memberApprovals": approvals,
        "memberApprovalCount": len(approvals),
        "ownerApprovedBy": str(row.get("owner_approved_by_name") or ""),
        "rejectionReason": str(row.get("rejection_reason") or ""),
        "viewerApproved": viewer_approval,
        "viewerIsSubmitter": bool(
            viewer_bi and viewer_bi == str(row.get("submitted_by_bi") or "")
        ),
    }


async def _project_many(runtime, rows, viewer_bi=""):
    rows = list(rows or [])
    ids = [str(row.get("submission_id") or "") for row in rows]
    grouped = {submission_id: [] for submission_id in ids}
    if ids:
        placeholders = ",".join("?" for _ in ids)
        approvals = await runtime.d1_all(
            "SELECT submission_id,reviewer_bi,reviewer_name,approved_at "
            "FROM marketplace_member_approvals "
            f"WHERE submission_id IN ({placeholders}) ORDER BY "
            "submission_id,approved_at ASC,reviewer_name ASC",
            *ids,
        )
        for approval in approvals or []:
            grouped.setdefault(
                str(approval.get("submission_id") or ""), []
            ).append(approval)
    return [
        await _project(
            runtime, row, viewer_bi,
            grouped.get(str(row.get("submission_id") or ""), []),
        )
        for row in rows
    ]


async def _submission(runtime, submission_id):
    return await runtime.d1_first(
        "SELECT * FROM marketplace_submissions WHERE submission_id=?",
        submission_id,
    )


async def _refresh_approvals(runtime, row):
    """Revalidate member accounts immediately before final approval."""
    submission_id = str(row.get("submission_id") or "")
    approvals = await runtime.d1_all(
        "SELECT reviewer_bi FROM marketplace_member_approvals "
        "WHERE submission_id=?",
        submission_id,
    )
    valid = 0
    for approval in approvals or []:
        reviewer_bi = str(approval.get("reviewer_bi") or "")
        account = await runtime.account(reviewer_bi)
        active = bool(account) and str(
            (account or {}).get("status") or "active") == "active"
        if not active:
            await runtime.d1_run(
                "DELETE FROM marketplace_member_approvals "
                "WHERE submission_id=? AND reviewer_bi=?",
                submission_id, reviewer_bi,
            )
            continue
        valid += 1
    await runtime.d1_run(
        "UPDATE marketplace_submissions SET state=?,updated_at=? "
        "WHERE submission_id=? AND state IN ('member_review','owner_review')",
        "owner_review" if valid >= MEMBER_APPROVALS_REQUIRED else "member_review",
        runtime.now(), submission_id,
    )
    return valid


async def _catalog(runtime):
    rows = await runtime.d1_all(
        "SELECT s.* FROM marketplace_submissions s WHERE s.state='published' "
        "AND NOT EXISTS (SELECT 1 FROM marketplace_submissions newer "
        "WHERE newer.marketplace_id=s.marketplace_id "
        "AND newer.package_id=s.package_id AND newer.state='published' "
        "AND (newer.published_at>s.published_at OR "
        "(newer.published_at=s.published_at AND newer.submission_id>s.submission_id))) "
        "ORDER BY s.marketplace_id,s.package_id"
    )
    packages = []
    for row in rows or []:
        package_id = str(row.get("package_id") or "")
        packages.append({
            "id": package_id,
            "version": str(row.get("version") or ""),
            "manifest": f"packages/{package_id}/manifest.json",
            "manifestSha256": str(row.get("manifest_sha256") or ""),
            "sources": json.loads(str(row.get("sources_json") or "[]")),
            "artifact": {
                "sha256": str(row.get("artifact_sha256") or ""),
                "size": int(row.get("artifact_size") or 0),
                "locations": json.loads(
                    str(row.get("artifact_locations_json") or "[]")
                ),
            },
        })
    return _response(runtime, {
        "$schema": "https://forkmesh.com/schemas/marketplace-catalog-v2.json",
        "schemaVersion": 2,
        "marketplace": {
            "id": "forkmesh-community",
            "name": "ForkMesh Community",
            "locations": [{
                "transport": "https",
                "url": runtime.public_catalog_url(),
                "provider": "forkmesh",
            }],
        },
        "packages": packages,
    })


async def handle(runtime, path):
    route = _route(path)
    if route is None:
        return _response(runtime, {"error": "not_found"}, 404)
    submission_id, action = route
    method = runtime.method()
    if action == "catalog":
        if method != "GET":
            return _response(runtime, {"error": "method_not_allowed"}, 405, "GET")
        await runtime.ensure_schema()
        return await _catalog(runtime)
    if method not in ("GET", "POST", "DELETE"):
        return _response(runtime, {"error": "method_not_allowed"}, 405, "GET, POST, DELETE")
    data = {}
    if method in ("POST", "DELETE"):
        if not runtime.same_origin():
            return _response(runtime, {"error": "origin_not_allowed"}, 403)
        data, error = await runtime.json_body(MAX_BODY_BYTES)
        if error:
            return _response(
                runtime, {"error": error},
                413 if error == "payload_too_large" else 400,
            )
    await runtime.ensure_schema()
    viewer_bi, viewer = await runtime.session(data)
    viewer_name = str((viewer or {}).get("name") or "").strip().lower()

    if action == "collection" and method == "GET":
        rows = await runtime.d1_all(
            "SELECT * FROM marketplace_submissions "
            "ORDER BY created_at DESC,submission_id DESC LIMIT 100"
        )
        return _response(runtime, {
            "ok": True,
            "memberApprovalsRequired": MEMBER_APPROVALS_REQUIRED,
            "submissions": await _project_many(
                runtime, rows, str(viewer_bi or "")),
        })

    if action == "collection" and method == "POST":
        if not viewer_bi or not viewer_name:
            return _response(runtime, {"error": "invalid_session"}, 401)
        fields = submission_fields(data)
        if fields is None:
            return _response(runtime, {"error": "invalid_submission"}, 400)
        quota = await runtime.d1_first(
            "SELECT SUM(CASE WHEN state IN ('member_review','owner_review') "
            "THEN 1 ELSE 0 END) AS pending,"
            "SUM(CASE WHEN created_at>=? THEN 1 ELSE 0 END) AS recent "
            "FROM marketplace_submissions WHERE submitted_by_bi=?",
            runtime.now() - 24 * 60 * 60 * 1000, str(viewer_bi),
        ) or {}
        if (
            int(quota.get("pending") or 0) >= MAX_PENDING_PER_SUBMITTER
            or int(quota.get("recent") or 0) >= MAX_SUBMISSIONS_PER_DAY
        ):
            return _response(runtime, {"error": "submission_rate_limited"}, 429)
        manifest_text, fetch_error = await runtime.fetch_manifest(
            fields["manifestUrl"], MAX_MANIFEST_BYTES
        )
        if fetch_error:
            return _response(runtime, {"error": fetch_error}, 400)
        manifest_error = validate_manifest_text(manifest_text, fields)
        if manifest_error:
            return _response(runtime, {"error": manifest_error}, 400)
        canonical = "\n".join((
            "forkmesh-marketplace-submission-v1",
            fields["marketplace"], fields["package"], fields["version"],
            fields["manifestSha256"], viewer_name,
        ))
        new_id = hashlib.sha256(canonical.encode("utf-8")).hexdigest()[:32]
        now = runtime.now()
        try:
            await runtime.d1_run(
                "INSERT INTO marketplace_submissions "
                "(submission_id,marketplace_id,package_id,version,manifest_url,"
                "manifest_sha256,sources_json,artifact_sha256,artifact_size,"
                "artifact_locations_json,submitted_by_bi,submitted_by_name,state,"
                "created_at,updated_at) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
                new_id, fields["marketplace"], fields["package"], fields["version"],
                fields["manifestUrl"], fields["manifestSha256"],
                json.dumps(fields["sources"], separators=(",", ":")),
                fields["artifact"]["sha256"], fields["artifact"]["size"],
                json.dumps(fields["artifact"]["locations"], separators=(",", ":")),
                str(viewer_bi), viewer_name, "member_review", now, now,
            )
        except Exception:
            existing = await runtime.d1_first(
                "SELECT submission_id FROM marketplace_submissions WHERE "
                "marketplace_id=? AND package_id=? AND version=? "
                "AND manifest_sha256=?",
                fields["marketplace"], fields["package"], fields["version"],
                fields["manifestSha256"],
            )
            if existing:
                return _response(runtime, {
                    "error": "submission_exists",
                    "id": str(existing.get("submission_id") or ""),
                }, 409)
            raise
        await runtime.audit(viewer_name, "marketplace.submit", new_id, "success")
        return _response(
            runtime, await _project(runtime, await _submission(runtime, new_id), str(viewer_bi)), 201
        )

    row = await _submission(runtime, submission_id)
    if not row:
        return _response(runtime, {"error": "not_found"}, 404)
    if action == "detail" and method == "GET":
        return _response(runtime, await _project(runtime, row, str(viewer_bi or "")))
    if not viewer_bi or not viewer_name:
        return _response(runtime, {"error": "invalid_session"}, 401)

    state = str(row.get("state") or "")
    if action == "member-approval":
        if state not in ("member_review", "owner_review"):
            return _response(runtime, {"error": "submission_not_reviewable"}, 409)
        if str(row.get("submitted_by_bi") or "") == str(viewer_bi):
            return _response(runtime, {"error": "submitter_cannot_approve"}, 403)
        if method == "POST":
            if str((viewer or {}).get("status") or "active") != "active":
                return _response(runtime, {"error": "active_account_required"}, 403)
            # The retired asset columns are NOT NULL, so they receive blanks;
            # an upsert also clears any value a pre-#516 approval stored.
            await runtime.d1_run(
                "INSERT INTO marketplace_member_approvals "
                "(submission_id,reviewer_bi,reviewer_name,wallet_address,"
                "value_lamports,value_checked_at,manifest_sha256,approved_at) "
                "VALUES(?,?,?,'',0,0,?,?) ON CONFLICT(submission_id,reviewer_bi) "
                "DO UPDATE SET reviewer_name=excluded.reviewer_name,"
                "wallet_address='',value_lamports=0,value_checked_at=0,"
                "manifest_sha256=excluded.manifest_sha256,"
                "approved_at=excluded.approved_at",
                submission_id, str(viewer_bi), viewer_name,
                str(row.get("manifest_sha256") or ""), runtime.now(),
            )
        elif method == "DELETE":
            await runtime.d1_run(
                "DELETE FROM marketplace_member_approvals "
                "WHERE submission_id=? AND reviewer_bi=?",
                submission_id, str(viewer_bi),
            )
        else:
            return _response(runtime, {"error": "method_not_allowed"}, 405, "POST, DELETE")
        count = await runtime.d1_first(
            "SELECT COUNT(*) AS n FROM marketplace_member_approvals "
            "WHERE submission_id=? AND manifest_sha256=?",
            submission_id, str(row.get("manifest_sha256") or ""),
        )
        approvals = int((count or {}).get("n") or 0)
        await runtime.d1_run(
            "UPDATE marketplace_submissions SET state=?,updated_at=? "
            "WHERE submission_id=? AND state IN ('member_review','owner_review')",
            "owner_review" if approvals >= MEMBER_APPROVALS_REQUIRED else "member_review",
            runtime.now(), submission_id,
        )
        await runtime.audit(
            viewer_name,
            "marketplace.member_approve" if method == "POST" else "marketplace.member_revoke",
            submission_id, "success",
        )
        return _response(runtime, await _project(
            runtime, await _submission(runtime, submission_id), str(viewer_bi)
        ))

    if action in ("owner-approval", "reject"):
        if method != "POST":
            return _response(runtime, {"error": "method_not_allowed"}, 405, "POST")
        if not await runtime.is_forkmesh_repo_owner(viewer_name):
            return _response(runtime, {"error": "forkmesh_owner_required"}, 403)
        if str(row.get("submitted_by_bi") or "") == str(viewer_bi):
            return _response(runtime, {"error": "submitter_cannot_finalize"}, 403)
        prior = await runtime.d1_first(
            "SELECT 1 AS ok FROM marketplace_member_approvals "
            "WHERE submission_id=? AND reviewer_bi=?",
            submission_id, str(viewer_bi),
        )
        if prior:
            return _response(runtime, {"error": "member_reviewer_cannot_finalize"}, 403)
        if state in ("published", "rejected", "withdrawn"):
            return _response(runtime, {"error": "submission_already_final"}, 409)
        if action == "reject":
            reason = _clean(data.get("reason"), 300)
            if not reason:
                return _response(runtime, {"error": "rejection_reason_required"}, 400)
            await runtime.d1_run(
                "UPDATE marketplace_submissions SET state='rejected',"
                "owner_approved_by_bi=?,owner_approved_by_name=?,"
                "rejection_reason=?,updated_at=? WHERE submission_id=? "
                "AND state IN ('member_review','owner_review')",
                str(viewer_bi), viewer_name, reason, runtime.now(), submission_id,
            )
            rejected = await _submission(runtime, submission_id)
            if (
                str((rejected or {}).get("state") or "") != "rejected"
                or str((rejected or {}).get("owner_approved_by_bi") or "")
                != str(viewer_bi)
            ):
                return _response(runtime, {"error": "approval_race_retry"}, 409)
            await runtime.audit(viewer_name, "marketplace.reject", submission_id, "success")
        else:
            valid = await _refresh_approvals(runtime, row)
            if valid < MEMBER_APPROVALS_REQUIRED:
                return _response(runtime, {
                    "error": "member_approvals_required",
                    "required": MEMBER_APPROVALS_REQUIRED,
                    "current": valid,
                }, 409)
            now = runtime.now()
            await runtime.d1_run(
                "UPDATE marketplace_submissions SET state='published',"
                "owner_approved_by_bi=?,owner_approved_by_name=?,"
                "published_at=?,updated_at=? WHERE submission_id=? "
                "AND state='owner_review' AND (SELECT COUNT(*) FROM "
                "marketplace_member_approvals WHERE submission_id=? "
                "AND manifest_sha256=?)>=?",
                str(viewer_bi), viewer_name, now, now, submission_id,
                submission_id, str(row.get("manifest_sha256") or ""),
                MEMBER_APPROVALS_REQUIRED,
            )
            published = await _submission(runtime, submission_id)
            if (
                str((published or {}).get("state") or "") != "published"
                or str((published or {}).get("owner_approved_by_bi") or "")
                != str(viewer_bi)
            ):
                return _response(runtime, {"error": "approval_race_retry"}, 409)
            await runtime.audit(viewer_name, "marketplace.publish", submission_id, "success")
        return _response(runtime, await _project(
            runtime, await _submission(runtime, submission_id), str(viewer_bi)
        ))

    if action == "detail" and method == "DELETE":
        if str(row.get("submitted_by_bi") or "") != str(viewer_bi):
            return _response(runtime, {"error": "forbidden"}, 403)
        if state not in ("member_review", "owner_review"):
            return _response(runtime, {"error": "submission_already_final"}, 409)
        await runtime.d1_run(
            "UPDATE marketplace_submissions SET state='withdrawn',updated_at=? "
            "WHERE submission_id=? AND submitted_by_bi=? "
            "AND state IN ('member_review','owner_review')",
            runtime.now(), submission_id, str(viewer_bi),
        )
        await runtime.audit(viewer_name, "marketplace.withdraw", submission_id, "success")
        return _response(runtime, await _project(
            runtime, await _submission(runtime, submission_id), str(viewer_bi)
        ))
    return _response(runtime, {"error": "method_not_allowed"}, 405)
