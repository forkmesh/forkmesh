import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT / "src") not in sys.path:
    sys.path.insert(0, str(ROOT / "src"))

import dashboard_shell  # noqa: E402

PUBLIC = ROOT / "public"
DASHBOARD = PUBLIC / "dashboard"
RUNNERS_JS = (DASHBOARD / "js" / "09-runners.js").read_text(encoding="utf-8")
VIEW = (DASHBOARD / "partials" / "views" / "runners.html").read_text(encoding="utf-8")
HEADER = (DASHBOARD / "partials" / "header.html").read_text(encoding="utf-8")
BUNDLE = (PUBLIC / "dashboard.js").read_text(encoding="utf-8")
CSS = (DASHBOARD / "tailwind.css").read_text(encoding="utf-8")


def function_source(name):
    match = re.search(
        r"^  (?:async )?function %s\(.*?(?=^  (?:async )?function |\Z)" % re.escape(name),
        RUNNERS_JS, re.M | re.S)
    assert match, name
    return match.group(0)


def test_setup_panel_offers_a_command_and_ssh_details():
    for marker in ("data-runner-setup-open", "data-runner-setup-name",
                   'data-runner-setup-tab="command"', 'data-runner-setup-tab="ssh"',
                   "data-runner-setup-mint", "data-runner-setup-copy",
                   "data-runner-setup-revoke", "data-runner-setup-desktop",
                   "data-runner-ssh-host", "data-runner-ssh-port",
                   "data-runner-ssh-user", "data-runner-ssh-probe",
                   "data-runner-ssh-trust", "data-runner-ssh-password",
                   "data-runner-ssh-key", "data-runner-ssh-install",
                   "data-runner-ssh-installs"):
        assert marker in VIEW, marker
    assert "never stored, logged or shown again" in VIEW
    assert "is used once to sign in and is not kept" in VIEW
    assert "trust on first use" in VIEW
    assert "ssh-keygen -lf /etc/ssh/ssh_host_ed25519_key.pub" in VIEW
    assert 'type="password"' in VIEW and 'autocomplete="off"' in VIEW
    assert "Set up a runner" in VIEW
    page = (PUBLIC / dashboard_shell.PAGES["runners"]["asset"]).read_text(encoding="utf-8")
    assert "data-runner-setup-open" in page


def test_command_flow_uses_the_token_endpoints_and_waits_for_registration():
    mint = function_source("mintRunnerSetupToken")
    assert '"/api/runners/setup-tokens", { name, via }' in mint
    show = function_source("showRunnerSetupCommand")
    assert "pre.textContent = minted.command" in show
    poll = function_source("pollRunnerSetupToken")
    assert "/api/runners/setup-tokens/${encodeURIComponent(token.id)}" in poll
    assert 'setup.state === "used"' in poll and "clearRunnerSetupCommand();" in poll
    revoke = function_source("revokeRunnerSetupToken")
    assert "/revoke`" in revoke


def test_ssh_flow_confirms_the_fingerprint_and_clears_secrets_before_sending():
    probe = function_source("probeRunnerSsh")
    assert '"/api/runners/ssh/probe", { host, port }' in probe
    assert "trust.checked = false" in probe
    install = function_source("installRunnerSsh")
    assert install.index("data-runner-ssh-trust") < install.index("/api/runners/ssh/install")
    assert install.index("clearRunnerSshSecrets();") < install.index("/api/runners/ssh/install")
    assert 'body.password = "";' in install
    clear = function_source("clearRunnerSshSecrets")
    for field in ("data-runner-ssh-password", "data-runner-ssh-key", "data-runner-ssh-passphrase"):
        assert field in clear
    assert "clearRunnerSshSecrets();" in function_source("initRunnerSetup")
    assert "console." not in RUNNERS_JS
    assert "localStorage" not in RUNNERS_JS and "sessionStorage" not in RUNNERS_JS


def test_desktop_handoff_keeps_the_password_on_the_users_computer():
    desktop = function_source("openRunnerSetupInDesktop")
    assert 'new URL("forkmesh://control/cloudflare")' in desktop
    assert 'url.searchParams.set("role", "runner")' in desktop
    assert 'url.searchParams.set("runnerToken", minted.token)' in desktop
    assert "password" not in desktop.lower()
    assert 'mintRunnerSetupToken("desktop")' in desktop


def test_attention_chip_on_every_page_links_to_the_sign_in():
    assert "data-runner-attention" in HEADER
    assert "data-runner-attention-text" in HEADER
    for page_id, meta in dashboard_shell.PAGES.items():
        page = (PUBLIC / meta["asset"]).read_text(encoding="utf-8")
        if "data-app-header" in page:
            assert "data-runner-attention" in page, page_id
    start = function_source("startRunnerAttention")
    assert "runnerAttention.started" in start
    load = function_source("loadRunnerAttention")
    assert '"/api/runners/attention"' in load
    summary = function_source("runnerAttentionSummary")
    assert '"/dashboard/runners?"' in summary and "&connect=1" in summary
    render = function_source("renderRunnerAttention")
    assert "runnersView.focus = { runner: summary.runner, provider: provider.id, connect: true };" in render
    assert RUNNERS_JS in BUNDLE
    for cls in (r"max-w-\[16rem\]", r"aria-selected\:bg-secondary", r"bg-amber-500\/10"):
        assert cls in CSS
