"""Membership is offered on request, consistent, and trustworthy."""

import hashlib
from pathlib import Path


# MainWindowRepos.cpp was split into feature files; read them in the original
# source order so section slices that span functions still line up.
MAIN_WINDOW_REPOS_SOURCES = (
    "MainWindowRepos.cpp",
    "MainWindowReposInternal.h",
    "MainWindowRepoSettings.cpp",
    "MainWindowRepoSync.cpp",
    "MainWindowMirrorPush.cpp",
    "MainWindowRepoFetch.cpp",
    "MainWindowAccounts.cpp",
)

REPOSITORY = Path(__file__).resolve().parents[2]
APP = REPOSITORY / "app"
WWW = REPOSITORY / "www"
WORLD = REPOSITORY / "world"
DESKTOP = REPOSITORY / "desktop"
PROMPT = (APP / "public/membership-prompt.js").read_text(encoding="utf-8")


def test_shared_prompt_has_all_patreon_tiers_and_the_artwork_stays_intact():
    assert 'class="fm-member-free"' in PROMPT
    for tier, price, reward_id in (
        ("supporter", "$5 / month", "29122413"),
        ("pro", "$10 / month", "29122568"),
        ("builder", "$50 / month", "29122566"),
        ("studio", "$100 / month", "29260767"),
    ):
        assert f'data-plan="{tier}"' in PROMPT
        assert price in PROMPT
        assert f"rid={reward_id}" in PROMPT
    assert "https://www.patreon.com/16434219/join" in PROMPT
    expected_hashes = {
        "patreon-supporter.png": "8e9c36d9e6cf99f90b67c2eb3347a44731cd5091018178fab34e1352896ddb21",
        "patreon-pro.png": "1017d76e391c9b51337a0aa0a24eefcf41756de1e9f6ec8c745d0663b09b09c2",
        "patreon-builder.png": "c590253e883c1b4774a81ca2a618d2344b6a0f23990639284b336b04c61e0b8a",
        "patreon-studio.png": "e27958430e330895f31e3eb16010aa25a07ff29430e5b6031791b8fcc653c6ba",
    }
    for name, expected_hash in expected_hashes.items():
        payload = (WWW / "public/assets/membership" / name).read_bytes()
        assert len(payload) > 10_000
        assert hashlib.sha256(payload).hexdigest() == expected_hash


def test_all_live_patreon_perks_are_present_in_compact_cards():
    for perk in (
        "Supporters wall / public credits listing",
        "Automatic AI PR reviews and quick fixes",
        "Exclusive Builder World outfit and office prop set",
        "Priority support channel (business-hours priority, not 24/7)",
        "Direct roadmap input for your use case",
    ):
        assert perk in PROMPT
    assert "grid-template-columns:repeat(4,minmax(0,1fr))" in PROMPT
    assert "max-height:calc(100svh - 12px);overflow:hidden" in PROMPT


def test_panel_only_ever_opens_on_request():
    """No timer, no dismissal ledger, no unsolicited interstitial."""
    for banned in (
        "WAIT_SECONDS",
        "DISMISS_MS",
        "DISMISS_KEY",
        "SKIP_PATHS",
        "setInterval",
        "rememberDismissal",
        "visibilityState",
    ):
        assert banned not in PROMPT, banned
    # The ways a visitor can ask for it.
    assert 'window.addEventListener("forkmesh:show-membership", open)' in PROMPT
    assert '[data-forkmesh-membership]' in PROMPT
    assert "Object.freeze({ installed: true, open, close })" in PROMPT


def test_prompt_asks_for_support_without_green_dots_or_tier_art():
    assert "--fm-live" not in PROMPT
    assert "fm-member-kicker" not in PROMPT
    assert "fm-member-plan-art" not in PROMPT
    assert "Most chosen" not in PROMPT
    assert "ForkMesh Core is free, and it stays free for everyone." in PROMPT
    assert PROMPT.count('class="fm-member-perks-label"') == 4
    for label in (
        "Everything in Supporter, plus:",
        "Everything in Pro, plus:",
        "Everything in Builder, plus:",
    ):
        assert label in PROMPT
    assert PROMPT.count("Support for $") == 4
    assert 'if (count === 0) return "Be the first to join";' in PROMPT


def test_panel_is_dismissible_and_never_gates_core():
    assert 'data-member-close aria-label="Not now"' in PROMPT
    assert 'event.key === "Escape"' in PROMPT
    assert "every Core feature stays enabled whether or not you join" in PROMPT


def test_the_keep_the_mesh_independent_framing_is_gone():
    desktop = (DESKTOP / "src/MainWindowMembership.cpp").read_text(
        encoding="utf-8"
    )
    for source in (PROMPT, desktop):
        assert "Keep the mesh independent" not in source
        assert "independent" not in source
    assert "Become a ForkMesh member." in PROMPT
    assert "Become a ForkMesh member." in desktop


