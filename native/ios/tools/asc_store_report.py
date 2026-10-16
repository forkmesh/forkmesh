#!/usr/bin/env python3
from __future__ import annotations

import base64
import csv
import gzip
import hashlib
import io
import json
import os
import sys
import time
import urllib.parse
import urllib.request
from collections import Counter
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime, timedelta, timezone
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from asc_appstore import ios_versions, item_version, state, submission_items  # noqa: E402
from asc_bundle_ids import APP_BUNDLE_ID, action_variables  # noqa: E402
from asc_testflight import Session, app_id  # noqa: E402

USAGE = "usage: asc_store_report.py [--feedback N] [--json] [--publish] [--marketing ORG]"
FORMAT = "forkmesh-app-store-status-v1"
PUBLISH_PREFIX = "/api/app-store-status"
PUBLISH_PROOF = "forkmesh-app-store-status-request-v1"
PAGE_PATH = "/dashboard/app-store-status"
TOOLS_DIR = Path(__file__).resolve().parents[3] / "tools"
VERSIONS_SHOWN = 4
SUBMISSIONS_SHOWN = 4
BUILDS_SHOWN = 3
NEEDS_ACTION = frozenset({
    "REJECTED", "METADATA_REJECTED", "INVALID_BINARY", "DEVELOPER_REJECTED",
    "UNRESOLVED_ISSUES", "MISSING_EXPORT_COMPLIANCE",
})
PENDING = frozenset({
    "WAITING_FOR_REVIEW", "IN_REVIEW", "READY_FOR_REVIEW", "PROCESSING", "PROCESSING_EXCEPTION",
    "WAITING_FOR_BETA_REVIEW", "IN_BETA_REVIEW", "WAITING_FOR_EXPORT_COMPLIANCE",
    "PENDING_DEVELOPER_RELEASE", "PENDING_APPLE_RELEASE", "READY_FOR_BETA_SUBMISSION",
    "WAITING_FOR_BETA_SUBMISSION", "ACCEPTED", "COMPLETING",
})
GOOD = frozenset({
    "READY_FOR_SALE", "READY_FOR_DISTRIBUTION", "VALID", "IN_BETA_TESTING", "BETA_APPROVED",
    "APPROVED", "COMPLETE", "READY_FOR_TESTING", "PROCESSING_DONE",
})
LABELS = {
    "READY_FOR_SALE": "Ready for sale",
    "READY_FOR_DISTRIBUTION": "Ready for distribution",
    "IN_BETA_TESTING": "In beta testing",
    "UNRESOLVED_ISSUES": "Unresolved issues",
}
PRODUCT_KINDS = {
    "1": "downloads", "1F": "downloads", "1T": "downloads",
    "3": "redownloads", "3F": "redownloads", "3T": "redownloads",
    "7": "updates", "7F": "updates", "7T": "updates",
}
KINDS = ("downloads", "redownloads", "updates")
MONTHLY_REPORT_DELAY = timedelta(days=7)
RECENT_DAYS = (7, 30)
SCREENSHOTS_SHOWN = 4
SCREENSHOT_MAX_BYTES = 3 * 1024 * 1024
SCREENSHOT_SIDES = (2048, 1600, 1280, 1024)
SCREENSHOT_QUALITIES = (82, 70, 55)
MARKETING_PREFIX = "/api/marketing"
MARKETING_BACKFILL_DAYS = 365
MARKETING_OVERLAP_DAYS = 3
MARKETING_PENDING_DAYS = 2
MARKETING_BATCH_DAYS = 120


def get(api, path: str, query: dict | None = None) -> dict:
    return api("GET", path + ("?" + urllib.parse.urlencode(query) if query else ""))


def optional(api, problems: list, path: str, query: dict | None = None):
    try:
        return get(api, path, query)
    except SystemExit as error:
        problems.append(str(error)[:200])
        return None


def status(value: str | None, empty: str = "Unknown") -> dict:
    value = value or ""
    if value in NEEDS_ACTION:
        tone = "action"
    elif value in PENDING:
        tone = "pending"
    elif value in GOOD:
        tone = "good"
    else:
        tone = "neutral"
    label = LABELS.get(value) or (value.replace("_", " ").capitalize() if value else empty)
    return {"state": value, "label": label, "tone": tone}


