#!/usr/bin/env python3
"""Static contracts for the simplified auth pages."""

from pathlib import Path

from _dashboard_shell import assembled_dashboard, deferred_dialogs
from _dashboard_bundle import assembled_dashboard_js


PUBLIC = Path(__file__).resolve().parents[1] / "public"
AUTH_PAGES = (
    PUBLIC / "login.html",
    PUBLIC / "signup.html",
)


def _read(page: Path) -> str:
    # dashboard.js is built from ordered public/dashboard/js/*.js fragments.
    if page.name == "dashboard.js":
        return assembled_dashboard_js()
    return page.read_text(encoding="utf-8")


def test_auth_pages_use_simple_no_nav_shell():
    for page in AUTH_PAGES:
        html = _read(page)

        assert 'class="site-header"' not in html
        assert 'class="global-nav"' not in html
        assert 'id="theme-toggle"' not in html
        assert 'class="auth-shell"' in html
        assert 'class="auth-card"' in html


def test_auth_pages_keep_single_logo_home_link():
    for page in AUTH_PAGES:
        html = _read(page)

        assert html.count('class="brand" href="/" aria-label="ForkMesh home"') == 1
        assert 'class="brand-mark"' in html
        assert 'src="/assets/logo.svg"' in html
        assert 'aria-hidden="true"' in html


def _css_rule(html: str, selector: str) -> str:
    start = html.index(selector + " {")
    return html[start:html.index("}", start)]


def test_signup_uses_the_reference_card_control_scale():
    html = _read(PUBLIC / "signup.html")

    assert ".auth-button-primary" in html
    assert ".auth-input-sm" in html
    assert "height: 2.75rem;" in html
    assert "height: 2.875rem;" in html
    assert "border-radius: 0.625rem;" in html
    assert "height: 2.25rem;" not in html


def test_login_button_is_the_same_height_as_its_fields():
    login = _read(PUBLIC / "login.html")

    assert ".auth-input-sm" in login
    assert "height: 2.5rem;" in _css_rule(login, ".field input")
    assert "height: 2.5rem;" in _css_rule(login, ".auth-button-primary")
    assert "border-radius: 0.625rem;" in _css_rule(login, ".field input")
    assert "border-radius: 0.625rem;" in _css_rule(login, ".auth-button-primary")
    assert "height: 2.875rem;" not in login
    assert "height: 2.75rem;" not in login


def test_signup_uses_white_primary_actions_and_neutral_focus():
    html = _read(PUBLIC / "signup.html")

    assert "radial-gradient(circle at top, rgba(74, 222, 128" not in html
    assert "rgba(255, 255, 255, 0.16)" in html
    assert "background: #f5f5f5;" in html
    assert "color: #050505;" in html
    assert "border-color: rgba(255, 255, 255, 0.34);" in html
    assert "box-shadow: 0 0 0 2px rgba(255, 255, 255, 0.12);" in html


def test_login_has_no_glow_and_no_green_in_either_theme():
    login = _read(PUBLIC / "login.html")

    assert "radial-gradient" not in login
    assert "--auth-glow: none;" in login
    assert "background-image: none;" in _css_rule(login, ".auth-shell")
    for green in ("#1f883d", "#2ea043", "#3fb950", "31, 136, 61", "63, 185, 80", "74, 222, 128"):
        assert green not in login
    assert "var(--accent-bright)" not in login


def test_login_focus_ring_and_primary_action_are_neutral_in_both_themes():
    login = _read(PUBLIC / "login.html")
    dark = _css_rule(login, "body[data-fm-auth]")
    light = _css_rule(login, "html.light body[data-fm-auth]")

    assert "--auth-focus: #4d4d4d;" in dark
    assert "--auth-focus: #a3a3a3;" in light
    assert "--auth-action-bg: #f5f5f5;" in dark
    assert "--auth-action-bg: #222222;" in light
    assert "--auth-link: #f5f5f5;" in dark
    assert "--auth-link: #171717;" in light
    focus = _css_rule(login, ".field input:focus")
    assert "border-color: var(--auth-focus);" in focus
    assert "box-shadow: 0 0 0 3px var(--auth-focus-ring);" in focus


