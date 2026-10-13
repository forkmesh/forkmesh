#!/usr/bin/env bash
set -euo pipefail
exec python3 "$(cd "$(dirname "$0")/.." && pwd)/app/tools/sync_request_block_rules.py" --rqlite "$@"
