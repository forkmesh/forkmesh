from __future__ import annotations

import re

import apps
import apps_core as core


REVIEW_REASON_MAX = 500
DIRECTORY_LIMIT = 200
RATING_MARKET = "forkmesh-apps"


def _json(runtime, data, status=200, cache_control="no-store"):
    return runtime.response(data, status=status, cache_control=cache_control)


def _err(runtime, error, status=400, **extra):
    body = {"ok": False, "error": error}
    body.update(extra)
    return _json(runtime, body, status)


def _rating(row):
    count = max(0, int((row or {}).get("rating_count") or 0))
    total = max(0, int((row or {}).get("rating_total") or 0))
    return {"average": round(total / count, 1) if count else 0.0,
            "count": count}


async def directory(runtime):
    rows = await runtime.d1_all(
        "SELECT a.*, s.rating_count, s.rating_total FROM apps a "
        "LEFT JOIN marketplace_package_rating_stats s "
        "ON s.market=? AND s.package_id=a.slug "
        "WHERE a.listing='listed' ORDER BY a.updated_at DESC LIMIT ?",
        RATING_MARKET, DIRECTORY_LIMIT)
    listed = []
    for row in rows or []:
        record = await runtime.open(row.get("data"))
        if not isinstance(record, dict):
            continue
        view = core.public_app_view(row, record, _rating(row))
        view["homepageUrl"] = str(record.get("homepageUrl") or "")
        view["installs"] = await apps._install_count(runtime, row["app_id"])
        listed.append(view)
    listed.sort(key=lambda view: (-view["rating"]["average"],
                                  -view["installs"], view["name"].lower()))
    return _json(runtime, {"ok": True, "apps": listed,
                           "ratingMarket": RATING_MARKET},
                 cache_control="public, max-age=60")


async def submit(runtime, row, record, name):
    listing = str(row.get("listing") or "private")
    if listing not in ("private", "rejected"):
        return _err(runtime, "not_submittable", 409, listing=listing)
    await runtime.d1_run(
        "UPDATE apps SET listing='pending', updated_at=? WHERE app_id=?",
        runtime.now(), row["app_id"])
    await runtime.audit(name, "apps.listing.submit", "app", row["app_id"])
    return _json(runtime, {"ok": True, "listing": "pending"})


async def review_queue(runtime, name):
    if not await runtime.is_admin(name):
        return _err(runtime, "forbidden", 403)
    rows = await runtime.d1_all(
        "SELECT * FROM apps WHERE listing IN ('pending','listed','suspended') "
        "ORDER BY CASE listing WHEN 'pending' THEN 0 WHEN 'suspended' "
        "THEN 1 ELSE 2 END, updated_at DESC LIMIT ?", DIRECTORY_LIMIT)
    out = []
    for row in rows or []:
        record = await runtime.open(row.get("data"))
        if not isinstance(record, dict):
            continue
        view = await apps.developer_view(runtime, row, record)
        view["contentHash"] = core.content_hash(record)
        view["homepageUrl"] = str(record.get("homepageUrl") or "")
        history = await runtime.d1_all(
            "SELECT action, reason, self_review, created_at FROM app_reviews "
            "WHERE app_id=? ORDER BY created_at DESC LIMIT 5", row["app_id"])
        view["reviews"] = [{
            "action": str(item.get("action") or ""),
            "reason": str(item.get("reason") or ""),
            "selfReview": bool(item.get("self_review")),
            "at": int(item.get("created_at") or 0),
        } for item in history or []]
        out.append(view)
    return _json(runtime, {"ok": True, "apps": out})


async def review(runtime, account_bi, name, row, record, data):
    if not await runtime.is_admin(name):
        await runtime.audit(name, "apps.listing.review", "app",
                            row["app_id"], outcome="denied")
        return _err(runtime, "forbidden", 403)
    action = str(data.get("action") or "")
    allowed = core.REVIEW_ACTIONS.get(action)
    listing = str(row.get("listing") or "private")
    if not allowed:
        return _err(runtime, "invalid_action")
    if listing not in allowed:
        return _err(runtime, "invalid_transition", 409, listing=listing)
    reason = core.clean_text(data.get("reason"), REVIEW_REASON_MAX)
    if action in ("reject", "suspend") and len(reason) < 4:
        return _err(runtime, "reason_required")
    digest = core.content_hash(record)
    if action == "approve" and str(data.get("contentHash") or "") != digest:
        return _err(runtime, "content_changed", 409, contentHash=digest)
    target = {"approve": "listed", "reject": "rejected",
              "suspend": "suspended", "reinstate": "pending"}[action]
    now = runtime.now()
    self_review = 1 if row.get("owner_bi") == account_bi else 0
    await runtime.d1_run(
        "UPDATE apps SET listing=?, listed_hash=?, updated_at=? "
        "WHERE app_id=?", target, digest if target == "listed" else "",
        now, row["app_id"])
    await runtime.d1_run(
        "INSERT INTO app_reviews (review_id, app_id, action, reviewer_bi, "
        "self_review, reason, content_hash, created_at) "
        "VALUES (?,?,?,?,?,?,?,?)", runtime.new_hex(16), row["app_id"],
        action, account_bi, self_review, reason, digest, now)
    if target == "suspended":
        await runtime.d1_run(
            "UPDATE app_tokens SET revoked_at=? WHERE app_id=? "
            "AND revoked_at=0", now, row["app_id"])
        await runtime.d1_run(
            "DELETE FROM app_event_outbox WHERE app_id=?", row["app_id"])
    await runtime.audit(name, "apps.listing." + action, "app", row["app_id"],
                        details={"selfReview": bool(self_review)})
    owner = str(record.get("ownerName") or "")
    if owner:
        titles = {"approve": "%s is listed in the App Directory",
                  "reject": "%s was not accepted for the App Directory",
                  "suspend": "%s was suspended",
                  "reinstate": "%s is back in review"}
        await runtime.notify(owner, titles[action] % record.get("name"),
                             reason, "/dashboard/apps/build",
                             "app-review:%s:%d" % (row["app_id"], now))
    return _json(runtime, {"ok": True, "listing": target,
                           "selfReview": bool(self_review)})


def clean_homepage(value, dev):
    raw = str(value or "").strip()
    if not raw:
        return ""
    cleaned = core.clean_redirect_uri(raw, dev)
    if not cleaned or not re.match(r"^https?://", cleaned):
        return None
    return cleaned
