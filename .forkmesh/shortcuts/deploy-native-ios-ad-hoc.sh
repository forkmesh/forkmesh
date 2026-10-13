#!/usr/bin/env bash
# name: Deploy native iOS app (ad hoc, Codemagic)
# description: Wait until forkmesh.com serves main, start Codemagic's ios-native-adhoc build on the forkmesh app, follow its steps and logs live and print the IPA or the failing step's log.
set -euo pipefail

cd "$(dirname "$0")/../.."
echo "Shipping main: $(git log -1 --format='%h %s' main)"
exec python3 -u tools/next_codemagic.py build ios-native-adhoc --mirror --execute --wait
