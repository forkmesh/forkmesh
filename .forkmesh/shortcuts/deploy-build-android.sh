#!/usr/bin/env bash
# name: Deploy Android build
# description: Build the next Android build number of the current version from main (the signed Android APK), publish it to the forkmesh.com downloads and wait until the homepage offers it.

# step: Plan the build
set -euo pipefail
cd "$(dirname "$0")/../.."
python3 -u tools/forkmesh-release-host.py --deploy-build android --dry-run "$@"

# step: Build, sign and publish
python3 -u tools/forkmesh-release-host.py --deploy-build android "$@"

# step: Check the homepage
python3 -u tools/forkmesh-release-host.py --check-build android
