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
    r"const PRODUCT_ACTIONS = new Map\(\[(.*?)\]\);", ANALYTICS, re.DOTALL
)
ALLOWED = dict(re.findall(r'\["([^"]+)",\s*"([^"]+)"\]', ALLOWLIST_BLOCK.group(1)))
STAGES = {"interest", "install", "signup", "account", "activation"}

MARKUP_ACTIONS = {
    (path.name, action)
    for path in [*PUBLIC.rglob("*.html"), *PUBLIC.glob("*.js")]
    for action in re.findall(
        r'(?:data-forkmesh-product-action="|forkmeshProductAction = ")([^"]+)"',
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


def test_every_action_names_a_funnel_stage():
    assert set(ALLOWED.values()) <= STAGES


def test_homepage_conversion_points_are_instrumented():
    marked = {action for _, action in MARKUP_ACTIONS}
    for action in ("home-install-hero", "home-install-nav", "home-signup-nav",
                   "home-signup-free", "home-pro-membership", "home-signup-final",
                   "home-start-instance", "home-download", "desktop-download",
                   "desktop-install-copied"):
        assert action in marked, action
    signup = (PUBLIC.parents[1] / "app" / "public" / "signup.js").read_text(encoding="utf-8")
    assert 'recordProductAction?.("signup-completed")' in signup
    assert ALLOWED["signup-completed"] == "account"


def test_homepage_hero_runs_the_headline_experiment():
    html = (PUBLIC / "index.html").read_text(encoding="utf-8")
    hosts = re.findall(r'data-fm-experiment="([^"]+)"', html)
    assert hosts and set(hosts) == {"home-hero-headline"}
    assert 'data-fm-variant="control"' in html
    assert re.search(r'data-fm-variant="concrete" hidden', html)
    assert "sdk.onFeatureFlags(applyExperiments);" in ANALYTICS
    assert 'showExperimentVariants(() => "control");' in ANALYTICS
