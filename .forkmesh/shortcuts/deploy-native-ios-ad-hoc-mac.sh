#!/usr/bin/env bash
# name: Deploy native iOS app (ad hoc, office Mac)
# description: Wait until the cores serve main, build Codemagic's ios-native-adhoc steps on the office MacBook (no Codemagic minutes), follow every step live, then publish the IPA and email the install link to the early iOS testers.
set -euo pipefail

cd "$(dirname "$0")/../.."
echo "Shipping main: $(git log -1 --format='%h %s' main)"
exec python3 -u tools/mac_adhoc_deploy.py --execute
