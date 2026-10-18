#!/usr/bin/env bash
# name: Deploy Linux build
# description: Build the next Linux build number of the current version from main (the Linux binary and AppImage), publish it to the forkmesh.com downloads and wait until the homepage offers it.

# step: Plan the build
set -euo pipefail
cd "$(dirname "$0")/../.."
python3 -u tools/forkmesh-release-host.py --deploy-build linux --dry-run "$@"

# step: Build, sign and publish
python3 -u tools/forkmesh-release-host.py --deploy-build linux "$@"

# step: Check the homepage
python3 -u tools/forkmesh-release-host.py --check-build linux
