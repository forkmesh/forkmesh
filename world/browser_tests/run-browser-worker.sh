#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"
if [ "${FORKMESH_BROWSER_PRESTAGED:-0}" != "1" ]; then
  python3 ../../app/tools/build_dashboard_assets.py >/dev/null
  python3 ../../app/tools/build_site_assets.py cutover >/dev/null
fi

if [ "${FORKMESH_BROWSER_FULL:-0}" = "1" ]; then
  cd ../../app
  source ./pywrangler.sh
  export FORKMESH_D1_LOCAL=1
  pywrangler dev \
    --config ../world/browser_tests/wrangler.browser-tests.toml \
    --ip 127.0.0.1 \
    --port 4179 \
    --persist-to ../world/browser_tests/.wrangler/browser-tests-v4 \
    --local
  exit $?
fi

cd ../../app/dist
exec python3 -m http.server 4179 --bind 127.0.0.1
