"""Regression contract binding funnel markup to the analytics allowlist.

`analytics.js` silently drops any `forkmesh_product_action` whose name is
absent from `PRODUCT_ACTIONS` (in `recordProductAction`).  A conversion CTA can
therefore be marked up correctly, pass review, and still record nothing.  This test fails the build
when the two sides drift apart.
"""

import re
from pathlib import Path


PUBLIC = Path(__file__).resolve().parents[1] / "public"
ANALYTICS = (PUBLIC / "analytics.js").read_text(encoding="utf-8")

ALLOWLIST_BLOCK = re.search(
    r"const PRODUCT_ACTIONS = new Set\(\[(.*?)\]\);", ANALYTICS, re.DOTALL
)
ALLOWED = set(re.findall(r'"([^"]+)"', ALLOWLIST_BLOCK.group(1)))

MARKUP_ACTIONS = {
    (path.name, action)
    for path in PUBLIC.rglob("*.html")
    for action in re.findall(
        r'data-forkmesh-product-action="([^"]+)"',
        path.read_text(encoding="utf-8"),
    )
}


def test_allowlist_block_is_parseable():
    assert ALLOWLIST_BLOCK is not None
    assert ALLOWED


def test_every_marked_up_action_is_allowlisted():
    dropped = sorted(
        f"{page}: {action}"
        for page, action in MARKUP_ACTIONS
        if action not in ALLOWED
    )
    assert not dropped, (
        "these CTAs are instrumented in markup but dropped by "
        f"PRODUCT_ACTIONS, so they record nothing: {dropped}"
    )
