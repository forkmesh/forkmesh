#!/usr/bin/env bash
# name: Deploy new version
# description: Cut the next version, reset every platform's build number to 0, then run the Android, Linux, Windows and macOS deploy shortcuts so the homepage offers the new version everywhere.

# step: Cut the new version
set -euo pipefail
cd "$(dirname "$0")/../.."
echo "Cutting a version from main: $(git log -1 --format='%h %s' main)"
reply=$(python3 tools/forkmesh_release_request.py create "$@") || { echo "$reply"; exit 1; }
echo "$reply"
tag=$(printf '%s' "$reply" | python3 -c 'import json, sys; print(json.load(sys.stdin)["tag"])')

# step: Wait for the release candidate
python3 -u tools/forkmesh-release-host.py --wait-candidate --tag "$tag"

# step: Reset build numbers to 0
python3 -u tools/forkmesh-release-host.py --reset-builds --tag "$tag"

# step: Deploy Android build
bash .forkmesh/shortcuts/deploy-build-android.sh --first-build --tag "$tag"

# step: Deploy Linux build
bash .forkmesh/shortcuts/deploy-build-linux.sh --first-build --tag "$tag"

# step: Deploy Windows build
bash .forkmesh/shortcuts/deploy-build-windows.sh --first-build --tag "$tag"

# step: Deploy macOS build
bash .forkmesh/shortcuts/deploy-build-macos.sh --first-build --tag "$tag"
