#!/usr/bin/env bash
# name: Build macOS desktop app (office Mac)
# description: Build the universal macOS desktop .dmg from main on the office MacBook, follow the build live, then copy the .dmg back to ~/projects/forkmesh/data/mac-adhoc/desktop/.
set -euo pipefail

cd "$(dirname "$0")/../.."
echo "Building main: $(git log -1 --format='%h %s' main)"
exec python3 -u tools/mac_adhoc_deploy.py --desktop --execute