def test_every_surface_offers_a_way_to_become_a_paying_member():
    """Removing the interstitial must not remove the path to joining."""
    header = (WWW / "public/site-header.js").read_text(encoding="utf-8")
    assert "data-forkmesh-membership>Membership</a>" in header
    # A visitor without JavaScript, or with a failed bundle fetch, still lands
    # somewhere they can actually pay.
    assert 'const PATREON_JOIN_HREF = "https://www.patreon.com/16434219/join"' in header
    assert "window.open(PATREON_JOIN_HREF" in header

    settings = (
        APP / "public/dashboard/partials/views/settings.html"
    ).read_text(encoding="utf-8")
    built = (APP / "public/dashboard/settings/index.html").read_text(
        encoding="utf-8"
    )
    for source in (settings, built):
        # The old target was a homepage anchor that no longer exists.
        assert "forkmesh.com/#memberships" not in source
        assert "data-membership-choose data-forkmesh-membership" in source

    desktop_settings = (DESKTOP / "src/MainWindowSettings.cpp").read_text(
        encoding="utf-8"
    )
    assert "View membership options" in desktop_settings
    assert "&MainWindow::openMembershipOptions" in desktop_settings
    account_menu = "".join(
        (DESKTOP / "src" / name).read_text(encoding="utf-8")
        for name in MAIN_WINDOW_REPOS_SOURCES
    )
    assert 'QStringLiteral("Become a member…")' in account_menu
    assert "openMembershipOptions();" in account_menu


def test_membership_hash_is_a_stable_address_for_apps_and_extensions():
    """Mobile apps and extensions send people to forkmesh.com/#membership."""
    assert 'location.hash === "#membership"' in PROMPT
    assert 'addEventListener("hashchange"' in PROMPT
    header = (WWW / "public/site-header.js").read_text(encoding="utf-8")
    assert 'location.hash === "#membership"' in header

    home = (WWW / "public/index.html").read_text(encoding="utf-8")
    assert home.count('href="#membership" data-forkmesh-membership>Membership</a>') == 2
    hackathon = (WWW / "public/hackathon.html").read_text(encoding="utf-8")
    assert hackathon.count('href="/#membership">Membership</a>') == 2


def test_apps_and_extensions_link_to_the_membership_address():
    membership = "https://forkmesh.com/#membership"
    sources = {
        "android": "native/android/core/src/main/kotlin/com/forkmesh/nativeapp/account/AuthFlowLogic.kt",
        "browser-extension": "marketplace/packages/browser-extension/content/extension/popup.html",
        "vscode": "marketplace/packages/forkmesh-ide/content/vscode/src/extension.ts",
    }
    for surface, path in sources.items():
        assert membership in (REPOSITORY / path).read_text(encoding="utf-8"), surface

    android = (
        REPOSITORY
        / "native/android/app/src/main/kotlin/com/forkmesh/nativeapp/feature/account/settings/SettingsScreen.kt"
    ).read_text(encoding="utf-8")
    assert 'Section("Membership")' in android
    assert "openExternalUrl(context, LegalLinks.MEMBERSHIP)" in android
    vscode = (
        REPOSITORY / "marketplace/packages/forkmesh-ide/content/vscode/package.json"
    ).read_text(encoding="utf-8")
    assert vscode.count('"command": "forkmesh.becomeMember"') == 2
    assert "command:forkmesh.becomeMember" in vscode


def test_every_dashboard_page_has_membership_in_sidebar_and_account_menu():
    pages = [
        path
        for path in (APP / "public").rglob("*.html")
        if "partials" not in path.parts
        and 'id="accountMenuDropdown"' in path.read_text(encoding="utf-8")
    ]
    assert len(pages) >= 10
    for page in pages:
        source = page.read_text(encoding="utf-8")
        assert source.count(
            'data-forkmesh-membership href="https://forkmesh.com/#membership"'
        ) >= 2, page


def test_prompt_links_to_the_public_financial_ledger():
    assert "Financially transparent" in PROMPT
    assert 'href="/ledger"' in PROMPT
    desktop = (DESKTOP / "src/MainWindowMembership.cpp").read_text(
        encoding="utf-8"
    )
    assert "Financially transparent" in desktop
    assert "https://forkmesh.com/ledger" in desktop


def test_shared_prompt_is_loaded_in_every_web_surface():
    assert "/membership-prompt.js" in (
        APP / "public/dashboard/shell.html"
    ).read_text(encoding="utf-8")
    assert "/membership-prompt.js" in (
        WWW / "public/site-header.js"
    ).read_text(encoding="utf-8")
    assert "/membership-prompt.js" in (
        WWW / "public/index.html"
    ).read_text(encoding="utf-8")
    assert "/membership-prompt.js" in (
        WORLD / "public/world/index.html"
    ).read_text(encoding="utf-8")
    assert '"/membership-prompt.js"' in (
        WORLD / "worker.js"
    ).read_text(encoding="utf-8")
    deploy_targets = (APP / "tools/deploy_targets.py").read_text(
        encoding="utf-8"
    )
    assert '"www/public/assets/membership"' in deploy_targets


def test_desktop_panel_is_not_scheduled_and_keeps_no_dismissal_ledger():
    window = (DESKTOP / "src/MainWindow.cpp").read_text(encoding="utf-8")
    assert "maybeShowMembershipPrompt" not in window
    assert "MainWindow::openMembershipOptions" not in window
    source = (DESKTOP / "src/MainWindowMembership.cpp").read_text(
        encoding="utf-8"
    )
    assert "promptDismissedAt" not in source
    assert "QSettings" not in source
    assert "void MainWindow::openMembershipOptions()" in source


def test_desktop_prompt_embeds_art_and_overrides_global_label_backgrounds():
    source = (DESKTOP / "src/MainWindowMembership.cpp").read_text(
        encoding="utf-8"
    )
    resources = (DESKTOP / "resources/icons.qrc").read_text(encoding="utf-8")
    assert "QDialog#membershipSupportPrompt QLabel" in source
    assert "background: transparent" in source
    assert "Qt::ScrollBarAlwaysOff" in source
    for tier in ("supporter", "pro", "builder", "studio"):
        assert f":/app/membership/patreon-{tier}.png" in source
        assert f"membership/patreon-{tier}.png" in resources