def test_login_card_is_a_frame_around_an_inset_panel():
    login = _read(PUBLIC / "login.html")

    card = login.index('<section class="auth-card"')
    body = login.index('<div class="auth-card-body">', card)
    foot = login.index('<div class="auth-card-foot">', body)
    assert card < body < foot < login.index("</section>", foot)
    assert "border-radius: 16px;" in _css_rule(login, ".auth-card")
    assert "background: var(--auth-card-bg);" in _css_rule(login, ".auth-card")
    assert "border-radius: 15px;" in _css_rule(login, ".auth-card-body")
    assert "background: var(--auth-panel-bg);" in _css_rule(login, ".auth-card-body")
    assert "border-top" not in _css_rule(login, ".auth-card-foot")


def test_login_autofill_keeps_the_field_colours():
    login = _read(PUBLIC / "login.html")

    assert ".field input:-webkit-autofill" in login
    assert "-webkit-text-fill-color: var(--auth-text);" in login
    assert "box-shadow: 0 0 0 2.5rem var(--auth-field-bg) inset;" in login


def test_signup_links_are_white_not_green():
    html = _read(PUBLIC / "signup.html")

    assert ".row-links a" in html
    assert "color: #ffffff;" in html
    assert "color: var(--accent-bright);" not in html
    assert "background: var(--accent-bright);" not in html
    assert ".auth-button-primary { color: var(--accent-bright)" not in html
    assert ".join-eyebrow {\n        display: inline-flex; align-items: center; gap: 8px;\n        color: var(--accent-bright);" not in html


def test_login_and_signup_cross_links_remain():
    login = _read(PUBLIC / "login.html")
    signup = _read(PUBLIC / "signup.html")

    assert 'href="/signup"' in login
    assert 'href="/login"' in signup
    assert 'href="/signup.html"' not in login
    assert 'href="/login.html"' not in signup


def test_signup_card_is_centered_vertically_like_login():
    signup = _read(PUBLIC / "signup.html")

    shell_css = signup[
        signup.index(".auth-shell {")
        : signup.index(".auth-card {")
    ]

    assert "place-items: center;" in shell_css
    assert "place-items: start center;" not in shell_css
    assert "padding: 28px 16px;" in shell_css
    assert "padding: 28px 16px 56px;" not in shell_css


def test_signup_is_single_step_email_password_name_form():
    signup = _read(PUBLIC / "signup.html")

    assert 'id="signup-form"' in signup
    assert 'id="node-name"' in signup
    assert 'id="acct-email"' in signup
    assert 'maxlength="254"' in signup
    assert 'id="acct-pass"' in signup
    assert 'id="signup-create"' in signup
    assert "Join ForkMesh" in signup
    assert "Add a Solana payout address later from your dashboard profile" not in signup
    assert 'id="step-account"' not in signup
    assert 'id="name-continue"' not in signup
    assert "Name reserved" not in signup


def test_signup_posts_single_signup_request_not_payment_or_reserve_flow():
    signup_js = _read(PUBLIC / "signup.js")

    assert 'api("/api/accounts/signup"' in signup_js
    assert 'api("/api/accounts/reserve"' not in signup_js
    assert 'api("/api/accounts/donation-address"' not in signup_js
    assert "solana" not in signup_js.lower()
    assert 'localStorage.setItem("forkmesh.session", JSON.stringify({' in signup_js


def test_signup_reconciles_state_after_history_restoration():
    signup_js = _read(PUBLIC / "signup.js")

    assert 'window.addEventListener("pageshow"' in signup_js
    assert "validateName();" in signup_js
    assert "updateCreateState();" in signup_js


def test_signup_rejects_malformed_email_before_submit_with_inline_error():
    signup = _read(PUBLIC / "signup.html")
    signup_js = _read(PUBLIC / "signup.js")

    assert 'id="acct-email"' in signup
    assert 'aria-describedby="email-hint"' in signup
    assert 'pattern="[^\\s@]+@' in signup
    assert 'id="email-hint" aria-live="polite"' in signup
    assert "EMAIL_LOCAL_RE" in signup_js
    assert "EMAIL_DOMAIN_RE" in signup_js
    assert 'local.includes("..")' in signup_js
    assert "nameOk && validEmail(email)" in signup_js
    assert 'emailInput.addEventListener("blur", () => validateEmail(true))' in signup_js
    assert 'if (!validateEmail(true))' in signup_js
    assert 'emailInput.setAttribute("aria-invalid"' in signup_js
    assert "domain ending like .com or .org" in signup_js


