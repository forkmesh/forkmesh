#!/usr/bin/env python3
"""Static contracts for the ForkMesh Patreon support section."""

from pathlib import Path


PUBLIC = Path(__file__).resolve().parents[2] / "www" / "public"
INDEX_HTML = (PUBLIC / "index.html").read_text(encoding="utf-8")


def _support_section() -> str:
    start = INDEX_HTML.index('<section id="support-forkmesh"')
    end = INDEX_HTML.index('<section id="faq"', start)
    return INDEX_HTML[start:end]


def _mobile_styles() -> str:
    start = INDEX_HTML.rindex("@media (max-width: 768px)")
    end = INDEX_HTML.index("@media (max-width: 420px)", start)
    return INDEX_HTML[start:end]


def _support_card_styles() -> str:
    start = INDEX_HTML.index("#support-forkmesh .support-mission-card {")
    end = INDEX_HTML.index("#support-forkmesh .support-mission-card::before {", start)
    return INDEX_HTML[start:end]


def _support_cta_styles() -> str:
    start = INDEX_HTML.index("#support-forkmesh .support-mission-cta {")
    end = INDEX_HTML.index("#support-forkmesh .support-mission-cta:hover {", start)
    return INDEX_HTML[start:end]


def _support_actions_styles() -> str:
    start = INDEX_HTML.index("#support-forkmesh .support-mission-actions {")
    end = INDEX_HTML.index("#support-forkmesh .support-mission-cta {", start)
    return INDEX_HTML[start:end]


def _support_heading_styles() -> str:
    start = INDEX_HTML.index("#support-forkmesh h2 {")
    return INDEX_HTML[start : INDEX_HTML.index("}", start)]


def test_support_section_sits_after_pricing_and_before_faq():
    support_start = INDEX_HTML.index('<section id="support-forkmesh"')
    # The plans-summary strip and the community-rewards band that used to
    # precede support are both gone; the plans table holds that slot now.
    plans_start = INDEX_HTML.index('<section id="pricing"')
    faq_start = INDEX_HTML.index('<section id="faq"', plans_start)

    assert plans_start < support_start < faq_start
    assert "plans-summary" not in INDEX_HTML
    assert '</section>\n\n      <section id="support-forkmesh"' in INDEX_HTML


def test_support_section_keeps_the_mission_and_a_minimal_safe_cta():
    support = _support_section()

    for text in (
        "Help us keep source code alive across machines people control.",
        "65+ merged open-source contributions",
        "flagged and became unavailable almost overnight",
        "Kaif and Jett started ForkMesh",
        "Support ForkMesh on Patreon",
        "Browse Instance",
        'href="https://www.patreon.com/16434219/join"',
        'href="/dashboard"',
        'target="_blank"',
        'rel="noreferrer"',
    ):
        assert text in support

    assert 'class="support-mission-action"' not in support
    assert "Support is optional, but it helps us build faster and stay focused." not in support
    assert '<div class="support-mission-actions">' in support

    for removed_text in (
        "desktop node",
        "local Git mirroring",
        "repo browsing",
        "signed collaboration flows",
        "self-hosted infrastructure",
        "source code should not have one fragile heartbeat",
        "Local Git mirrors",
        "Self-hosted nodes",
        "Signed collaboration flows",
        "Open docs and protocol work",
    ):
        assert removed_text not in support


def test_support_section_uses_scoped_responsive_card_styling():
    for marker in (
        "#support-forkmesh {",
        "scroll-margin-top: 6rem;",
        "#support-forkmesh .support-mission-card {",
        "#support-forkmesh .support-mission-card::before {",
        "#support-forkmesh .support-mission-actions {",
        "#support-forkmesh .support-mission-secondary {",
        "rgba(255, 255, 255, 0.055)",
        "#support-forkmesh .support-mission-cta {\n        display: inline-flex;",
        "background: var(--btn-primary-fill);",
        "font-family: var(--landing-hero-font);",
        "font-family: var(--landing-body-font);",
        "font-family: var(--landing-heading-font);",
    ):
        assert marker in INDEX_HTML

    for removed_marker in (
        "#support-forkmesh .support-mission-funding {",
        "#support-forkmesh .support-mission-focus {",
        "#support-forkmesh .support-mission-action {",
        "#support-forkmesh .support-mission-note {",
    ):
        assert removed_marker not in INDEX_HTML

    card_styles = _support_card_styles()
    assert card_styles.count("gradient(") == 1
    # The card shares the neutral .feature-card surface (black on --border)
    # instead of the earlier green wash; green survives only as the kicker dot.
    assert "background: #000000;" in card_styles
    assert "border: 1px solid var(--border);" in card_styles
    assert "rgba(34, 197, 94" not in card_styles
    assert "rgba(134, 239, 172" not in card_styles
    assert "grid-template-columns: minmax(0, 1fr);" in card_styles

    # The heading uses the shared section-heading face, matching #features,
    # #solution, and the FAQ title rather than the serif hero face.
    heading_styles = _support_heading_styles()
    assert "font-family: var(--landing-heading-font);" in heading_styles

    cta_styles = _support_cta_styles()
    assert "margin-top" not in cta_styles
    assert "box-shadow: var(--btn-primary-shadow);" in cta_styles
    assert "border: 1px solid var(--btn-primary-border);" in cta_styles

    assert "margin-top: 1.5rem;" in _support_actions_styles()

    mobile = _mobile_styles()
    assert "#support-forkmesh .support-mission-card {" in mobile
    assert "#support-forkmesh .support-mission-cta {" not in mobile

    # The card itself is always one column; the copy inside it is mobile-first
    # and only splits into the heading/aside pair on wide screens, so that is
    # the declaration that actually governs the responsive behaviour.
    assert "grid-template-columns: minmax(0, 1fr);" in _support_card_styles()
    assert (
        "@media (min-width: 901px) {\n"
        "        #support-forkmesh .support-mission-copy {\n"
        "          grid-template-columns: minmax(0, 1.35fr) minmax(0, 0.85fr);"
    ) in INDEX_HTML


def test_support_section_avoids_replacement_and_anti_platform_framing():
    support = _support_section().lower()

    for prohibited in ("replace", "replacement", "anti-platform", "competitor"):
        assert prohibited not in support
