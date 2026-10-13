#!/usr/bin/env python3
"""Public-profile tip address contract.

A payout address is deliberately public profile data: /api/accounts/<name>
returns it to anyone, so /@name shows it as a scannable QR plus the literal
address a visitor can copy. ForkMesh holds neither the funds nor the key, so
the page is pure presentation of a value the account chose to publish. An
account with no address published says so instead of rendering an empty card,
and an organization - whose public record carries no payout field at all -
hides the card rather than asserting an address is missing.
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
REPOSITORY = ROOT.parent
SRC = ROOT / "src"
if str(SRC) not in sys.path:
    sys.path.insert(0, str(SRC))

from _dashboard_shell import assembled_dashboard_page  # noqa: E402
from _dashboard_bundle import assembled_dashboard_js  # noqa: E402

BUNDLE = assembled_dashboard_js()
ENTRY_TEXT = (SRC / "entry.py").read_text(encoding="utf-8")
WWW_WORKER = (REPOSITORY / "www" / "worker.js").read_text(encoding="utf-8")


def tips_section():
    match = re.search(
        r"// ---- Tips -+\n(.*?)\n  // ---- Fediverse presence", BUNDLE, re.S)
    assert match is not None, "tips section missing from dashboard.js"
    return match.group(1)


def test_profile_sidebar_carries_the_tip_card_in_both_profile_documents():
    # The sidebar is cloned from one <template>, so the card has to be in the
    # markup of the overview page and the repositories tab alike.
    for page in ("profile", "profile-repositories"):
        html = assembled_dashboard_page(page)
        card = re.search(r"<section data-profile-tips\b[^>]*>", html)
        assert card is not None, page
        # Visible by default: the empty state is the honest resting state, and
        # renderProfileTips only hides the whole card for organizations. The
        # \s guard keeps the utility class `overflow-hidden` from reading as
        # the `hidden` attribute.
        assert re.search(r"\shidden[\s>]", card.group(0)) is None, page
        detail = re.search(r"<div data-profile-tips-detail\b[^>]*>", html)
        assert detail is not None, page
        assert re.search(r"\shidden[\s>]", detail.group(0)) is not None, page
        assert "data-profile-tips-qr" in html, page
        assert "data-profile-tips-address" in html, page
        assert "data-profile-tips-copy" in html, page
        assert (
            "has not entered a Solana address for tips." in html
        ), page


def test_tip_card_renders_a_published_address_as_text_and_a_qr_code():
    section = tips_section()
    # The same base58 shape the Worker validates on write and re-checks before
    # publishing the field, so the page never draws a code for a junk value.
    assert "/^[1-9A-HJ-NP-Za-km-z]{32,44}$/" in section
    assert 'SOLANA_RE = re.compile(r"^[1-9A-HJ-NP-Za-km-z]{32,44}$")' in ENTRY_TEXT
    assert 'const address = String(session?.solana || "").trim();' in section
    assert "PROFILE_TIP_ADDRESS_RE.test(address) ? address" in section
    assert "if (code) code.textContent = address;" in section
    assert "window.ForkMeshQR.toCanvas(address, 148)" in section
    # The card is rendered from the profile subject, which on /@name is the
    # fetched public account rather than the signed-in session.
    assert "renderProfileTips(session);" in BUNDLE


def test_tip_card_states_plainly_when_no_address_is_published():
    section = tips_section()
    assert 'empty.hidden = Boolean(address);' in section
    assert (
        '"You have not entered a Solana address for tips."' in section
    )
    assert (
        '`${name ? "@" + name : "This user"} has not entered a Solana address '
        'for tips.`' in section
    )
    # Organizations publish no payout field at all, so the card disappears
    # instead of claiming the organization skipped one.
    assert 'card.hidden = session?.kind === "organization";' in section


def test_qr_generator_is_fetched_on_demand_from_this_origin():
    # world/public/qr.js is ForkMesh's own generator, staged onto the App
    # origin by build_site_assets rather than bundled or vendored twice.
    assert (REPOSITORY / "world" / "public" / "qr.js").is_file()
    assert not (ROOT / "public" / "qr.js").exists()
    staging = (ROOT / "tools" / "build_site_assets.py").read_text(encoding="utf-8")
    assert 'copy_file(WORLD / "public" / "qr.js", destination / "qr.js")' in staging
    # One shared loader, so a page that draws several codes fetches it once
    # and a page that draws none never fetches it at all.
    assert BUNDLE.count('script.src = "/qr.js";') == 1
    assert "function loadQrGenerator()" in BUNDLE
    assert "if (window.ForkMeshQR) return Promise.resolve(true);" in BUNDLE
    assert "void drawProfileTipQr(holder, address);" in tips_section()


def test_www_proxies_the_qr_script_so_public_profiles_can_draw_a_code():
    # forkmesh.com/@name is App-rendered but served from the marketing origin,
    # and WWW ships no qr.js of its own: without this the code would 404 under
    # script-src 'self'.
    subresources = re.search(
        r"const APP_SUBRESOURCES = new Set\(\[(.*?)\]\);", WWW_WORKER, re.S)
    assert subresources is not None
    assert '"/qr.js"' in subresources.group(1)


def test_every_profile_shell_asset_resolves_on_the_marketing_origin():
    import tomllib

    sys.path.insert(0, str(ROOT / "tools"))
    import build_site_assets

    shell = (ROOT / "public" / "dashboard" / "shell.html").read_text(encoding="utf-8")
    assets = set(re.findall(r'(?:src|href)="(/[^"?#]+\.(?:js|css))', shell))
    assert "/dashboard-composer.js" in assets
    subresources = re.search(
        r"const APP_SUBRESOURCES = new Set\(\[(.*?)\]\);", WWW_WORKER, re.S)
    proxied = set(re.findall(r'"([^"]+)"', subresources.group(1)))
    worker_first = [
        set(tomllib.loads((REPOSITORY / "www" / name).read_text(encoding="utf-8"))
            ["assets"]["run_worker_first"])
        for name in ("wrangler.toml", "wrangler.production.toml")
    ]
    staged = set(build_site_assets.WWW_APP_MODULES)
    for asset in sorted(assets):
        if (REPOSITORY / "www" / "public" / asset.lstrip("/")).is_file() or asset.lstrip("/") in staged:
            continue
        assert asset in proxied, asset
        for routes in worker_first:
            assert asset in routes, asset


def test_qr_paint_is_discarded_when_the_visitor_moved_to_another_profile():
    section = tips_section()
    # The script load is awaited, so the holder's recorded address is the
    # identity check that keeps a slow fetch from painting a stale code.
    assert "holder.dataset.profileTipsQrValue !== address) return;" in section
    assert "holder.dataset.profileTipsQrValue = address;" in section
    # A failed load degrades to a note: the printed address still pays.
    assert 'holder.textContent = "QR unavailable";' in section
