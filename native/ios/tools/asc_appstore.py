#!/usr/bin/env python3
import os
import sys
import time
import urllib.parse

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from asc_testflight import Session, app_id, wait_for_valid  # noqa: E402

USAGE = """usage:
    asc_appstore.py version XCCONFIG_VERSION
    asc_appstore.py submit VERSION BUILD [WHATS_NEW]
    asc_appstore.py plan VERSION
    asc_appstore.py status"""

EDITABLE = frozenset({
    "PREPARE_FOR_SUBMISSION", "READY_FOR_REVIEW", "DEVELOPER_REJECTED", "REJECTED",
    "METADATA_REJECTED", "INVALID_BINARY",
})
IN_REVIEW = frozenset({"WAITING_FOR_REVIEW", "IN_REVIEW", "WAITING_FOR_EXPORT_COMPLIANCE"})
OPEN_SUBMISSIONS = frozenset({"READY_FOR_REVIEW", "UNRESOLVED_ISSUES"})
ACTIVE_SUBMISSIONS = frozenset({"WAITING_FOR_REVIEW", "IN_REVIEW"})
WHATS_NEW_LIMIT = 4000
REVIEW_NOTES_LIMIT = 4000
REVIEW_NOTES = (
    "ForkMesh for iOS is free and sells nothing. There are no subscriptions, memberships, "
    "paid tiers, credits or other paid digital content in the app, and nothing bought "
    "anywhere else unlocks or changes anything in it: every account gets the same app. "
    "The app shows no prices and has no buttons, links or other calls to action for buying "
    "anything outside the app. Agent usage and server costs shown in the app are the user's "
    "own accounts with other companies (such as Anthropic and Vultr), reported by the user's "
    "own computer; ForkMesh does not sell or resell them."
)
HEALTH_REVIEW_NOTES = (
    "HealthKit: seven optional tools use Apple Health (Pedometer, Water Intake, Calorie Counter, "
    "Sleep, Sugar Break, Stretch Break and Food Guide). They are identified in the app in two places. "
    "1) Settings > Legal > Apple Health opens a screen titled Apple Health that says the app "
    "uses Apple's HealthKit and lists, tool by tool, what is read from and written to Apple "
    "Health. 2) After installing a tool from Market, its screen has a card marked 'Works with "
    "Apple Health' beside the Apple Health switch, with the data it reads and writes and a "
    "'How ForkMesh uses Apple Health' button. Apple Health data is never used for advertising "
    "and is described at https://forkmesh.com/privacy#apple-health."
)


def parse(version: str) -> tuple[int, ...]:
    parts = version.strip().split(".")
    if not 1 <= len(parts) <= 3 or not all(part.isdigit() for part in parts):
        raise SystemExit(f"ERROR: {version!r} is not a MAJOR.MINOR.PATCH version.")
    numbers = [int(part) for part in parts]
    return tuple(numbers + [0] * (3 - len(numbers)))


def show(version: tuple[int, ...]) -> str:
    return ".".join(str(part) for part in version)


def state(row: dict) -> str:
    attributes = row.get("attributes", {})
    return attributes.get("appVersionState") or attributes.get("appStoreState") or ""


def closed(row: dict) -> bool:
    return state(row) not in EDITABLE | IN_REVIEW


def target_version(wanted: str, versions: list) -> str:
    burned = [parse(row["attributes"]["versionString"]) for row in versions if closed(row)]
    ceiling = max(burned, default=(0, 0, 0))
    if parse(wanted) > ceiling:
        return show(parse(wanted))
    return show((ceiling[0], ceiling[1], ceiling[2] + 1))


def in_flight(versions: list):
    rows = [row for row in versions if not closed(row)]
    rows.sort(key=lambda row: row["attributes"].get("createdDate", ""), reverse=True)
    return rows[0] if rows else None


def plan(version: str, versions: list) -> list:
    steps = []
    same = [row for row in versions if row["attributes"]["versionString"] == version]
    if any(closed(row) for row in same):
        raise SystemExit(f"ERROR: App Store version {version} is already {state(same[0])}; "
                         "a new release needs a higher version.")
    current = in_flight(versions)
    if current is None:
        return [("create", version)]
    name = current["attributes"]["versionString"]
    if state(current) in IN_REVIEW:
        steps.append(("cancel review", name))
    if name != version:
        steps.append(("rename", f"{name} -> {version}"))
    steps.append(("reuse", current["id"]))
    return steps


