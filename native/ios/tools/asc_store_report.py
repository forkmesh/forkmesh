#!/usr/bin/env python3
import os
import sys
import urllib.parse

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from asc_appstore import ios_versions, item_version, state, submission_items  # noqa: E402
from asc_testflight import Session, app_id  # noqa: E402

USAGE = "usage: asc_store_report.py [--feedback N]"
VERSIONS_SHOWN = 4
SUBMISSIONS_SHOWN = 4
BUILDS_SHOWN = 3
NEEDS_ACTION = frozenset({
    "REJECTED", "METADATA_REJECTED", "INVALID_BINARY", "DEVELOPER_REJECTED",
    "UNRESOLVED_ISSUES", "MISSING_EXPORT_COMPLIANCE",
})


def get(api, path: str, query: dict | None = None) -> dict:
    return api("GET", path + ("?" + urllib.parse.urlencode(query) if query else ""))


def optional(api, path: str, query: dict | None = None):
    try:
        return get(api, path, query)
    except SystemExit as error:
        print(f"  (unavailable: {str(error)[:200]})")
        return None


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


def heading(title: str) -> None:
    print(f"\n== {title} ==")


def app_store(api, app: str) -> None:
    heading("App Store versions")
    versions = sorted(ios_versions(api, app),
                      key=lambda row: row["attributes"].get("createdDate", ""), reverse=True)
    if not versions:
        print("  none")
    for row in versions[:VERSIONS_SHOWN]:
        a = row["attributes"]
        build = optional(api, f"/v1/appStoreVersions/{row['id']}/build") or {}
        number = ((build.get("data") or {}).get("attributes") or {}).get("version", "no build")
        print(f"  {a['versionString']} (build {number}): {flag(state(row))}"
              f"; release {a.get('releaseType')}; created {day(a.get('createdDate'))}")

    heading("App Review submissions")
    found = get(api, "/v1/reviewSubmissions",
                {"filter[app]": app, "filter[platform]": "IOS", "limit": "50"})
    rows = sorted(found.get("data", []),
                  key=lambda row: row["attributes"].get("submittedDate") or "", reverse=True)
    names = {row["id"]: row["attributes"]["versionString"] for row in versions}
    if not rows:
        print("  none")
    for row in rows[:SUBMISSIONS_SHOWN]:
        a = row["attributes"]
        print(f"  {row['id']}: {flag(a.get('state', ''))}; submitted {day(a.get('submittedDate'))}")
        for item in submission_items(api, row["id"]):
            name = names.get(item_version(item), "other item")
            print(f"    {name}: {flag(item['attributes'].get('state') or '')}")
    if any(row["attributes"].get("state") == "UNRESOLVED_ISSUES" for row in rows) or any(
            state(row) in NEEDS_ACTION for row in versions[:1]):
        print("  Apple's message is in App Store Connect -> App Review (Resolution Center); "
              "the API does not return its text.")


def testflight(api, app: str) -> None:
    heading("TestFlight builds")
    found = get(api, "/v1/builds", {
        "filter[app]": app, "sort": "-uploadedDate", "limit": str(BUILDS_SHOWN),
        "include": "preReleaseVersion,buildBetaDetail,betaAppReviewSubmission,betaGroups",
    })
    extra = included(found)
    if not found.get("data"):
        print("  none")
    for row in found.get("data", []):
        a = row["attributes"]
        version = (related(row, "preReleaseVersion", extra) or {}).get("attributes", {})
        detail = (related(row, "buildBetaDetail", extra) or {}).get("attributes", {})
        review = (related(row, "betaAppReviewSubmission", extra) or {}).get("attributes", {})
        groups = [g["attributes"]["name"] for g in related(row, "betaGroups", extra) or []]
        print(f"  {version.get('version', '?')} ({a.get('version')}): {a.get('processingState')}"
              f"; uploaded {day(a.get('uploadedDate'))}"
              f"{'; EXPIRED' if a.get('expired') else ''}")
        print(f"    internal {flag(detail.get('internalBuildState') or '?')}"
              f"; external {flag(detail.get('externalBuildState') or '?')}"
              f"; beta review {review.get('betaReviewState') or 'not submitted'}")
        print(f"    in groups: {', '.join(groups) or 'none'}")

    heading("TestFlight groups")
    groups = get(api, f"/v1/apps/{app}/betaGroups", {"limit": "200"}).get("data", [])
    if not groups:
        print("  none")
    for row in sorted(groups, key=lambda g: (not g["attributes"].get("isInternalGroup"),
                                             g["attributes"]["name"])):
        a = row["attributes"]
        testers = get(api, f"/v1/betaGroups/{row['id']}/betaTesters", {"limit": "1"})
        count = testers.get("meta", {}).get("paging", {}).get("total", len(testers.get("data", [])))
        kind = "internal" if a.get("isInternalGroup") else "external"
        notes = [f"{count} tester{'s' if count != 1 else ''}"]
        notes.append("every build" if a.get("hasAccessToAllBuilds") else "chosen builds")
        if not a.get("isInternalGroup"):
            if a.get("publicLinkEnabled"):
                limit = (f", limit {a.get('publicLinkLimit')}"
                         if a.get("publicLinkLimitEnabled") else "")
                notes.append(f"public link ON {a.get('publicLink') or ''}{limit}".rstrip())
            else:
                notes.append("public link off")
        notes.append("feedback on" if a.get("feedbackEnabled") else "feedback off")
        print(f"  {a['name']} ({kind}): {'; '.join(notes)}")


def feedback(api, app: str, shown: int) -> None:
    heading("TestFlight tester feedback")
    entries = []
    for kind in ("betaFeedbackScreenshotSubmissions", "betaFeedbackCrashSubmissions"):
        found = optional(api, f"/v1/apps/{app}/{kind}", {"sort": "-createdDate", "limit": str(shown)})
        for row in (found or {}).get("data", []):
            entries.append(("crash" if "Crash" in kind else "screenshot", row["attributes"]))
    entries.sort(key=lambda entry: entry[1].get("createdDate", ""), reverse=True)
    if not entries:
        print("  none")
    for kind, a in entries[:shown]:
        who = a.get("email") or "anonymous"
        device = " ".join(filter(None, [a.get("deviceModel"), a.get("osVersion")]))
        comment = " ".join((a.get("comment") or "").split()) or "(no comment)"
        print(f"  {day(a.get('createdDate'))} {kind} from {who} ({device or '?'}): {comment[:300]}")

    heading("App Store customer reviews")
    found = optional(api, f"/v1/apps/{app}/customerReviews", {"sort": "-createdDate", "limit": str(shown)})
    rows = (found or {}).get("data", [])
    if found is not None and not rows:
        print("  none")
    for row in rows:
        a = row["attributes"]
        body = " ".join((a.get("body") or "").split())
        print(f"  {day(a.get('createdDate'))} {'*' * int(a.get('rating') or 0)} "
              f"{a.get('title') or ''} ({a.get('territory')}): {body[:300]}")


def main(args: list[str]) -> None:
    shown = 5
    if args[:1] == ["--feedback"] and len(args) == 2 and args[1].isdigit():
        shown = int(args[1])
    elif args:
        raise SystemExit(USAGE)
    api = Session()
    app = app_id(api)
    print(f"App Store Connect app {app}")
    app_store(api, app)
    testflight(api, app)
    feedback(api, app, shown)


if __name__ == "__main__":
    main(sys.argv[1:])
