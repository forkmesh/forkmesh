#!/usr/bin/env python3
"""Header "Reconnect node" affordance next to the profile avatar.

A signed-in user account with no linked node has nothing authenticated to drain
its issues/chats/etc. to a desktop, so the header surfaces a re-link shortcut to
the Settings > Nodes flow. It stays hidden for guests, node sessions, and when
the link state is unknown (so it never false-alarms).

Issue #640: the shortcut used to drop web-only users on an unexplained settings
page whose only feedback was "Enter a valid node ID". It now deep-links to the
Nodes tab, opens with a callout saying why, spells out both accepted node-ID
forms with examples, and offers a path for someone who has no node at all.
"""

from _dashboard_shell import assembled_dashboard
from _dashboard_bundle import assembled_dashboard_js


DASHBOARD = assembled_dashboard()
DASHBOARD_JS = assembled_dashboard_js()

# The Nodes tab of the settings view, up to the section that follows it.
NODES_SECTION = DASHBOARD[
    DASHBOARD.index('data-settings-section="nodes"')
    : DASHBOARD.index('data-settings-section="organizations"')
]


def test_header_has_hidden_reconnect_button_by_the_avatar():
    # Rendered right before the profile-settings avatar button, hidden until the
    # profile renderer decides a node link is missing.
    assert "data-node-reconnect" in DASHBOARD
    reconnect = DASHBOARD[
        DASHBOARD.index("data-node-reconnect")
        : DASHBOARD.index("data-profile-settings-button")
    ]
    # Straight to the Nodes tab (a bare /dashboard/settings opens public-profile,
    # which is the "unexplained redirect" of issue #640), carrying the marker the
    # settings page reads to explain the arrival.
    assert 'href="/dashboard/settings/nodes?from=reconnect"' in reconnect
    assert "hidden" in reconnect
    assert "Reconnect node" in reconnect


def test_reconnect_visibility_gated_on_missing_linked_node():
    gate = DASHBOARD_JS[
        DASHBOARD_JS.index("function nodeNeedsReconnect")
        : DASHBOARD_JS.index("function renderProfile")
    ]
    # Guests / node sessions never see it.
    assert 'if (!signedIn || session.kind === "node") return false;' in gate
    # Only when the account affirmatively has zero linked nodes (present + empty).
    assert "Array.isArray(session.nodes) && session.nodes.length === 0" in gate


def test_render_profile_toggles_the_reconnect_button():
    assert '$("[data-node-reconnect]")' in DASHBOARD_JS
    assert 'reconnect.classList.toggle("hidden", !show);' in DASHBOARD_JS
    assert "const show = nodeNeedsReconnect(session);" in DASHBOARD_JS
    assert "renderNodeReconnect(session);" in DASHBOARD_JS


def test_settings_page_opens_the_nodes_tab_for_a_reconnect_arrival():
    # The marker is honoured even on a bare /dashboard/settings?from=reconnect
    # URL, so the click can never land on public-profile with no explanation.
    assert 'get("from") === "reconnect"' in DASHBOARD_JS
    assert "function arrivedFromNodeReconnect" in DASHBOARD_JS
    init = DASHBOARD_JS[
        DASHBOARD_JS.index("function initSettingsPage")
        : DASHBOARD_JS.index("async function initRepoPage")
    ]
    assert "arrivedFromNodeReconnect()" in init
    assert '"nodes"' in init


def test_nodes_tab_opens_with_a_callout_explaining_the_arrival():
    assert "data-node-reconnect-callout" in NODES_SECTION
    callout = NODES_SECTION[
        NODES_SECTION.index("data-node-reconnect-callout")
        : NODES_SECTION.index("data-link-grant-row")
    ]
    # Hidden until the panel decides to show it, and it says what the missing
    # link actually costs rather than only that something is missing.
    assert "hidden" in callout
    assert "No node is linked to this account yet" in callout
    assert "Reconnect node" in callout
    # Revealed by the same zero-nodes state that shows the header shortcut, or
    # by the arrival marker when the session payload has not landed yet.
    assert "nodeNeedsReconnect(session) || arrivedFromNodeReconnect()" in DASHBOARD_JS
    assert '$("[data-node-reconnect-callout]")' in DASHBOARD_JS


def test_nodes_tab_explains_what_a_node_id_is_with_both_accepted_formats():
    assert "What is a node ID?" in NODES_SECTION
    # Name form, with an example, and the key form with a full-length one so the
    # 43-character rule can be eyeballed against a paste.
    assert "mirror-01" in NODES_SECTION
    assert "43-character" in NODES_SECTION or "43 characters" in NODES_SECTION
    example = "J2MJT8PgCi665wAlfuHdmpl-WDH2xB6z3Tp6T0OPsOs"
    assert len(example) == 43
    assert example in NODES_SECTION
    # And where to obtain it.
    assert "Copy node ID" in NODES_SECTION
    # The field points at that explainer for screen readers and stops suggesting
    # the placeholder "node-name" is itself a format.
    assert 'aria-describedby="claim-node-id-help"' in NODES_SECTION
    assert 'id="claim-node-id-help"' in NODES_SECTION


def test_nodes_tab_offers_a_path_for_web_only_users():
    assert "data-no-node-help" in NODES_SECTION
    help_block = NODES_SECTION[NODES_SECTION.index("data-no-node-help"):]
    assert 'href="/desktop"' in help_block
    assert "curl -fsSL https://forkmesh.com/install.sh | bash" in help_block
    assert "Link this machine to your account" in help_block


def test_invalid_node_id_feedback_names_the_form_that_was_missed():
    # The generic message is gone from both the client-side guard and the
    # server-error map.
    assert '"Enter a valid node ID."' not in DASHBOARD_JS
    assert "function claimNodeIdHint" in DASHBOARD_JS
    assert 'setProfilePageHint("[data-claim-node-status]", claimNodeIdHint(nodeId), "bad");' in DASHBOARD_JS
    assert "invalid_node_id: claimNodeIdHint(nodeId)," in DASHBOARD_JS
    hint = DASHBOARD_JS[
        DASHBOARD_JS.index("function claimNodeIdHint")
        : DASHBOARD_JS.index("async function claimNode()")
    ]
    # An email, a mistyped name, a truncated key and a padded paste each get
    # their own answer.
    assert 'nodeId.includes("@")' in hint
    assert "is exactly 43 characters, unpadded" in hint
    assert 'Drop the trailing \\"=\\" padding.' in hint
    assert "starts with a letter" in hint
    # "no such node" points at the same install path instead of dead-ending.
    assert 'has registered with the relay' in DASHBOARD_JS
