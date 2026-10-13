import re
from pathlib import Path

import pytest

from _dashboard_bundle import assembled_dashboard_js

ROOT = Path(__file__).resolve().parents[1]
JS = ROOT / "public" / "dashboard" / "js"
CONTENT = (JS / "06-repo-content.js").read_text(encoding="utf-8")
BOOT = (JS / "10-boot.js").read_text(encoding="utf-8")
CSS = (ROOT / "public" / "dashboard" / "tailwind.css").read_text(encoding="utf-8")


def _function(source, name):
    start = source.index("function %s(" % name)
    end = source.index("\n  }\n", start)
    return source[start:end]


def test_agent_modal_submit_sends_queued_screenshots():
    submit = _function(CONTENT, "handleRepoAgentNewSubmit")
    assert "attachments: agentModalAttachmentsPayload(form)" in submit
    assert "form._pendingAgentImages = [];" in submit
    payload = _function(CONTENT, "agentModalAttachmentsPayload")
    assert "mediaType" in payload and "data: match[2]" in payload
    assert "name: img.name" in payload


def test_agent_modal_has_attach_controls_in_every_shell():
    shells = [
        path for path in (ROOT / "public").rglob("*.html")
        if "data-repo-agent-new-form" in path.read_text(encoding="utf-8")
    ]
    assert ROOT / "public/dashboard/partials/header.html" in shells
    for path in shells:
        html = path.read_text(encoding="utf-8")
        for marker in (
            "data-repo-agent-new-file-input",
            "data-repo-agent-new-attach ",
            "data-repo-agent-new-attachments",
            'accept="image/png,image/jpeg,image/webp,image/gif"',
        ):
            assert marker in html, (path, marker)
    add = _function(CONTENT, "addAgentModalImage")
    assert "ORG_AGENT_IMAGE_TYPES.has(file.type)" in add
    chips = _function(CONTENT, "renderAgentModalChips")
    assert 'images.length ? "grid gap-2" : "hidden"' in chips


def test_error_actions_are_installed_at_boot():
    boot = _function(BOOT, "bootDashboard")
    assert boot.index("installErrorActions();") < boot.index("initSharedChrome()")
    install = _function(CONTENT, "installErrorActions")
    assert "MutationObserver" in install
    assert "handleErrorActionClick" in install


def test_both_buttons_and_their_targets():
    bar = _function(CONTENT, "errorActionsBarHtml")
    assert 'data-error-action="copy"' in bar and ">Copy<" in bar
    assert 'data-error-action="prompt"' in bar and ">Send to prompt<" in bar
    click = _function(CONTENT, "handleErrorActionClick")
    assert "copyTextToClipboard(text)" in click
    assert "sendTextToPrompt(errorActionsPrompt(text))" in click
    send = _function(CONTENT, "sendTextToPrompt")
    assert send.index("ForkMeshComposer?.insert") < send.index("setAgentModalOpen(true)")


def test_error_hosts_cover_alerts_and_composer_but_not_red_labels():
    qualifies = _function(CONTENT, "errorActionsHostQualifies") + _function(CONTENT, "errorActionsHostShaped")
    assert 'role=\\"alert\\"' in qualifies
    assert 'fmc-status[data-tone=\\"bad\\"]' in qualifies
    assert 'el.tagName === "P" || el.tagName === "DIV"' in qualifies
    assert "button, a, label" in qualifies


def test_no_error_left_in_an_uncopyable_alert_or_tooltip():
    bundle = assembled_dashboard_js()
    assert "window.alert(" not in bundle
    assert re.search(r"\balert\(", bundle.replace("showErrorNotice(", "")) is None
    assert "setCustomValidity(" not in CONTENT
    assert "data-org-agent-followup-error" in CONTENT
    notice = _function(CONTENT, "showErrorNotice")
    assert 'setAttribute("role", "alert")' in notice


@pytest.mark.parametrize("name", ["errorActionsBarHtml", "decorateErrorHost", "showErrorNotice"])
def test_button_classes_exist_in_the_purged_css(name):
    body = _function(CONTENT, name)
    lists = re.findall(r'(?:className = |const button = )"([^"]+)"', body)
    assert lists
    for cls in " ".join(lists).split():
        selector = "." + re.sub(r"([\[\]/:.])", r"\\\1", cls)
        assert re.search(re.escape(selector) + r"[{:,\s]", CSS), cls
