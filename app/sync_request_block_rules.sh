#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
python3 tools/sync_request_block_rules.py --rqlite "$@"
