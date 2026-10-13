#!/usr/bin/env bash
# name: Turn TestFlight feedback into tasks
# description: Add a Tasks-page task for each new TestFlight tester feedback, with the tester's comment, device, build and screenshots attached, and delete the feedback from App Store Connect once its task is done.
set -euo pipefail

cd "$(dirname "$0")/../.."
exec python3 -u tools/testflight_feedback_tasks.py "$@"
