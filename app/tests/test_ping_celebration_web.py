import json
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
WELCOME = ROOT / "public" / "member-welcome.js"
DASHBOARD_SOURCE = ROOT / "public" / "dashboard" / "js" / "08-repo-detail-network.js"
DASHBOARD_BUNDLE = ROOT / "public" / "dashboard.js"

pytestmark = pytest.mark.skipif(
    shutil.which("node") is None, reason="Node.js is unavailable")

SCRIPT = """
const { freshCelebrations } = await import(process.argv[1]);
const now = 1_800_000_000_000;
const pings = [
  { id: "fresh", ts: now - 30_000, readAt: 0, meta: { celebrate: "crm-email-tracked" } },
  { id: "seen", ts: now - 10_000, readAt: 0, meta: { celebrate: "crm-email-tracked" } },
  { id: "read", ts: now - 10_000, readAt: now, meta: { celebrate: "crm-email-tracked" } },
  { id: "stale", ts: now - 6 * 60_000, readAt: 0, meta: { celebrate: "crm-email-tracked" } },
  { id: "plain", ts: now - 10_000, readAt: 0, meta: {} },
  { id: "", ts: now, readAt: 0, meta: { celebrate: "x" } },
];
console.log(JSON.stringify({
  fresh: freshCelebrations(pings, ["seen"], now).map((item) => item.id),
  none: freshCelebrations(null, [], now),
}));
"""


def test_only_new_unread_recent_celebration_pings_party():
    completed = subprocess.run(
        ["node", "--input-type=module", "-e", SCRIPT, WELCOME.as_uri()],
        capture_output=True, text=True, check=True, timeout=60)
    result = json.loads(completed.stdout)
    assert result == {"fresh": ["fresh"], "none": []}


def test_the_dashboard_parties_after_every_inbox_read():
    for path in (DASHBOARD_SOURCE, DASHBOARD_BUNDLE):
        text = path.read_text(encoding="utf-8")
        assert "celebrateNewPings(state.notifications);" in text
        assert 'import("/member-welcome.js")' in text
        assert "celebrateFreshPings(notifications)" in text