def test_login_rejects_malformed_email_before_calling_the_api():
    login_js = _read(PUBLIC / "login.js")
    guard = login_js.index("if (!validEmail(email))")

    # A bare "@" test let "test@" and "test@.com" through to the endpoint while
    # refusing "testgmail.com" locally, so the same class of input behaved two
    # ways (issue #622). Signup's rules now decide both, in the browser.
    assert 'email.includes("@")' not in login_js
    assert "EMAIL_LOCAL_RE" in login_js
    assert "EMAIL_DOMAIN_RE" in login_js
    assert 'local.includes("..")' in login_js
    assert guard < login_js.index('fetch("/api/accounts/login"')
    assert "domain ending like .com or .org" in login_js
    assert "domain ending like .com or .org" in _read(PUBLIC / "signup.js")


def test_dashboard_profile_has_email_verification_and_payout_wallet_controls():
    # Assert against the composed dashboard shell (see _dashboard_shell) and
    # the dialogs dashboard.js adds on first open.
    dashboard = assembled_dashboard() + deferred_dialogs()
    dashboard_js = _read(PUBLIC / "dashboard.js")

    assert "data-profile-settings-button" in dashboard
    assert "data-profile-modal" in dashboard
    assert "data-profile-email-status" in dashboard
    assert "data-profile-verify-email" in dashboard
    assert "data-profile-solana" in dashboard
    assert "data-profile-password" in dashboard
    assert "data-profile-save" in dashboard
    assert "/api/accounts/profile" in dashboard_js


def test_dashboard_profile_page_exposes_account_settings_and_danger_zone():
    dashboard = assembled_dashboard()
    dashboard_js = _read(PUBLIC / "dashboard.js")
    sidebar_start = dashboard.index("data-settings-sidebar")
    sidebar_end = dashboard.index("<main data-settings-main")
    settings_sidebar = dashboard[sidebar_start:sidebar_end]

    assert 'data-view="profile"' in dashboard
    assert "data-settings-layout" in dashboard
    assert "data-settings-account-header" in dashboard
    assert "data-settings-sidebar" in dashboard
    assert "data-settings-main" in dashboard
    assert "data-settings-profile-picture" in dashboard
    assert "Go to your personal profile" in dashboard
    assert "Public profile" in dashboard
    assert "Verification and payout" in settings_sidebar
    assert "Danger zone" in settings_sidebar
    for label in (
        "Accessibility",
        "Billing and licensing",
        "Emails",
        "Password and authentication",
        "SSH and GPG keys",
        "Credentials",
        "Enterprises",
        "Teams",
        "Moderation",
        "Code, planning, and automation",
        "Codespaces",
        "Packages",
        "Copilot",
    ):
        assert label not in settings_sidebar
    for section in (
        "public-profile",
        "account",
        "appearance",
        "notifications",
        "payout",
        "nodes",
        "organizations",
        "danger",
    ):
        assert f'data-settings-section-link="{section}"' in dashboard
        assert f'data-settings-section="{section}"' in dashboard
    assert "setSettingsSection(" in dashboard_js
    assert "[data-settings-section-link]" in dashboard_js
    assert "data-profile-page-avatar" in dashboard
    assert "data-profile-page-node-name" in dashboard
    assert "data-profile-page-email" in dashboard
    assert "data-profile-page-email-status" in dashboard
    assert "data-profile-page-verify-email" in dashboard
    assert "data-profile-page-solana" in dashboard
    assert "data-profile-page-password" in dashboard
    assert "data-profile-page-save" in dashboard
    assert "data-profile-rename-input" in dashboard
    assert "data-profile-rename-status" in dashboard
    assert "data-profile-rename-save" in dashboard
    assert "data-profile-delete-password" in dashboard
    assert "data-profile-delete-confirm" in dashboard
    assert "data-profile-delete-account" in dashboard
    assert "Delete account" in dashboard
    assert "Type DELETE" in dashboard