def ios_versions(api, app: str) -> list:
    query = urllib.parse.urlencode({"filter[platform]": "IOS", "limit": "200"})
    return api("GET", f"/v1/apps/{app}/appStoreVersions?{query}").get("data", [])


def submissions(api, app: str, states) -> list:
    query = urllib.parse.urlencode({
        "filter[app]": app, "filter[platform]": "IOS", "filter[state]": ",".join(sorted(states)),
        "limit": "50",
    })
    return api("GET", f"/v1/reviewSubmissions?{query}").get("data", [])


def cancel_review(api, app: str, version_id: str) -> None:
    for row in submissions(api, app, ACTIVE_SUBMISSIONS):
        api("PATCH", f"/v1/reviewSubmissions/{row['id']}",
            {"data": {"type": "reviewSubmissions", "id": row["id"],
                      "attributes": {"canceled": True}}})
        print(f"Cancelled review submission {row['id']} ({row['attributes'].get('state')}).")
    limit = time.time() + 15 * 60
    while True:
        row = api("GET", f"/v1/appStoreVersions/{version_id}").get("data", {})
        if state(row) in EDITABLE:
            return
        if time.time() > limit:
            raise SystemExit(f"ERROR: the version is still {state(row)} after cancelling its review.")
        time.sleep(20)


def prepare_version(api, app: str, version: str) -> str:
    versions = ios_versions(api, app)
    steps = plan(version, versions)
    for action, detail in steps:
        print(f"App Store version: {action} {detail}")
    current = in_flight(versions)
    if current is None:
        created = api("POST", "/v1/appStoreVersions", {"data": {
            "type": "appStoreVersions",
            "attributes": {"platform": "IOS", "versionString": version,
                           "releaseType": "AFTER_APPROVAL"},
            "relationships": {"app": {"data": {"type": "apps", "id": app}}},
        }})
        return created["data"]["id"]
    if state(current) in IN_REVIEW:
        cancel_review(api, app, current["id"])
    api("PATCH", f"/v1/appStoreVersions/{current['id']}", {"data": {
        "type": "appStoreVersions", "id": current["id"],
        "attributes": {"versionString": version, "releaseType": "AFTER_APPROVAL"},
    }})
    return current["id"]


def attach_build(api, version_id: str, build_id: str) -> None:
    api("PATCH", f"/v1/appStoreVersions/{version_id}/relationships/build",
        {"data": {"type": "builds", "id": build_id}})


def set_whats_new(api, version_id: str, text: str) -> None:
    text = text.strip()[:WHATS_NEW_LIMIT]
    if not text:
        return
    rows = api("GET", f"/v1/appStoreVersions/{version_id}/appStoreVersionLocalizations?limit=50")
    for row in rows.get("data", []):
        api("PATCH", f"/v1/appStoreVersionLocalizations/{row['id']}", {"data": {
            "type": "appStoreVersionLocalizations", "id": row["id"],
            "attributes": {"whatsNew": text},
        }})
        print(f"What's New set for {row['attributes'].get('locale')}.")


def review_notes(existing: str) -> str:
    existing = (existing or "").strip()
    if REVIEW_NOTES not in existing:
        existing = "\n\n".join(part for part in (REVIEW_NOTES, existing) if part)
    if HEALTH_REVIEW_NOTES not in existing:
        existing = existing.replace(REVIEW_NOTES, REVIEW_NOTES + "\n\n" + HEALTH_REVIEW_NOTES, 1)
    return existing[:REVIEW_NOTES_LIMIT]


def review_detail(api, version_id: str):
    try:
        return api("GET", f"/v1/appStoreVersions/{version_id}/appStoreReviewDetail").get("data")
    except SystemExit as error:
        if " answered 404:" in str(error):
            return None
        raise


