#!/usr/bin/env bash
# name: Unstick the office Mac build
# description: Stop every office MacBook shortcut this desktop runs, kill every build process on the Mac (leftovers from dead builds included), release the Mac's build lock and check the Mac is idle. Run a Mac shortcut again afterwards to rebuild.
set -euo pipefail

cd "$(dirname "$0")/../.."
exec python3 -u tools/mac_adhoc_deploy.py --unstick