def test_dashboard_profile_page_exposes_public_profile_controls():
    dashboard = assembled_dashboard()
    dashboard_js = _read(PUBLIC / "dashboard.js")

    assert "data-profile-page-bio" in dashboard
    assert "data-profile-page-location" in dashboard
    assert "data-profile-page-timezone" in dashboard
    assert "data-profile-page-mastodon" in dashboard
    assert "data-profile-page-private" in dashboard
    assert "data-profile-links-editor" in dashboard
    assert "data-profile-link-label" in dashboard
    assert "data-profile-link-url" in dashboard
    assert "data-profile-txt-value" in dashboard
    assert "data-profile-public-password" not in dashboard
    assert "data-profile-public-save" in dashboard
    assert "Save public profile" in dashboard
    assert "profileBio" in dashboard_js
    assert "profileLocation" in dashboard_js
    assert "profileTimezone" in dashboard_js
    assert "mastodon" in dashboard_js
    assert "profilePrivate" in dashboard_js
    assert "profileLinks: collectProfileLinks()" in dashboard_js
    assert "savePublicProfile" in dashboard_js


def test_dashboard_public_email_dropdown_unlocks_for_a_verified_address():
    """Issue #605: the dropdown was authored disabled and never wired up.

    It stays disabled only while there is nothing publishable; a confirmed
    address makes it selectable, and the choice round-trips to the worker.
    """
    dashboard = assembled_dashboard()
    dashboard_js = _read(PUBLIC / "dashboard.js")

    assert "data-profile-public-email" in dashboard
    assert "function renderProfilePublicEmail(session)" in dashboard_js
    assert 'Boolean(session?.emailVerified) && email.includes("@")' in dashboard_js
    assert "select.disabled = !verified;" in dashboard_js
    assert 'PROFILE_PUBLIC_EMAIL_READY_CLASS' in dashboard_js
    assert '["", "Don\'t show my email"]' in dashboard_js
    assert "publicEmail && !publicEmail.disabled" in dashboard_js
    assert 'publicEmail: (publicEmail.value || "").trim()' in dashboard_js
    assert "bad_public_email:" in dashboard_js
    # The saved address is echoed back through the session payload so the
    # dropdown reopens on the stored choice instead of the placeholder.
    assert 'publicEmail: body.publicEmail ?? base.publicEmail ?? ""' in dashboard_js
    assert 'publicEmail: body.publicEmail || ""' in _read(PUBLIC / "login.js")


def test_public_profile_shows_a_published_email_row():
    dashboard = assembled_dashboard()
    dashboard_js = _read(PUBLIC / "dashboard.js")

    assert "data-profile-email-row" in dashboard
    assert "data-profile-email-text" in dashboard
    assert "renderProfilePublicEmailRow(session)" in dashboard_js
    assert "row.hidden = !publicEmail;" in dashboard_js
    assert "link.href = publicEmail ? `mailto:${publicEmail}` : \"#\";" in dashboard_js


def test_public_profile_username_uses_session_name_and_anchors_its_status():
    settings = (PUBLIC / "dashboard" / "settings" / "index.html").read_text(
        encoding="utf-8")
    account_js = (PUBLIC / "dashboard" / "js" / "04-account.js").read_text(
        encoding="utf-8")

    assert "renameInput.value = session?.nodeName || \"\";" in account_js
    username = settings.index("data-profile-rename-input")
    status = settings.index("data-profile-rename-status")
    password = settings.index("data-profile-rename-password")
    assert username < status < password


def test_dashboard_profile_save_supersedes_stale_cached_profile_reads():
    dashboard_js = _read(PUBLIC / "dashboard.js")

    # The owner hydration must bypass both the browser cache and the Worker's
    # viewer-less public-profile edge cache, or a pre-edit response can replace
    # the successful POST response on reload (issue #602).
    assert "?viewer=${encodedName}" in dashboard_js
    assert "{ fresh: true }" in dashboard_js
    assert "const mutationVersion = state.profileMutationVersion;" in dashboard_js
    assert "state.profileMutationVersion += 1;" in dashboard_js
    assert "mutationVersion !== state.profileMutationVersion" in dashboard_js

    # The delegated click handler is the single owner of this button. A direct
    # listener as well would issue two concurrent profile writes per click.
    assert 'addEventListener("click", saveProfile)' not in dashboard_js


