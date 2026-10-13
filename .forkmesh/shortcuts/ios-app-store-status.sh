#!/usr/bin/env bash
# name: iOS App Store status
# description: Show the iOS app in App Store Connect: each App Store version's state (waiting for review, in review, rejected), the App Review submissions and any that need action, the newest TestFlight builds and which groups get them, every TestFlight group with its testers and public link, and the latest tester feedback and customer reviews.
set -euo pipefail

cd "$(dirname "$0")/../.."
exec python3 -u tools/mac_adhoc_deploy.py --store-status "$@"
