#!/usr/bin/env bash
# name: Deploy macOS build
# description: Build the next macOS build number of the current version from main (the universal macOS .dmg on the office Mac), publish it to the forkmesh.com downloads and wait until the homepage offers it.

# step: Plan the build
set -euo pipefail
cd "$(dirname "$0")/../.."
python3 -u tools/forkmesh-release-host.py --deploy-build macos --dry-run "$@"

# step: Build, sign and publish
python3 -u tools/forkmesh-release-host.py --deploy-build macos "$@"

# step: Check the homepage
python3 -u tools/forkmesh-release-host.py --check-build macos
