#!/usr/bin/env python3
"""The "Volunteer to mirror" button on External repositories & stubs (issue #680).

In the web app the button looked completely inert. The handler resolved the
node to volunteer from ``session.nodes`` and, for a web-only account with no
linked desktop node, fell back to the account's own name. The relay accepted
that (an account trivially "owns" itself), recorded a request no process could
ever serve, and the reloaded card looked identical: still "Volunteer to
mirror", no loading state, no confirmation. Only the tiny status pill changed,
and only on the first click.

Now a session without a mirror-capable node gets the desktop handoff modal
with concrete steps, a real node gets a busy state and a confirmation on the
card, failures are spelled out on the button, and the relay refuses a user
account volunteering itself so the two agree.
"""

import json
import subprocess
from pathlib import Path

from _dashboard_bundle import assembled_dashboard_js
from _dashboard_shell import assembled_dashboard_page

ROOT = Path(__file__).resolve().parents[1]
ENTRY_TEXT = (ROOT / "src" / "entry.py").read_text(encoding="utf-8")
MODAL = (ROOT / "public" / "dashboard" / "partials" / "new-repo-modal.html").read_text(
    encoding="utf-8")

BUNDLE = assembled_dashboard_js()


def _slice(start, end):
    return BUNDLE[BUNDLE.index(start):BUNDLE.index(end)]


NODE_NAME_FN = _slice("function externalMirrorNodeName()", "function externalRepositoryById(")
VOLUNTEER_FN = _slice("async function volunteerForExternalMirror(", "function externalMirrorHandoffSteps(")
STEPS_FN = _slice("function externalMirrorHandoffSteps(", "function openExternalMirrorHandoff(")
HANDOFF_FN = _slice("function openExternalMirrorHandoff(", "// New-repository flow (adhoc #30).")
CARD_FN = _slice("function externalRepositoryCard(", "function renderExternalRepositories(")


def _run_js(script):
    result = subprocess.run(
        ["node", "--input-type=module", "-e", script],
        capture_output=True, text=True, check=True)
    return json.loads(result.stdout)


def _node_name_for(session):
    script = (
        "const state = { session: %s };\n%s\n"
        "console.log(JSON.stringify(externalMirrorNodeName()));"
        % (json.dumps(session), NODE_NAME_FN))
    return _run_js(script)


def test_node_kind_session_volunteers_itself():
    assert _node_name_for({"kind": "node", "nodeName": "mirror-01", "nodes": []}) == "mirror-01"


def test_user_session_volunteers_its_first_linked_node():
    assert _node_name_for({
        "kind": "user", "nodeName": "sample-user", "nodes": ["", "home-box", "spare"],
    }) == "home-box"
    assert _node_name_for({
        "kind": "user", "nodeName": "sample-user", "nodes": [{"name": "home-box"}],
    }) == "home-box"


def test_web_only_user_never_volunteers_its_own_account_name():
    # The regression: this used to return "sample-user" and record a bogus request.
    assert _node_name_for({"kind": "user", "nodeName": "sample-user", "nodes": []}) == ""
    assert _node_name_for({"kind": "user", "nodeName": "sample-user"}) == ""
    assert _node_name_for(None) == ""


def test_missing_node_opens_the_desktop_handoff_instead_of_returning_silently():
    assert "if (!node) return;" not in VOLUNTEER_FN
    assert "openExternalMirrorHandoff(repository)" in VOLUNTEER_FN
    # And if the modal markup is ever absent, the button itself says why.
    assert "EXTERNAL_MIRROR_BUTTON_LABELS.desktop" in VOLUNTEER_FN
    # The signed-out branch no longer writes into a hint that lives inside a
    # closed modal.
    assert "setNewRepoHint" not in VOLUNTEER_FN


def test_request_shows_a_busy_state_and_reports_failures_on_the_button():
    assert 'button.setAttribute("aria-busy", "true")' in VOLUNTEER_FN
    assert "EXTERNAL_MIRROR_BUTTON_LABELS.busy" in VOLUNTEER_FN
    assert "EXTERNAL_MIRROR_BUTTON_LABELS.retry" in VOLUNTEER_FN
    assert "EXTERNAL_MIRROR_BUTTON_LABELS.signin" in VOLUNTEER_FN
    # A relay refusal of the node opens the same handoff naming that node.
    assert 'code === "eligible_owned_mirror_node_required"' in VOLUNTEER_FN
    assert "openExternalMirrorHandoff(repository, { node })" in VOLUNTEER_FN
    labels = _slice("const EXTERNAL_MIRROR_BUTTON_LABELS = {", "function setExternalMirrorButtonLabel(")
    for label in ("Volunteering…", "Could not volunteer — try again",
                  "Sign in again to volunteer", "Desktop node required"):
        assert label in labels


