#!/usr/bin/env bash
# name: Remind release maintainer: deploy to the Play Store
# description: Create a Tasks-page reminder assigned to the configured release maintainer to ship the Android app to Google Play, with the current version, the newest release tag and the commands to build the bundle.
set -euo pipefail

cd "$(dirname "$0")/../.."
head=$(git log -1 --format='%h %s' main)
tag=$(git tag --list 'v[0-9]*' --sort=-v:refname | head -n 1)
gradle=native/android/app/build.gradle.kts
version=$(sed -n 's/.*versionName = versionNameOverride.ifEmpty { "\(.*\)" }.*/\1/p' "$gradle")
code=$(sed -n 's/.*versionCode = versionCodeOverride ?: \([0-9]*\).*/\1/p' "$gradle")
: "${FORKMESH_RELEASE_ASSIGNEE:?set FORKMESH_RELEASE_ASSIGNEE to the release maintainer account}"
exec python3 -I - "$PWD" "$head" "$tag" "$version" "$code" "$FORKMESH_RELEASE_ASSIGNEE" <<'PY'
import sys

root, head, tag, version, code, assignee = sys.argv[1:7]
sys.path.insert(0, root + "/tools")
import forkmesh_mcp_server as mcp

details = f"""A reminder to deploy the Android app to the Google Play Store.

- Android version on main: {version} / versionCode {code} (native/android/app/build.gradle.kts)
- Newest release tag: {tag or "none"}
- main: {head}
- Application id: `com.forkmesh.forkmesh` (the id the earlier store build used, so its users upgrade in place)

Steps:
1. Sign in to Play Console with the ForkMesh Google account and open or create the ForkMesh app: https://play.google.com/console
2. Build the signed bundle from `native/android` with the upload key (`FORKMESH_ANDROID_KEYSTORE`, `FORKMESH_ANDROID_KEYSTORE_PASSWORD`, `FORKMESH_ANDROID_KEY_ALIAS`, `FORKMESH_ANDROID_KEY_PASSWORD`):
   `./gradlew :app:bundleRelease -Pforkmesh.appId=com.forkmesh.forkmesh -Pforkmesh.versionCode={code} -Pforkmesh.versionName={version}`
   The bundle is `app/build/outputs/bundle/release/app-release.aab`. The versionCode must be above every code Play has seen.
3. Keep Play App Signing on. If Play already has this app with an upload key we no longer hold, request an upload key reset in Play Console.
4. Fill in the store listing, Data safety (same data types as the App Store privacy answers), the content rating questionnaire and the privacy policy URL `https://forkmesh.com/privacy`.
5. Upload the bundle to Internal testing, install it on a phone, check sign-in and the relay, then promote it to Production.

Guides: docs/native/ANDROID.md (application id, signing) and docs/native/README.md, "Configuration"."""

task = mcp.create_task({
    "title": "Reminder: deploy the Android app to the Play Store",
    "details": details,
    "assigneeKind": "user",
    "assignee": assignee,
    "repository": "forkmesh/forkmesh",
})
created = task.get("task") if isinstance(task.get("task"), dict) else task
print(f"Created task {created.get('id', '')} for {created.get('assignee', '')}: {created.get('title', '')}")
PY