def flag(value: str) -> str:
    return f"{value}  <-- needs action" if value in NEEDS_ACTION else value


def day(stamp: str | None) -> str:
    return (stamp or "")[:16].replace("T", " ") or "?"


def included(found: dict) -> dict:
    return {(row["type"], row["id"]): row for row in found.get("included", [])}


def related(row: dict, name: str, extra: dict):
    data = row.get("relationships", {}).get(name, {}).get("data")
    if isinstance(data, list):
        return [extra[(ref["type"], ref["id"])] for ref in data if (ref["type"], ref["id"]) in extra]
    if data:
        return extra.get((data["type"], data["id"]))
    return None


def unavailable(snapshot: dict, section: str, problems: list) -> None:
    if problems:
        snapshot["unavailable"][section] = problems[0]


def app_store(api, app: str, snapshot: dict) -> list:
    versions = sorted(ios_versions(api, app),
                      key=lambda row: row["attributes"].get("createdDate", ""), reverse=True)
    problems = []
    for row in versions[:VERSIONS_SHOWN]:
        a = row["attributes"]
        build = optional(api, problems, f"/v1/appStoreVersions/{row['id']}/build") or {}
        number = ((build.get("data") or {}).get("attributes") or {}).get("version", "")
        snapshot["versions"].append({
            "version": a.get("versionString") or "", "build": number,
            "status": status(state(row)), "releaseType": a.get("releaseType") or "",
            "createdAt": a.get("createdDate") or ""})
    unavailable(snapshot, "versions", problems)

    found = get(api, "/v1/reviewSubmissions",
                {"filter[app]": app, "filter[platform]": "IOS", "limit": "50"})
    rows = sorted(found.get("data", []),
                  key=lambda row: row["attributes"].get("submittedDate") or "", reverse=True)
    names = {row["id"]: row["attributes"]["versionString"] for row in versions}
    for row in rows[:SUBMISSIONS_SHOWN]:
        a = row["attributes"]
        items = [{"name": names.get(item_version(item), "other item"),
                  "status": status(item["attributes"].get("state"))}
                 for item in submission_items(api, row["id"])]
        snapshot["submissions"].append({"id": row["id"], "status": status(a.get("state")),
                                        "submittedAt": a.get("submittedDate") or "",
                                        "items": items})
    latest = versions[:1]
    if latest and state(latest[0]) in NEEDS_ACTION:
        a = latest[0]["attributes"]
        snapshot["attention"].append(
            f"{a.get('versionString')} is {status(state(latest[0]))['label'].lower()}: Apple's "
            "message is in App Store Connect -> App Review (Resolution Center)")
    if any(row["attributes"].get("state") == "UNRESOLVED_ISSUES" for row in rows):
        snapshot["attention"].append(
            "An App Review submission has unresolved issues: reply in App Store Connect -> "
            "App Review (Resolution Center); the API does not return Apple's text")
    return versions


def vendor_number(environ=os.environ) -> str:
    return environ.get("ASC_VENDOR_NUMBER", "").strip() or action_variables().get("ASC_VENDOR_NUMBER", "")


def report(api, vendor: str, app: str, frequency: str, date: str) -> dict:
    cache = api.__dict__.setdefault("sales_reports", {})
    key = (vendor, app, frequency, date)
    if key in cache:
        return cache[key]
    query = {
        "filter[frequency]": frequency, "filter[reportType]": "SALES",
        "filter[reportSubType]": "SUMMARY", "filter[version]": "1_0",
        "filter[vendorNumber]": vendor, "filter[reportDate]": date,
    }
    raw = api.download("/v1/salesReports?" + urllib.parse.urlencode(query))
    found = {"available": raw is not None, "counts": Counter(),
             "territories": Counter(), "devices": Counter()}
    for row in csv.DictReader(io.StringIO(gzip.decompress(raw).decode("utf-8") if raw else ""),
                              delimiter="\t"):
        kind = PRODUCT_KINDS.get((row.get("Product Type Identifier") or "").strip())
        if not kind or (row.get("Apple Identifier") or "").strip() != app:
            continue
        count = int(float(row.get("Units") or 0))
        found["counts"][kind] += count
        if kind == "downloads":
            found["territories"][(row.get("Country Code") or "").strip().upper()] += count
            found["devices"][(row.get("Device") or "").strip()] += count
    for part in ("territories", "devices"):
        found[part].pop("", None)
    cache[key] = found
    return found