def test_success_is_confirmed_on_the_re_rendered_card():
    # Recorded before the reload so the fresh card reads it.
    assert "state.externalMirrorVolunteers.set(String(repositoryId), node);" in VOLUNTEER_FN
    assert VOLUNTEER_FN.index("externalMirrorVolunteers.set") < VOLUNTEER_FN.index(
        "await loadExternalRepositories({ fresh: true });")
    assert "externalMirrorVolunteers: new Map()" in BUNDLE
    # The card swaps the button for a confirmation naming the node.
    assert "data-external-mirror-volunteered=" in CARD_FN
    assert "Mirror requested ·" in CARD_FN
    assert "${canVolunteer && volunteeredNode ?" in CARD_FN
    assert "${canVolunteer && !volunteeredNode ?" in CARD_FN


def _steps_for(repository, node):
    script = (
        "const escapeHtml = (v) => String(v).replace(/[&<>\"']/g, (c) => "
        "({'&':'&amp;','<':'&lt;','>':'&gt;','\"':'&quot;',\"'\":'&#39;'}[c]));\n"
        "%s\nconsole.log(JSON.stringify(externalMirrorHandoffSteps(%s, %s)));"
        % (STEPS_FN, json.dumps(repository), json.dumps(node)))
    return _run_js(script)


def test_handoff_steps_walk_from_install_to_a_hosted_mirror():
    repo = {"name": "spidertest", "originalUrl": "https://github.com/acme/spidertest"}
    steps = _steps_for(repo, "")
    assert len(steps) == 5
    assert "Install the ForkMesh desktop app" in steps[0]
    assert 'href="/dashboard/settings/nodes"' in steps[0]
    assert "Remote clone URL" in steps[1]
    assert "https://github.com/acme/spidertest" in steps[2]
    assert "spidertest" in steps[2]
    assert "Publish" in steps[3]
    assert "Volunteer to mirror" in steps[4]


def test_handoff_steps_name_a_refused_node_instead_of_the_install_step():
    steps = _steps_for({"name": "x", "originalUrl": "https://example.test/x"}, "home-box")
    assert "home-box" in steps[0]
    assert "Install" not in steps[0]


def test_handoff_steps_escape_provider_data():
    steps = _steps_for({"name": "<img src=x onerror=1>", "originalUrl": "https://e.test/\"x"}, "")
    joined = "\n".join(steps)
    assert "<img" not in joined
    assert "&lt;img" in joined
    assert '"x' not in joined


def test_handoff_reuses_the_repositories_page_modal_with_its_own_copy():
    assert "setNewRepoModalOpen(true);" in HANDOFF_FN
    assert "setNewRepoHandoffOpen(true, {" in HANDOFF_FN
    assert "from your desktop node" in HANDOFF_FN
    assert "no linked desktop node" in HANDOFF_FN
    assert "is not an eligible mirror node for this account yet" in HANDOFF_FN
    # Returns false (so the caller can fall back) when the modal isn't shipped.
    assert 'if (!$("[data-new-repo-modal]") || !list) return false;' in HANDOFF_FN
    handoff = _slice("function setNewRepoHandoffOpen(open, copy = {})", "function newRepoSource()")
    assert "copy.title ||" in handoff and "copy.subtitle ||" in handoff
    assert "(open && copy.intro) || NEW_REPO_STEPS_INTRO" in handoff


def test_modal_intro_is_addressable_and_ships_on_the_repositories_page():
    assert "data-new-repo-steps-intro" in MODAL
    repos_page = assembled_dashboard_page("repos")
    assert "data-new-repo-steps-intro" in repos_page
    assert "data-external-repo-list" in repos_page
    # The steps panel still carries the install link the handoff relies on.
    assert 'href="/desktop"' in repos_page


def test_relay_refuses_a_user_account_volunteering_itself():
    fn = ENTRY_TEXT[
        ENTRY_TEXT.index("async def _repository_import_operator_eligible")
        : ENTRY_TEXT.index("async def _repository_import_moderator")
    ]
    assert '_account_kind(node_rec) != "node"' in fn
    assert "await _account_owns_node(env, actor, node)" in fn
    assert 'account.get("status") != "active"' in fn
