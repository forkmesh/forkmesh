#!/usr/bin/env bash
set -euo pipefail
exec "$(dirname "$0")/../app/deploy.sh" world "$@"
