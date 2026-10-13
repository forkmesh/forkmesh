#!/usr/bin/env bash
# name: Office Mac build status
# description: Show the office MacBook's builds: the Mac shortcuts this desktop runs and how long since each printed anything, who holds the Mac's build lock, the newest build log's last lines, and every build process on the Mac, with leftovers from dead builds marked.
set -euo pipefail

cd "$(dirname "$0")/../.."
exec python3 -u tools/mac_adhoc_deploy.py --status