def test_dashboard_profile_timezone_uses_supported_timezone_select():
    dashboard = assembled_dashboard()
    dashboard_js = _read(PUBLIC / "dashboard.js")

    assert "<select data-profile-page-timezone" in dashboard
    assert "input data-profile-page-timezone" not in dashboard
    assert "Use browser time zone" in dashboard
    assert 'Intl.supportedValuesOf("timeZone")' in dashboard_js
    assert "profileTimezoneGmtLabel" in dashboard_js
    assert "profileTimezoneGmtOffset(zone)" in dashboard_js
    assert "GMT+00:00" in dashboard_js
    assert "renderProfileTimezoneOptions(session)" in dashboard_js


def test_dashboard_profile_page_js_checks_availability_renames_and_deletes_account():
    dashboard_js = _read(PUBLIC / "dashboard.js")

    assert "renderProfilePage(session)" in dashboard_js
    assert "checkNodeNameAvailability" in dashboard_js
    assert 'fetch(`/api/accounts/${encodeURIComponent(candidate)}`' in dashboard_js
    assert "renameNodeName" in dashboard_js
    assert "newNodeName" in dashboard_js
    assert "refreshRepositories()" in dashboard_js
    assert "deleteAccount" in dashboard_js
    assert "deleteAccount: true" in dashboard_js
    assert "function clearForkMeshBrowserState()" in dashboard_js
    assert 'key?.startsWith("forkmesh.")' in dashboard_js
    assert "logout();" in dashboard_js
    # Reaching settings is a real navigation now; the account panels are
    # settings sub-tabs restored from the URL. Patreon OAuth callbacks force
    # the account tab so bounded outcomes are visible.
    assert "function initSettingsPage()" in dashboard_js
    assert "settingsSectionFromPath()" in dashboard_js
    assert 'patreonOutcome' in dashboard_js


def test_login_explains_disabled_legacy_accounts():
    login_js = _read(PUBLIC / "login.js")

    assert 'body.error === "account_disabled"' in login_js
    assert "This account has been disabled." in login_js


def test_login_password_toggle_survives_a_mixed_version_document():
    login_js = _read(PUBLIC / "login.js")

    assert "if (password && passwordToggle)" in login_js
    assert "if (showIcon) showIcon.hidden" in login_js
    assert "if (hideIcon) hideIcon.hidden" in login_js


def test_login_persists_returned_session_details():
    login_js = _read(PUBLIC / "login.js")

    assert 'localStorage.setItem("forkmesh.session", JSON.stringify({' in login_js
    assert "nodeName: body.nodeName" in login_js
    assert "email: body.email" in login_js
    assert "status: body.status" in login_js
    assert "pubkey: body.pubkey" in login_js
    assert "emailVerified: Boolean(body.emailVerified)" in login_js
    assert "isAdmin: Boolean(body.isAdmin)" in login_js
    assert 'solana: body.solana || ""' in login_js
    assert "hasPayoutAddress: Boolean(body.hasPayoutAddress)" in login_js
    assert 'location.protocol === "https:"' in login_js
    assert "body.sessionToken &&" in login_js
    assert "window.ForkMeshAPI?.isSameSite !== false" in login_js
    assert '? "cookie"' in login_js
    assert ': body.sessionToken || ""' in login_js
    assert 'profileBio: body.profileBio || ""' in login_js
    assert 'profileAbout: body.profileAbout || body.profileReadme || ""' in login_js
    assert 'profileLocation: body.profileLocation || ""' in login_js
    assert 'profileTimezone: body.profileTimezone || ""' in login_js
    assert "profileFollowers: Number(body.followers) || 0" in login_js
    assert "profileFollowing: Number(body.following) || 0" in login_js
    assert "profileMirrorCount: Number(body.mirrorCount) || 0" in login_js
    assert "createdAt: Number(body.createdAt) || 0" in login_js
    assert "referralClicks: Number(body.referralClicks) || 0" in login_js
    assert "profilePrivate: Boolean(body.profilePrivate)" in login_js
    assert "followersPublic: Boolean(body.followersPublic)" in login_js
    assert 'mastodon: body.mastodon || ""' in login_js
    assert "emailNotifications: body.emailNotifications !== false" in login_js
    assert "notificationPreferences:" in login_js
    assert "accountSnapshotVersion: 1" in login_js


