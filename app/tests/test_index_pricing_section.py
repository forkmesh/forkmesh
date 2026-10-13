#!/usr/bin/env python3
"""Static contracts for the homepage pricing section."""

from pathlib import Path


PUBLIC = Path(__file__).resolve().parents[2] / "www" / "public"
INDEX_HTML = (PUBLIC / "index.html").read_text(encoding="utf-8")


def test_plans_table_sits_between_solution_and_support():
    """The plans-summary strip was replaced by the full plans table.

    Keep the current homepage design while pinning the public entitlement
    boundary, the launch memberships, and the managed-research gate.
    """
    plans = INDEX_HTML.index('<section id="pricing"')
    support = INDEX_HTML.index('<section id="support-forkmesh"')
    faq = INDEX_HTML.index('<section id="faq"', support)

    assert plans < support < faq

    section = INDEX_HTML[plans:support]
    for text in (
        'id="plans-title"',
        "Free protocol. Paid service.",
        "Use, fork, and self-host ForkMesh forever.",
        '<div class="p-name">Protocol + Hosted Free</div>',
        '<div class="p-name">Supporter</div>',
        '<div class="p-name">Pro <span class="pop-tag">Popular</span></div>',
        '<div class="p-name">Managed services</div>',
        "Research <span class=\"per\">/ not for sale</span>",
        "Always free and portable",
        "Managed services (research only)",
        "No membership changes protocol rights",
        'href="/docs/business-model"',
        'data-billing="monthly"',
        'data-billing="yearly"',
        '<div class="compare-title">Compare plans</div>',
    ):
        assert text in section

    for unvalidated_claim in (
        "Always-on mirror included",
        "Enterprise additions",
        "Contact sales",
        "Custom rollout",
    ):
        assert unvalidated_claim not in section

    # The retired strip must not linger in markup or stylesheet.
    assert "plans-summary" not in INDEX_HTML
    assert "Core stays free. Pro unlocks hosted agents." not in INDEX_HTML

    # id="pricing" now belongs to this plans table: /pricing was retired and
    # 308s to /#pricing. What must not come back is the old rewards band that
    # previously held the id.
    assert INDEX_HTML.count('id="pricing"') == 1
    assert "Earn rewards for mirroring" not in INDEX_HTML

    # The card checkout that used to follow the table was removed from the
    # homepage in September 2026 (the plan cards link to /signup and the
    # dashboard owns membership changes), and on-site checkout is gone
    # altogether, so neither its markup, its hooks nor its script may come
    # back here.
    assert '<section id="memberships"' not in INDEX_HTML
    assert "data-checkout" not in INDEX_HTML
    assert "memberships.js" not in INDEX_HTML
    assert "#memberships" not in INDEX_HTML


def test_plan_cards_stay_pinned_while_the_feature_rows_scroll():
    plans = INDEX_HTML.index('<section id="pricing"')
    support = INDEX_HTML.index('<section id="downloads"', plans)
    section = INDEX_HTML[plans:support]

    assert section.index('<div class="pt-head" role="rowgroup">') < section.index(
        '<div class="pt-body" role="rowgroup">')
    head = section.split('<div class="pt-head" role="rowgroup">', 1)[1].split(
        '<div class="pt-mini-head"', 1)[0]
    assert head.count('role="columnheader"') == 5
    assert '<div class="compare-title">Compare plans</div>' in head
    assert head.count('class="p-btn"') == 4
    assert head.count('class="btn-chevron"') == 4
    for name in ("Free", "Supporter", "Pro", "Managed"):
        assert f"<span>{name}</span>" in section

    body = section.split('<div class="pt-body" role="rowgroup">', 1)[1]
    assert body.count('<div class="pt-row" role="row">') == 13
    assert body.count('class="pt-feature-desc"') == 13
    assert body.count('<span class="pt-yes" role="img" aria-label="Included"></span>') == 25
    assert body.count('<span class="pt-note">Research</span>') == 3

    styles = INDEX_HTML.split("<style>", 1)[1]
    sticky = styles.split("      #pricing .pt-head {", 1)[1].split("}", 1)[0]
    assert "position: sticky;" in sticky
    assert "top: var(--pt-sticky-top);" in sticky
    assert "--pt-sticky-top: calc(var(--home-nav-h) + 0.25rem);" in styles
    mask = styles.split("      #pricing .pt-head::before,", 1)[1].split("}", 1)[0]
    assert "bottom: 100%;" in mask
    assert "height: var(--pt-sticky-top);" in mask
    intro = styles.split("      #pricing .pt-intro {", 1)[1].split("}", 1)[0]
    assert "z-index: 7;" in intro
    assert "#pricing .pt-mini-head {\n          position: sticky;" in styles