def units(api, vendor: str, app: str, frequency: str, date: str) -> Counter:
    return Counter(report(api, vendor, app, frequency, date)["counts"])


def next_month(month):
    return (month.replace(day=1) + timedelta(days=32)).replace(day=1)


def counts(found: Counter) -> dict:
    return {kind: found[kind] for kind in KINDS}


def downloads(api, app: str, versions: list, vendor: str, today) -> dict:
    result = {"available": False, "note": "", "since": "", "total": counts(Counter()),
              "recent": []}
    if not vendor:
        result["note"] = ("set ASC_VENDOR_NUMBER (App Store Connect -> Payments and Financial "
                          "Reports, the number under the account name) in the environment or "
                          "the desktop's Actions variables")
        return result
    created = [row["attributes"].get("createdDate") for row in versions]
    created = [stamp[:10] for stamp in created if stamp]
    if not created:
        result["note"] = "none: the app has no App Store version yet"
        return result
    first = datetime.strptime(min(created), "%Y-%m-%d").date().replace(day=1)
    try:
        total = Counter()
        month = first
        while next_month(month) + MONTHLY_REPORT_DELAY <= today:
            total += units(api, vendor, app, "MONTHLY", month.strftime("%Y-%m"))
            month = next_month(month)
        start = min(month, today - timedelta(days=max(RECENT_DAYS)))
        days = [start + timedelta(days=n) for n in range((today - start).days)]
        with ThreadPoolExecutor(8) as pool:
            daily = dict(zip(days, pool.map(
                lambda when: units(api, vendor, app, "DAILY", when.isoformat()), days)))
    except SystemExit as error:
        result["error"] = str(error)[:200]
        return result
    total += sum((found for when, found in daily.items() if when >= month), Counter())
    result.update(available=True, since=first.strftime("%Y-%m"), total=counts(total))
    for span in RECENT_DAYS:
        recent = sum((found for when, found in daily.items()
                      if when >= today - timedelta(days=span)), Counter())
        result["recent"].append({"days": span, **counts(recent)})
    return result


def marketing_start(stored_last: str, since: str, today):
    start = today - timedelta(days=MARKETING_BACKFILL_DAYS)
    try:
        start = max(start, datetime.strptime(since + "-01", "%Y-%m-%d").date())
    except ValueError:
        pass
    try:
        return max(start, datetime.strptime(stored_last, "%Y-%m-%d").date()
                   - timedelta(days=MARKETING_OVERLAP_DAYS))
    except ValueError:
        return start


def marketing_days(api, vendor: str, app: str, start, end, today) -> list:
    days = [start + timedelta(days=n) for n in range((end - start).days + 1)]
    with ThreadPoolExecutor(8) as pool:
        found = list(pool.map(lambda when: report(api, vendor, app, "DAILY", when.isoformat()),
                              days))
    while days and not found[-1]["available"] and (
            today - days[-1]).days < MARKETING_PENDING_DAYS:
        days.pop()
        found.pop()
    return [{"day": when.isoformat(), **counts(item["counts"]),
             "territories": {key: value for key, value in item["territories"].items() if value > 0},
             "devices": {key: value for key, value in item["devices"].items() if value > 0}}
            for when, item in zip(days, found)]


def marketing_request(method: str, path: str, body=None) -> dict:
    if str(TOOLS_DIR) not in sys.path:
        sys.path.insert(0, str(TOOLS_DIR))
    import forkmesh_mcp_server
    return forkmesh_mcp_server.marketing_request(method, path, body)


def marketing_organization(organization: str) -> str:
    if str(TOOLS_DIR) not in sys.path:
        sys.path.insert(0, str(TOOLS_DIR))
    import forkmesh_mcp_server
    return forkmesh_mcp_server.marketing_organization(organization or None)


