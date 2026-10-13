#!/usr/bin/env bash
# name: Deploy native iOS app to the App Store (office Mac)
# description: Wait until the cores serve main, build Codemagic's ios-native-release steps on the office MacBook, upload the build to App Store Connect, put it in TestFlight internal testing and submit it for App Review, following every step live.
set -euo pipefail

cd "$(dirname "$0")/../.."
echo "Submitting main: $(git log -1 --format='%h %s' main)"
exec python3 -u tools/mac_adhoc_deploy.py --app-store --execute