def set_review_notes(api, version_id: str) -> None:
    detail = review_detail(api, version_id)
    if detail is None:
        api("POST", "/v1/appStoreReviewDetails", {"data": {
            "type": "appStoreReviewDetails",
            "attributes": {"notes": review_notes("")},
            "relationships": {"appStoreVersion": {
                "data": {"type": "appStoreVersions", "id": version_id}}},
        }})
        print("App Review notes created.")
        return
    current = detail.get("attributes", {}).get("notes") or ""
    notes = review_notes(current)
    if notes == current.strip():
        print("App Review notes are already up to date.")
        return
    api("PATCH", f"/v1/appStoreReviewDetails/{detail['id']}", {"data": {
        "type": "appStoreReviewDetails", "id": detail["id"], "attributes": {"notes": notes},
    }})
    print("App Review notes updated.")


def submission_items(api, submission_id: str) -> list:
    query = urllib.parse.urlencode({"include": "appStoreVersion", "limit": "50"})
    return api("GET", f"/v1/reviewSubmissions/{submission_id}/items?{query}").get("data", [])


def item_version(item: dict) -> str:
    data = item.get("relationships", {}).get("appStoreVersion", {}).get("data") or {}
    return data.get("id", "")


def submit_for_review(api, app: str, version_id: str) -> str:
    open_rows = submissions(api, app, OPEN_SUBMISSIONS)
    if open_rows:
        submission = open_rows[0]["id"]
        print(f"Reusing review submission {submission} ({open_rows[0]['attributes'].get('state')}).")
    else:
        submission = api("POST", "/v1/reviewSubmissions", {"data": {
            "type": "reviewSubmissions", "attributes": {"platform": "IOS"},
            "relationships": {"app": {"data": {"type": "apps", "id": app}}},
        }})["data"]["id"]
        print(f"Created review submission {submission}.")
    items = submission_items(api, submission)
    ours = [item for item in items if item_version(item) == version_id]
    for item in ours:
        if item["attributes"].get("state") not in (None, "READY_FOR_REVIEW"):
            api("PATCH", f"/v1/reviewSubmissionItems/{item['id']}", {"data": {
                "type": "reviewSubmissionItems", "id": item["id"],
                "attributes": {"resolved": True},
            }})
    if not ours:
        api("POST", "/v1/reviewSubmissionItems", {"data": {
            "type": "reviewSubmissionItems",
            "relationships": {
                "reviewSubmission": {"data": {"type": "reviewSubmissions", "id": submission}},
                "appStoreVersion": {"data": {"type": "appStoreVersions", "id": version_id}},
            },
        }})
    done = api("PATCH", f"/v1/reviewSubmissions/{submission}", {"data": {
        "type": "reviewSubmissions", "id": submission, "attributes": {"submitted": True},
    }})
    return done.get("data", {}).get("attributes", {}).get("state", "")


def version_command(wanted: str) -> None:
    api = Session()
    print(target_version(wanted, ios_versions(api, app_id(api))))


def plan_command(version: str) -> None:
    api = Session()
    for action, detail in plan(version, ios_versions(api, app_id(api))):
        print(f"{action}: {detail}")


def status_command() -> None:
    api = Session()
    app = app_id(api)
    for row in ios_versions(api, app):
        print(f"{row['attributes']['versionString']}: {state(row)} "
              f"(release {row['attributes'].get('releaseType')})")
    for row in submissions(api, app, OPEN_SUBMISSIONS | ACTIVE_SUBMISSIONS):
        print(f"review submission {row['id']}: {row['attributes'].get('state')}")


def submit_command(version: str, build: str, whats_new: str) -> None:
    api = Session()
    app = app_id(api)
    row, _ = wait_for_valid(api, app, version, build)
    version_id = prepare_version(api, app, version)
    attach_build(api, version_id, row["id"])
    print(f"Build {version} ({build}) attached.")
    set_whats_new(api, version_id, whats_new or f"ForkMesh {version}.")
    set_review_notes(api, version_id)
    result = submit_for_review(api, app, version_id)
    print(f"Submitted {version} ({build}) for App Review: {result or 'submitted'}; "
          "it goes live when Apple approves it.")


if __name__ == "__main__":
    args = sys.argv[1:]
    if len(args) == 2 and args[0] == "version":
        version_command(args[1])
    elif len(args) == 2 and args[0] == "plan":
        plan_command(args[1])
    elif len(args) == 1 and args[0] == "status":
        status_command()
    elif len(args) in (3, 4) and args[0] == "submit":
        submit_command(args[1], args[2], args[3] if len(args) == 4 else "")
    else:
        raise SystemExit(USAGE)