def publish_marketing(snapshot: dict, api, app: str, vendor: str, today, organization: str = "",
                      request=None) -> bool:
    if not vendor:
        print("\nnot sent to Marketing: set ASC_VENDOR_NUMBER to read daily downloads")
        return False
    days = []
    try:
        if request is None:
            request = marketing_request
            organization = marketing_organization(organization)
        path = f"{MARKETING_PREFIX}/{organization}/app-store"
        stored = request("GET", path).get("appStore") or {}
        last = stored.get("lastDay") or "" if (stored.get("app") or {}).get("id") == app else ""
        start = marketing_start(last, snapshot["downloads"].get("since", ""), today)
        end = today - timedelta(days=1)
        days = marketing_days(api, vendor, app, start, end, today) if start <= end else []
        body = {"app": {"id": app, "name": snapshot["app"].get("name", ""),
                        "bundleId": snapshot["app"].get("bundleId", APP_BUNDLE_ID)},
                "versions": snapshot["versions"], "reviews": snapshot["reviews"]}
        result = {}
        for index in range(0, max(1, len(days)), MARKETING_BATCH_DAYS):
            result = request("PUT", path, {**body, "days": days[index:index + MARKETING_BATCH_DAYS]})
    except (Exception, SystemExit) as error:
        print(f"\nnot sent to Marketing: {error}")
        return False
    summary = result.get("appStore") or {}
    month = next((span for span in summary.get("windows", []) if span.get("days") == 30), {})
    print(f"\nsent {len(days)} days of App Store downloads to Marketing ({organization}): "
          f"{month.get('downloads', 0)} downloads in the 30 days to "
          f"{summary.get('lastDay') or 'no reported day yet'}")
    return True


def testflight(api, app: str, snapshot: dict) -> None:
    found = get(api, "/v1/builds", {
        "filter[app]": app, "sort": "-uploadedDate", "limit": str(BUILDS_SHOWN),
        "include": "preReleaseVersion,buildBetaDetail,betaAppReviewSubmission,betaGroups",
    })
    extra = included(found)
    for row in found.get("data", []):
        a = row["attributes"]
        version = (related(row, "preReleaseVersion", extra) or {}).get("attributes", {})
        detail = (related(row, "buildBetaDetail", extra) or {}).get("attributes", {})
        review = (related(row, "betaAppReviewSubmission", extra) or {}).get("attributes", {})
        groups = [g["attributes"]["name"] for g in related(row, "betaGroups", extra) or []]
        snapshot["builds"].append({
            "version": version.get("version") or "", "build": a.get("version") or "",
            "processing": status(a.get("processingState")),
            "uploadedAt": a.get("uploadedDate") or "", "expired": bool(a.get("expired")),
            "internal": status(detail.get("internalBuildState")),
            "external": status(detail.get("externalBuildState")),
            "betaReview": status(review.get("betaReviewState"), "Not submitted"),
            "groups": groups})

    groups = get(api, f"/v1/apps/{app}/betaGroups", {"limit": "200"}).get("data", [])
    for row in sorted(groups, key=lambda g: (not g["attributes"].get("isInternalGroup"),
                                             g["attributes"]["name"])):
        a = row["attributes"]
        testers = get(api, f"/v1/betaGroups/{row['id']}/betaTesters", {"limit": "1"})
        count = testers.get("meta", {}).get("paging", {}).get("total", len(testers.get("data", [])))
        internal = bool(a.get("isInternalGroup"))
        linked = not internal and bool(a.get("publicLinkEnabled"))
        snapshot["groups"].append({
            "name": a.get("name") or "", "internal": internal, "testers": int(count or 0),
            "allBuilds": bool(a.get("hasAccessToAllBuilds")), "publicLinkEnabled": linked,
            "publicLink": (a.get("publicLink") or "") if linked else "",
            "publicLinkLimit": int(a.get("publicLinkLimit") or 0)
            if linked and a.get("publicLinkLimitEnabled") else 0,
            "feedbackEnabled": bool(a.get("feedbackEnabled"))})


