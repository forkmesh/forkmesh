#!/usr/bin/env python3
"""Static contracts for the account Patreon UI after office CTA removal."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PUBLIC = ROOT / "public"
SETTINGS = (
    PUBLIC / "dashboard" / "partials" / "views" / "settings.html"
).read_text(encoding="utf-8")
ACCOUNT_JS = (PUBLIC / "dashboard" / "js" / "04-account.js").read_text(
    encoding="utf-8"
)
SHELL = (ROOT / "src" / "dashboard_shell.py").read_text(encoding="utf-8")


def test_no_office_request_bundle_survives_in_the_dashboard_shell():
    # The office-request CTA lived on the retired /pricing page. The page is
    # gone; what has to stay gone is its bundle.
    assert '"pricing.js"' not in SHELL


def test_account_settings_expose_patreon_connect_controls():
    for marker in (
        'data-patreon-connection',
        'data-patreon-connect',
        'data-patreon-connection-state',
        'data-patreon-connection-status',
        "Connect Patreon",
        'href="https://www.patreon.com/16434219/join"',
    ):
        assert marker in SETTINGS

    for marker in (
        "function loadPatreonConnection(",
        "function startPatreonOAuth(",
        "function consumePatreonCallbackOutcome(",
        '/api/accounts/patreon',
        '/api/accounts/patreon/oauth/start',
        'cache: "no-store"',
        'Reconnect Patreon',
        'button.dataset.patreonConfigured = data.configured ? "true" : "false"',
        'button.disabled = true',
        'Use the Patreon memberships link below',
    ):
        assert marker in ACCOUNT_JS
