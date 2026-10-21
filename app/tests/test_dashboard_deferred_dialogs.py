"""Dialogs no page shows on load are left out of the page documents and added
by dashboard.js on first open (dashboard_bundle.DEFERRED_MARKUP).

The runtime half (injected on open, focus, one copy, placement) is
browser_tests/tests/dashboard-deferred-dialogs.spec.js."""

import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PUBLIC = ROOT / "public"
if str(ROOT / "src") not in sys.path:
    sys.path.insert(0, str(ROOT / "src"))

import dashboard_bundle  # noqa: E402
import dashboard_shell  # noqa: E402

JS = PUBLIC / "dashboard" / "js"
BUNDLE = (PUBLIC / "dashboard.js").read_text(encoding="utf-8")


def _read(path):
    return path.read_text(encoding="utf-8")


def _function(source, name):
    start = source.index("function %s(" % name)
    cursor = source.index("{", start)
    depth = 0
    while True:
        if source[cursor] == "{":
            depth += 1
        elif source[cursor] == "}":
            depth -= 1
            if depth == 0:
                return source[start:cursor + 1]
        cursor += 1


DEFERRED_HOOKS = (
    'id="agentModal"', 'id="notificationModal"', "data-profile-modal",
    "data-profile-about-modal", "data-agent-login-modal", "data-feedback-modal",
)


def _markup(name):
    return _read(PUBLIC / dashboard_bundle.deferred_markup_path(name))


def test_page_documents_leave_the_dialogs_out():
    pages = list(dashboard_shell.PAGES.values()) + list(
        dashboard_shell.PACKAGED_PAGE_COPIES.values())
    for meta in pages:
        html = _read(PUBLIC / meta["asset"])
        for hook in DEFERRED_HOOKS:
            assert hook not in html, (meta["asset"], hook)
        # What opens them stays in the first paint.
        assert 'id="agentModalToggle"' in html, meta["asset"]
        assert 'id="notificationToggle"' in html, meta["asset"]
    shell = _read(PUBLIC / "dashboard" / "shell.html")
    assert '<!--#include partial="modals"-->' not in shell


def test_each_template_is_in_the_bundle_verbatim():
    assert "function deferredMarkup(name)" in BUNDLE
    for name, spec in dashboard_bundle.DEFERRED_MARKUP.items():
        html = _markup(name)
        assert json.dumps(html) in BUNDLE, name
        marker = spec["marker"]
        assert marker.startswith("#")
        assert 'id="%s"' % marker[1:] in html, name
        if spec["after"]:
            header = _read(PUBLIC / "dashboard" / "partials" / "header.html")
            assert 'id="%s"' % spec["after"][1:] in header
    every = "".join(_markup(name) for name in dashboard_bundle.DEFERRED_MARKUP)
    for hook in DEFERRED_HOOKS:
        assert len(re.findall(re.escape(hook) + r"(?![\w-])", every)) == 1, hook
    # Dialog semantics travel with the markup.
    for match in re.finditer(r'role="dialog"', every):
        tag = every[every.rindex("<", 0, match.start()):every.index(">", match.end())]
        assert "aria-labelledby=" in tag


def test_the_bundle_is_composed_with_the_templates():
    def read(rel):
        return _read(PUBLIC / rel)

    composed, _ = dashboard_bundle.compose_split_from_reader(
        read, dashboard_shell.content_version)
    assert composed == BUNDLE
    loader = composed.index("function deferredMarkup(name)")
    assert loader < composed.index(_read(JS / "10-boot.js")[:200])


def test_every_open_adds_the_markup_first():
    helpers = _read(JS / "02-helpers.js")
    ensure = _function(helpers, "ensureDeferredMarkup")
    assert "document.querySelector(spec.marker)" in ensure
    assert "anchor.after(template.content)" in ensure
    assert "document.body.append(template.content)" in ensure
    assert "applyPlatformName(element)" in ensure
    assert "window.lucide?.createIcons()" in ensure
    opens = {
        "04-account.js": {"setProfileModalOpen": "dialogs",
                          "setProfileAboutModalOpen": "dialogs"},
        "08-repo-detail-network.js": {"setNotificationModalOpen": "dialogs"},
        "06-repo-content.js": {"showAgentLoginModal": "dialogs",
                               "setAgentModalOpen": "agent"},
        "09-feedback.js": {"setFeedbackModalOpen": "dialogs"},
    }
    for fragment, functions in opens.items():
        source = _read(JS / fragment)
        for name, group in functions.items():
            body = _function(source, name)
            call = 'ensureDeferredMarkup("%s")' % group
            assert call in body, name
            first_lookup = re.search(r'\$\("', body)
            assert body.index(call) < first_lookup.start(), name


def test_controls_of_added_dialogs_are_reached_through_the_document():
    feedback = _read(JS / "09-feedback.js")
    init = _function(feedback, "initDashboardFeedback")
    assert 'if (!$("[data-feedback-modal]")) return;' not in init
    assert 'document.addEventListener("submit"' in init
    assert 'closest?.("[data-feedback-form]")' in init
    network = _read(JS / "08-repo-detail-network.js")
    # A toggle click before the popover exists opens it.
    assert ('setAgentModalOpen($("#agentModal")?.classList.contains("hidden") ?? true);'
            in network)
    content = _read(JS / "06-repo-content.js")
    assert 'if (!$("#agentModalToggle")) return false;' in _function(content, "sendTextToPrompt")
