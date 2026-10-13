#!/usr/bin/env python3
"""Put an uploaded build into TestFlight internal testing (no Beta App Review).

    asc_testflight.py internal VERSION BUILD [WHATS_NEW]
        TESTFLIGHT_INTERNAL_GROUPS (optional, comma separated) limits the groups.
        FORKMESH_TESTFLIGHT_WAIT_MINUTES (default 60) bounds the processing wait.
    asc_testflight.py status VERSION BUILD
"""

import os
import sys
import time
import urllib.parse

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from asc_bundle_ids import APP_BUNDLE_ID, call, token  # noqa: E402


class Session:
    def __init__(self):
        self.bearer = ""
        self.minted = 0.0

    def __call__(self, method, path, body=None):
        if time.time() - self.minted > 480:
            self.bearer = token()
            self.minted = time.time()
        return call(self.bearer, method, path, body)


def app_id(api) -> str:
    query = urllib.parse.urlencode({"filter[bundleId]": APP_BUNDLE_ID, "limit": "200"})
    for row in api("GET", "/v1/apps?" + query).get("data", []):
        if row["attributes"]["bundleId"] == APP_BUNDLE_ID:
            return row["id"]
    raise SystemExit(f"ERROR: App Store Connect has no app for {APP_BUNDLE_ID}.")


def find_build(api, app: str, version: str, build: str):
    query = urllib.parse.urlencode(
        {
            "filter[app]": app,
            "filter[version]": build,
            "filter[preReleaseVersion.version]": version,
            "include": "buildBetaDetail",
            "limit": "10",
        }
    )
    found = api("GET", "/v1/builds?" + query)
    rows = found.get("data", [])
    if not rows:
        return None, None
    detail = next((i for i in found.get("included", []) if i["type"] == "buildBetaDetails"), None)
    return rows[0], detail


def wait_for_valid(api, app: str, version: str, build: str):
    limit = time.time() + 60 * int(os.environ.get("FORKMESH_TESTFLIGHT_WAIT_MINUTES", "60"))
    last = ""
    while True:
        row, detail = find_build(api, app, version, build)
        state = row["attributes"]["processingState"] if row else "NOT_FOUND"
        if state != last:
            print(f"{version} ({build}): {state}", flush=True)
            last = state
        if state == "VALID":
            return row, detail
        if state in ("FAILED", "INVALID"):
            raise SystemExit(f"ERROR: App Store Connect marked {version} ({build}) {state}; see the email Apple sent.")
        if time.time() > limit:
            raise SystemExit(f"ERROR: {version} ({build}) still {state} after the wait; run `status` later.")
        time.sleep(30)


def set_whats_new(api, build_id: str, text: str) -> None:
    text = text.strip()[:4000]
    if not text:
        return
    existing = api("GET", f"/v1/builds/{build_id}/betaBuildLocalizations?limit=50").get("data", [])
    english = next((row for row in existing if row["attributes"].get("locale", "").startswith("en")), None)
    if english:
        api(
            "PATCH",
            f"/v1/betaBuildLocalizations/{english['id']}",
            {"data": {"type": "betaBuildLocalizations", "id": english["id"], "attributes": {"whatsNew": text}}},
        )
    else:
        api(
            "POST",
            "/v1/betaBuildLocalizations",
            {
                "data": {
                    "type": "betaBuildLocalizations",
                    "attributes": {"locale": "en-US", "whatsNew": text},
                    "relationships": {"build": {"data": {"type": "builds", "id": build_id}}},
                }
            },
        )
    print("What to Test set.")


def internal_groups(api, app: str):
    wanted = {g.strip() for g in os.environ.get("TESTFLIGHT_INTERNAL_GROUPS", "").split(",") if g.strip()}
    groups = [
        row
        for row in api("GET", f"/v1/apps/{app}/betaGroups?limit=200").get("data", [])
        if row["attributes"].get("isInternalGroup")
    ]
    if wanted:
        groups = [row for row in groups if row["attributes"]["name"] in wanted]
    return groups


def internal(version: str, build: str, whats_new: str) -> None:
    api = Session()
    app = app_id(api)
    row, _ = wait_for_valid(api, app, version, build)
    build_id = row["id"]
    set_whats_new(api, build_id, whats_new)
    groups = internal_groups(api, app)
    if not groups:
        raise SystemExit("ERROR: the app has no internal TestFlight group (App Store Connect -> TestFlight -> Internal Testing -> +).")
    for group in groups:
        name = group["attributes"]["name"]
        if group["attributes"].get("hasAccessToAllBuilds"):
            print(f"Internal group {name} gets every build.")
            continue
        api(
            "POST",
            f"/v1/betaGroups/{group['id']}/relationships/builds",
            {"data": [{"type": "builds", "id": build_id}]},
        )
        print(f"Added to internal group {name}.")
    for _ in range(20):
        _, detail = find_build(api, app, version, build)
        state = (detail or {}).get("attributes", {}).get("internalBuildState", "")
        if state in ("IN_BETA_TESTING", "READY_FOR_BETA_TESTING"):
            print(f"TestFlight internal testing: {state}")
            return
        if state == "MISSING_EXPORT_COMPLIANCE":
            raise SystemExit("ERROR: MISSING_EXPORT_COMPLIANCE: Info.plist must carry ITSAppUsesNonExemptEncryption.")
        time.sleep(15)
    print(f"WARNING: internal state is {state or 'unknown'}; check App Store Connect -> TestFlight.")


def status(version: str, build: str) -> None:
    api = Session()
    row, detail = find_build(api, app_id(api), version, build)
    if not row:
        print(f"{version} ({build}): not found")
        return
    print(f"{version} ({build}): {row['attributes']['processingState']}", end="")
    if detail:
        a = detail["attributes"]
        print(f"; internal {a.get('internalBuildState')}; external {a.get('externalBuildState')}")
    else:
        print()


if __name__ == "__main__":
    args = sys.argv[1:]
    if len(args) in (3, 4) and args[0] == "internal":
        internal(args[1], args[2], args[3] if len(args) == 4 else "")
    elif len(args) == 3 and args[0] == "status":
        status(args[1], args[2])
    else:
        raise SystemExit(__doc__)