def test_dashboard_reuses_login_snapshot_and_falls_back_for_legacy_sessions():
    dashboard_js = _read(PUBLIC / "dashboard.js")

    assert "async function hydrateCanonicalProfile(session, { force = false } = {})" in dashboard_js
    assert "canonicalProfileReady = hydrateCanonicalProfile(session)" in dashboard_js
    hydrate = dashboard_js[
        dashboard_js.index("async function hydrateCanonicalProfile"):
        dashboard_js.index("function legacyRedirectTarget")
    ]
    # Login returns the complete canonical payload, so normal dashboard
    # navigation must stop before the fallback account request. Old/cross-origin
    # sessions lack the marker and take that fallback exactly once.
    marker_guard = (
        "if (!force && Number(session?.accountSnapshotVersion) >= 1) return session;"
    )
    assert marker_guard in hydrate
    assert hydrate.index(marker_guard) < hydrate.index("const mutationVersion")
    assert "accountSnapshotVersion: 1" in dashboard_js
    # The stored/fallback snapshot is also the authority on email verification,
    # so an unverified account leaves before notifications are fetched.
    assert "if (bounceToVerification(nextSession)) return;" in dashboard_js
    assert "return loadNotifications();" in dashboard_js
    assert '`/api/accounts/${encodedName}?viewer=${encodedName}`' in dashboard_js
    assert "fresh: true" in dashboard_js
    assert "writeSession(nextSession)" in dashboard_js
    # An explicit refresh after a node-link mutation still bypasses the login
    # snapshot so the newly linked identity is visible immediately.
    assert "refreshPublicProfile(state.session, { force: true })" in dashboard_js


def test_dashboard_profile_hydration_cannot_restore_stale_saved_values():
    dashboard_js = _read(PUBLIC / "dashboard.js")
    hydrate = dashboard_js[
        dashboard_js.index("async function hydrateCanonicalProfile"):
        dashboard_js.index("function legacyRedirectTarget")
    ]
    post = dashboard_js[
        dashboard_js.index("async function postProfile"):
        dashboard_js.index("async function refreshRepositories")
    ]

    # The viewer-specific lookup skips the Worker's manually managed public
    # profile cache. no-store also prevents Chrome from replaying the old
    # viewer-less response after a successful profile POST and reload.
    assert "?viewer=${encodedName}" in hydrate
    assert "fresh: true" in hydrate
    assert "cacheBust: false" in hydrate

    # A GET already in flight when the POST succeeds must not repaint its old
    # values over the endpoint's newly persisted response.
    assert "const mutationVersion = state.profileMutationVersion" in hydrate
    assert "mutationVersion !== state.profileMutationVersion" in hydrate
    assert "!response.ok || body.ok !== true" in post
    assert "state.profileMutationVersion += 1" in post


def test_dashboard_public_profile_edits_do_not_prompt_for_password():
    dashboard = assembled_dashboard()
    dashboard_js = _read(PUBLIC / "dashboard.js")

    assert "data-profile-about-password" not in dashboard
    assert "data-profile-public-password" not in dashboard
    assert "Required to save about changes" not in dashboard
    assert "Required for profile changes" not in dashboard
    assert 'profilePassword("[data-profile-about-password]")' not in dashboard_js
    assert 'profilePassword("[data-profile-public-password]")' not in dashboard_js
    assert "sessionToken: state.session?.sessionToken || \"\"" in dashboard_js


def test_login_has_forgot_password_link():
    login = _read(PUBLIC / "login.html")

    assert 'href="/forgot-password"' in login
    assert "Forgot" in login