def feedback(api, app: str, shown: int, snapshot: dict) -> None:
    entries = []
    problems = []
    for kind in ("betaFeedbackScreenshotSubmissions", "betaFeedbackCrashSubmissions"):
        found = optional(api, problems, f"/v1/apps/{app}/{kind}",
                         {"sort": "-createdDate", "limit": str(shown)})
        for row in (found or {}).get("data", []):
            entries.append(("crash" if "Crash" in kind else "screenshot", row))
    unavailable(snapshot, "feedback", problems)
    entries.sort(key=lambda entry: entry[1]["attributes"].get("createdDate", ""), reverse=True)
    for kind, row in entries[:shown]:
        a = row["attributes"]
        snapshot["feedback"].append({
            "kind": kind, "createdAt": a.get("createdDate") or "",
            "from": a.get("email") or "anonymous",
            "device": " ".join(filter(None, [a.get("deviceModel"), a.get("osVersion")])),
            "comment": " ".join((a.get("comment") or "").split())[:300],
            "screenshots": screenshots(row)})

    problems = []
    found = optional(api, problems, f"/v1/apps/{app}/customerReviews",
                     {"sort": "-createdDate", "limit": str(shown)})
    unavailable(snapshot, "reviews", problems)
    for row in (found or {}).get("data", []):
        a = row["attributes"]
        snapshot["reviews"].append({
            "createdAt": a.get("createdDate") or "", "rating": int(a.get("rating") or 0),
            "title": a.get("title") or "", "territory": a.get("territory") or "",
            "body": " ".join((a.get("body") or "").split())[:300]})


def screenshot_id(row: dict, index: int) -> str:
    a = row.get("attributes") or {}
    source = row.get("id") or f"{a.get('createdDate')}|{a.get('email')}|{a.get('comment')}"
    return hashlib.sha256(f"{source}|{index}".encode()).hexdigest()[:24]


def screenshots(row: dict) -> list:
    found = []
    for index, shot in enumerate((row.get("attributes") or {}).get("screenshots") or []):
        if len(found) >= SCREENSHOTS_SHOWN:
            break
        if not isinstance(shot, dict) or not str(shot.get("url") or "").startswith("https://"):
            continue
        found.append({"id": screenshot_id(row, index), "width": int(shot.get("width") or 0),
                      "height": int(shot.get("height") or 0), "url": shot["url"]})
    return found


def public(snapshot: dict) -> dict:
    copy = json.loads(json.dumps(snapshot))
    for row in copy.get("feedback", []):
        for shot in row.get("screenshots", []):
            shot.pop("url", None)
    return copy


def screenshot_sources(snapshot: dict) -> dict:
    return {shot["id"]: shot["url"] for row in snapshot.get("feedback", [])
            for shot in row.get("screenshots", []) if shot.get("url")}


def download(url: str) -> bytes:
    with urllib.request.urlopen(url, timeout=60) as response:
        return response.read(32 * 1024 * 1024)


def image_mime(raw: bytes) -> str:
    if raw.startswith(b"\xff\xd8\xff"):
        return "image/jpeg"
    if raw.startswith(b"\x89PNG\r\n\x1a\n"):
        return "image/png"
    return ""


def shrink(raw: bytes) -> tuple[bytes, str]:
    try:
        from PIL import Image
    except ImportError:
        mime = image_mime(raw)
        return (raw, mime) if mime and len(raw) <= SCREENSHOT_MAX_BYTES else (b"", "")
    try:
        image = Image.open(io.BytesIO(raw))
        image.load()
    except (OSError, ValueError):
        return b"", ""
    image = image.convert("RGB")
    for side in SCREENSHOT_SIDES:
        sized = image.copy()
        sized.thumbnail((side, side))
        for quality in SCREENSHOT_QUALITIES:
            out = io.BytesIO()
            sized.save(out, "JPEG", quality=quality, optimize=True)
            if out.tell() <= SCREENSHOT_MAX_BYTES:
                return out.getvalue(), "image/jpeg"
    return b"", ""


