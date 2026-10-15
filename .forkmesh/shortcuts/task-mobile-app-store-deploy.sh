#!/usr/bin/env bash
# name: Remind release maintainer: deploy to the App Store
# description: Create a Tasks-page reminder assigned to the configured release maintainer to ship the iOS app to the App Store, with the current version, the newest release tag and the commands to check and submit it.
set -euo pipefail

cd "$(dirname "$0")/../.."
head=$(git log -1 --format='%h %s' main)
tag=$(git tag --list 'v[0-9]*' --sort=-v:refname | head -n 1)
version=$(sed -n 's/^MARKETING_VERSION *= *//p' native/ios/Config/Version.xcconfig)
: "${FORKMESH_RELEASE_ASSIGNEE:?set FORKMESH_RELEASE_ASSIGNEE to the release maintainer account}"
exec python3 -I - "$PWD" "$head" "$tag" "$version" "$FORKMESH_RELEASE_ASSIGNEE" <<'PY'
import sys

root, head, tag, version, assignee = sys.argv[1:6]
sys.path.insert(0, root + "/tools")
import forkmesh_mcp_server as mcp

details = f"""A reminder to deploy the iOS app to the App Store.

- iOS version on main: {version} (native/ios/Config/Version.xcconfig)
- Newest release tag: {tag or "none"}
- main: {head}

Steps:
1. See what App Store Connect has now: `python3 native/ios/tools/asc_appstore.py status`
2. Preview the submission: `python3 native/ios/tools/asc_appstore.py plan {version}`
3. Build and submit: start Codemagic's `ios-native-release` on {tag or "the tag"} with `FORKMESH_IOS_APP_STORE=1`, or run it on the office Mac (docs/native/MAC_BUILD.md).

Guide: docs/native/IOS_RELEASE.md, "App Store rollout for a tagged release". App Store Connect: https://appstoreconnect.apple.com/apps"""

task = mcp.create_task({
    "title": "Reminder: deploy the iOS app to the App Store",
    "details": details,
    "assigneeKind": "user",
    "assignee": assignee,
    "repository": "forkmesh/forkmesh",
})
created = task.get("task") if isinstance(task.get("task"), dict) else task
print(f"Created task {created.get('id', '')} for {created.get('assignee', '')}: {created.get('title', '')}")
PY