def test_forgot_password_page_exists_and_links_back():
    forgot = _read(PUBLIC / "forgot-password.html")

    assert 'id="identifier"' in forgot
    assert 'id="forgot-btn"' in forgot
    assert 'src="/forgot-password.js' in forgot
    assert 'href="/login"' in forgot


def test_forgot_password_js_posts_to_api():
    forgot_js = _read(PUBLIC / "forgot-password.js")

    assert 'fetch("/api/accounts/forgot-password"' in forgot_js
    assert '"POST"' in forgot_js
    assert "identifier" in forgot_js


def test_reset_password_page_exists():
    reset = _read(PUBLIC / "reset-password.html")

    assert 'id="password"' in reset
    assert 'id="confirm"' in reset
    assert 'id="reset-btn"' in reset
    assert 'src="/reset-password.js' in reset
    assert 'href="/login"' in reset


def test_reset_password_js_posts_to_api_and_redirects():
    reset_js = _read(PUBLIC / "reset-password.js")

    assert 'fetch("/api/accounts/reset-password"' in reset_js
    assert '"POST"' in reset_js
    assert "node" in reset_js
    assert "token" in reset_js
    assert "exp" in reset_js
    assert 'location.href = "/login"' in reset_js


def test_worker_handles_forgot_and_reset_password_routes():
    entry = (PUBLIC.parent / "src" / "entry.py").read_text(encoding="utf-8")

    assert '"/api/accounts/forgot-password"' in entry
    assert "_account_forgot_password" in entry
    assert '"/api/accounts/reset-password"' in entry
    assert "_account_reset_password" in entry


def test_login_exposes_localhost_only_demo_credentials():
    login = _read(PUBLIC / "login.html")
    login_js = _read(PUBLIC / "login.js")

    assert 'id="demo-credentials"' in login
    assert "demo@forkmesh.local" in login
    assert "forkmesh-demo" in login
    assert 'const DEMO_EMAIL = "demo@forkmesh.local";' in login_js
    assert 'const DEMO_PASSWORD = "forkmesh-demo";' in login_js
    assert 'location.hostname === "localhost"' in login_js
    assert 'location.hostname === "127.0.0.1"' in login_js
    assert 'fetch("/api/accounts/login"' in login_js
    assert "email.toLowerCase() === DEMO_EMAIL" not in login_js
    assert 'nodeName: "demo-node"' not in login_js
    assert '"/dashboard.html"' not in login_js


def test_login_defaults_to_the_dashboard_without_an_intermediate_homepage():
    login_js = _read(PUBLIC / "login.js")

    assert ': nextPath() || referrerPath() || "/dashboard";' in login_js


def test_login_credential_fields_live_in_one_real_form():
    """A form-less login is one Chrome declines to autofill (adhoc #133).

    The browser pools form-less inputs into a synthetic form and then offers
    nothing when the email box is clicked, so the saved credential is
    unreachable. Same defect class as the dashboard password fields in #671.
    """
    login = _read(PUBLIC / "login.html")

    start = login.index('<form id="login-form"')
    form = login[start:login.index("</form>", start)]
    assert 'id="email" name="email" type="email" autocomplete="username"' in form
    assert 'id="password" name="password" type="password" autocomplete="current-password"' in form
    assert 'id="totp" name="totp"' in form
    assert 'id="login-btn" type="submit"' in form
    # One form only: a second one would split the username/password pair.
    assert login.count("<form") == 1


def test_login_submits_the_form_with_a_pre_form_fallback():
    login_js = _read(PUBLIC / "login.js")

    submit = login_js.index('form.addEventListener("submit"')
    assert 'const form = $("#login-form");' in login_js
    assert "event.preventDefault();" in login_js[submit:submit + 200]
    # A cached pre-form document still has a plain button and no implicit
    # submission, so the click/Enter wiring stays as the fallback branch.
    fallback = login_js.index('btn.addEventListener("click", submitLogin);')
    assert fallback > submit
    assert 'if (e.key === "Enter") submitLogin();' in login_js[fallback:fallback + 300]
    assert "login().catch(" in login_js