def upload_screenshots(snapshot: dict, missing: list, request, fetch=download) -> tuple[int, list]:
    sources = screenshot_sources(snapshot)
    sizes = {shot["id"]: shot for row in snapshot.get("feedback", [])
             for shot in row.get("screenshots", [])}
    sent, problems = 0, []
    for shot_id in missing:
        url = sources.get(shot_id)
        if not url:
            continue
        try:
            raw, mime = shrink(fetch(url))
        except (OSError, ValueError) as error:
            problems.append(f"screenshot {shot_id[:8]} could not be downloaded: {error}")
            continue
        if not raw:
            problems.append(f"screenshot {shot_id[:8]} is not a picture that fits in 3 MB")
            continue
        try:
            request("App Store Status", PUBLISH_PREFIX, PUBLISH_PROOF, "PUT",
                    f"{PUBLISH_PREFIX}/screenshots/{shot_id}",
                    {"mime": mime, "width": sizes[shot_id]["width"],
                     "height": sizes[shot_id]["height"],
                     "data": base64.b64encode(raw).decode("ascii")})
        except Exception as error:
            problems.append(f"screenshot {shot_id[:8]} was not uploaded: {error}")
            continue
        sent += 1
    return sent, problems


def collect(api, app: str, shown: int, vendor: str, today) -> dict:
    snapshot = {"format": FORMAT, "generatedAt": int(time.time() * 1000),
                "app": {"id": app, "bundleId": APP_BUNDLE_ID}, "attention": [],
                "versions": [], "submissions": [], "downloads": {}, "builds": [],
                "groups": [], "feedback": [], "reviews": [], "unavailable": {}}
    versions = app_store(api, app, snapshot)
    snapshot["downloads"] = downloads(api, app, versions, vendor, today)
    if snapshot["downloads"].get("error"):
        snapshot["unavailable"]["downloads"] = snapshot["downloads"].pop("error")
    testflight(api, app, snapshot)
    feedback(api, app, shown, snapshot)
    for build in snapshot["builds"][:1]:
        for key in ("internal", "external"):
            if build[key]["tone"] == "action":
                snapshot["attention"].append(
                    f"TestFlight build {build['version']} ({build['build']}): "
                    f"{key} testing is {build[key]['label'].lower()}")
    return snapshot


def heading(title: str) -> None:
    print(f"\n== {title} ==")


def problem(snapshot: dict, section: str) -> bool:
    message = snapshot["unavailable"].get(section)
    if message:
        print(f"  (unavailable: {message})")
    return bool(message)


def tally(found: dict) -> str:
    return (f"{found['downloads']} downloads, {found['redownloads']} redownloads, "
            f"{found['updates']} updates")


def render_downloads(found: dict, error: str = "") -> None:
    heading("Downloads")
    if error:
        print(f"  (unavailable: {error})")
        return
    if not found["available"]:
        print(f"  {found['note']}")
        return
    print(f"  since {found['since']}: {tally(found['total'])}")
    for recent in sorted(found["recent"], key=lambda span: span["days"]):
        print(f"  last {recent['days']} days: {tally(recent)}")
    print("  (App Store Connect Sales reports; a day's report arrives the next morning, Pacific time)")


