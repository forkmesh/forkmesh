#!/usr/bin/env python3
"""Static contracts for landing page typography."""

import re
from pathlib import Path


PUBLIC = Path(__file__).resolve().parents[2] / "www" / "public"
INDEX_HTML = (PUBLIC / "index.html").read_text(encoding="utf-8")


def _font_faces(family: str) -> list[str]:
    blocks = re.findall(
        r"@font-face\s*{(?P<body>.*?)\n\s*}",
        INDEX_HTML,
        flags=re.DOTALL,
    )
    return [block for block in blocks if f'font-family: "{family}"' in block]


def _has_font_face(family: str, path: str, weight: str, format_: str) -> bool:
    return any(
        f'url("{path}") format("{format_}")' in block
        and f"font-weight: {weight};" in block
        for block in _font_faces(family)
    )


def test_landing_page_uses_bundled_brand_font_assets():
    assert _has_font_face(
        "Domine",
        "/assets/fonts/Domine-Regular.woff2",
        "400",
        "woff2",
    )
    assert _has_font_face(
        "Domine",
        "/assets/fonts/Domine-Bold.woff2",
        "700",
        "woff2",
    )
    assert _has_font_face(
        "ABCFavorit",
        "/assets/fonts/ABCFavorit-Bold.woff2",
        "700",
        "woff2",
    )

    custom_font_css = "\n".join(
        block
        for family in ("Domine", "ABCFavorit")
        for block in _font_faces(family)
    )
    assert "/assets/fonts/Lato-" not in custom_font_css
    assert "/assets/fonts/DejaVuSansMono" not in custom_font_css
    assert "ABCFavoritMono" not in INDEX_HTML


def test_landing_page_declares_typography_roles():
    assert "--landing-body-font: Inter, ui-sans-serif, system-ui, sans-serif;" in INDEX_HTML
    assert "--landing-hero-font: Domine, Georgia, serif;" in INDEX_HTML
    assert (
        "--landing-heading-font: ABCFavorit, Inter, ui-sans-serif, system-ui, sans-serif;"
        in INDEX_HTML
    )
    assert (
        "--landing-mono-font: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace;"
        in INDEX_HTML
    )

    assert ".hero-section h1 {" in INDEX_HTML
    assert "font-family: var(--landing-hero-font);" in INDEX_HTML

    # The feature grid, the node-selector band, and the community-rewards band
    # were all removed from the homepage, so their selectors are gone. The
    # plans table and the capabilities deck joined the shared section-heading
    # rule in their place.
    for selector in (
        "#capabilities h2",
        "#solution h2",
        "#pricing h2",
        "#faq .faq-title",
    ):
        assert selector in INDEX_HTML

    for selector in (
        "#capabilities article h3",
        "#solution article h3",
        "#faq-list .faq-item summary",
    ):
        assert selector in INDEX_HTML

    for removed in (
        "#features h2",
        "#pricing.forkmesh",
        ".feature-card",
        "#network h2",
    ):
        assert removed not in INDEX_HTML

    assert "font-family: var(--landing-heading-font);" in INDEX_HTML

    for selector in (
        ".hero-section p",
        "#capabilities p",
        "#solution p",
        "#pricing .pricing-sub",
        "#faq .faq-intro",
        "#faq-list .faq-item p",
    ):
        assert selector in INDEX_HTML

    assert "font-family: var(--landing-body-font);" in INDEX_HTML


def test_landing_page_uses_inter_for_non_heading_text():
    # The rewards band that defined #pricing.forkmesh is gone; the plans table
    # is the section carrying body copy in its place.
    assert "#pricing.forkmesh {" not in INDEX_HTML
    assert "#pricing {" in INDEX_HTML
    assert "font-family: var(--landing-body-font);" in INDEX_HTML
    assert "--pricing-mono" not in INDEX_HTML
    assert "font-family: var(--pricing-mono);" not in INDEX_HTML