def render(snapshot: dict) -> None:
    print(f"App Store Connect app {snapshot['app']['id']}")
    heading("App Store versions")
    problem(snapshot, "versions")
    if not snapshot["versions"]:
        print("  none")
    for row in snapshot["versions"]:
        print(f"  {row['version']} (build {row['build'] or 'no build'}): "
              f"{flag(row['status']['state'])}; release {row['releaseType'] or None}"
              f"; created {day(row['createdAt'])}")

    heading("App Review submissions")
    if not snapshot["submissions"]:
        print("  none")
    for row in snapshot["submissions"]:
        print(f"  {row['id']}: {flag(row['status']['state'])}; submitted {day(row['submittedAt'])}")
        for item in row["items"]:
            print(f"    {item['name']}: {flag(item['status']['state'])}")
    if any("Resolution Center" in line for line in snapshot["attention"]):
        print("  Apple's message is in App Store Connect -> App Review (Resolution Center); "
              "the API does not return its text.")

    render_downloads(snapshot["downloads"], snapshot["unavailable"].get("downloads", ""))

    heading("TestFlight builds")
    if not snapshot["builds"]:
        print("  none")
    for row in snapshot["builds"]:
        print(f"  {row['version'] or '?'} ({row['build']}): {row['processing']['state']}"
              f"; uploaded {day(row['uploadedAt'])}{'; EXPIRED' if row['expired'] else ''}")
        print(f"    internal {flag(row['internal']['state'] or '?')}"
              f"; external {flag(row['external']['state'] or '?')}"
              f"; beta review {row['betaReview']['state'] or 'not submitted'}")
        print(f"    in groups: {', '.join(row['groups']) or 'none'}")

    heading("TestFlight groups")
    if not snapshot["groups"]:
        print("  none")
    for row in snapshot["groups"]:
        count = row["testers"]
        notes = [f"{count} tester{'s' if count != 1 else ''}"]
        notes.append("every build" if row["allBuilds"] else "chosen builds")
        if not row["internal"]:
            if row["publicLinkEnabled"]:
                limit = f", limit {row['publicLinkLimit']}" if row["publicLinkLimit"] else ""
                notes.append(f"public link ON {row['publicLink']}{limit}".rstrip())
            else:
                notes.append("public link off")
        notes.append("feedback on" if row["feedbackEnabled"] else "feedback off")
        print(f"  {row['name']} ({'internal' if row['internal'] else 'external'}): "
              f"{'; '.join(notes)}")

    heading("TestFlight tester feedback")
    if not problem(snapshot, "feedback") and not snapshot["feedback"]:
        print("  none")
    for row in snapshot["feedback"]:
        count = len(row.get("screenshots") or [])
        pictures = f" [{count} screenshot{'s' if count != 1 else ''}]" if count else ""
        print(f"  {day(row['createdAt'])} {row['kind']} from {row['from']} "
              f"({row['device'] or '?'}): {row['comment'] or '(no comment)'}{pictures}")

    heading("App Store customer reviews")
    if not problem(snapshot, "reviews") and not snapshot["reviews"]:
        print("  none")
    for row in snapshot["reviews"]:
        print(f"  {day(row['createdAt'])} {'*' * row['rating']} {row['title']} "
              f"({row['territory']}): {row['body']}")


def publish(snapshot: dict, request=None, fetch=download) -> bool:
    try:
        if request is None:
            if str(TOOLS_DIR) not in sys.path:
                sys.path.insert(0, str(TOOLS_DIR))
            import forkmesh_mcp_server
            request = forkmesh_mcp_server.package_request
            origin = forkmesh_mcp_server.crm_origin()
        else:
            origin = ""
        answer = request("App Store Status", PUBLISH_PREFIX, PUBLISH_PROOF, "PUT",
                         PUBLISH_PREFIX + "/snapshot", public(snapshot))
    except Exception as error:
        print(f"\nnot published to the App Store Status page: {error}")
        return False
    print(f"\npublished to the App Store Status page: {origin}{PAGE_PATH}")
    missing = (answer or {}).get("missingScreenshots") if isinstance(answer, dict) else None
    if isinstance(missing, list) and missing:
        sent, problems = upload_screenshots(snapshot, [str(item) for item in missing],
                                            request, fetch)
        print(f"uploaded {sent} of {len(missing)} new tester feedback screenshot"
              f"{'s' if len(missing) != 1 else ''}")
        for problem in problems:
            print(f"  {problem}")
    return True


def main(args: list[str]) -> None:
    shown = 5
    as_json = publishing = False
    organization = os.environ.get("FORKMESH_MARKETING_ORG", "").strip()
    rest = list(args)
    while rest:
        arg = rest.pop(0)
        if arg == "--feedback" and rest and rest[0].isdigit():
            shown = int(rest.pop(0))
        elif arg == "--json":
            as_json = True
        elif arg == "--publish":
            publishing = True
        elif arg == "--marketing" and rest:
            organization = rest.pop(0).strip().lower()
        else:
            raise SystemExit(USAGE)
    api = Session()
    app = app_id(api)
    vendor = vendor_number()
    today = datetime.now(timezone.utc).date()
    snapshot = collect(api, app, shown, vendor, today)
    if as_json:
        print(json.dumps(snapshot, indent=2))
    else:
        render(snapshot)
    if publishing:
        publish(snapshot)
        publish_marketing(snapshot, api, app, vendor, today, organization)


if __name__ == "__main__":
    main(sys.argv[1:])
