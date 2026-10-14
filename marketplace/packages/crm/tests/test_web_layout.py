"""Dashboard CRM page layout contracts: header, contact table, detail card."""

import re
from pathlib import Path


CRM_PACKAGE = Path(__file__).resolve().parents[1]
WEB_VIEW = (CRM_PACKAGE / "content" / "web" / "view.html").read_text(
    encoding="utf-8")
WEB_SCRIPT = (CRM_PACKAGE / "content" / "web" / "page.js").read_text(
    encoding="utf-8")


def _header():
    return WEB_VIEW.split("<header", 1)[1].split("</header>", 1)[0]


def test_the_header_has_one_primary_action_and_a_more_menu():
    header = _header()
    # Add contact is the only green button on the page header.
    assert header.count("bg-primary") == 1
    assert re.search(r"<button data-crm-contact-new[^>]*bg-primary", header)
    # Weekly chores live behind a native disclosure menu, still wired to the
    # same hooks the page script already drives.
    menu = header.split("<details data-crm-actions", 1)[1].split("</details>", 1)[0]
    for hook in ("data-crm-import", "data-crm-export",
                 "data-crm-zoho-sync", "data-crm-open-gmail",
                 "data-crm-open-zoho", "data-crm-extension-install",
                 "data-crm-extensions-open", "data-crm-settings-open"):
        assert hook in menu, hook
    # Refresh is not a weekly chore any more: it is the only way to see a
    # teammate's change, so it sits on the header itself.
    assert "data-crm-refresh" not in menu
    assert re.search(r"<button data-crm-refresh[^>]*>", header)
    assert "function wireCrmActionsMenu()" in WEB_SCRIPT
    # The label still wraps the file input, and the menu waits a tick before
    # closing so the picker opens.
    assert "setTimeout(close)" in WEB_SCRIPT
    assert "data-crm-actions-badge" in header
    assert "crmVersionBehind(installed, CRM_EXTENSION_MINIMUM)" in WEB_SCRIPT


def test_the_summary_is_one_line_instead_of_four_stat_tiles():
    header = _header()
    for key in ("contacts", "customers", "sent", "failed"):
        assert f"data-crm-stat-{key}" in header, key
        assert WEB_VIEW.count(f"data-crm-stat-{key}") == 1, key
    assert "text-2xl" not in WEB_VIEW
    assert '"text-destructive", Number(summary.failed || 0) > 0' in WEB_SCRIPT


def test_notices_use_a_neutral_border():
    notice = re.search(r"<div data-crm-notice[^>]*>", WEB_VIEW).group(0)
    assert "border-primary" not in notice


def test_contact_cells_read_as_text_and_edit_through_the_same_path():
    # Cells are still the inline-edit inputs the change handler saves, styled
    # to read as text until hovered or focused.
    assert 'class="crm-cell"' in WEB_SCRIPT
    inline = WEB_SCRIPT.split("function crmInlineContactField(", 1)[1].split("\n  }\n", 1)[0]
    assert "bg-background" not in inline
    assert "[data-view=\"crm\"] .crm-cell:focus" in WEB_VIEW
    assert ".crm-row:not(:hover) .crm-cell:not(:focus)::placeholder" in WEB_VIEW
    # Row actions appear on hover or keyboard focus, and always on touch.
    assert ".crm-row:focus-within .crm-row-actions" in WEB_VIEW
    assert "@media (pointer: coarse) { [data-view=\"crm\"] .crm-row-actions" in WEB_VIEW


def test_a_layout_with_nothing_visible_starts_from_the_default_columns():
    # A pipeline tab before anyone shapes it, or a sheet saved with every
    # column hidden, starts from a readable default set.
    assert "const fresh = !stored.some((column) => !column.hidden);" in WEB_SCRIPT
    defaults = WEB_SCRIPT.split("const CRM_DEFAULT_COLUMNS = new Set([", 1)[1]
    defaults = set(re.findall(r'"([A-Za-z]+)"', defaults.split("]", 1)[0]))
    assert defaults == {"name", "email", "company", "title", "flow", "stage",
                        "owner", "tags", "lastContactedAt"}
    assert "data-crm-columns" in WEB_VIEW
    assert "function openCrmColumnsMenu(event)" in WEB_SCRIPT
    menu = WEB_SCRIPT.split("function openCrmColumnsMenu(event)", 1)[1].split("\n  }\n", 1)[0]
    assert "crmFieldMenuItems()" in menu


def test_contacts_show_avatars_stage_tones_warmth_and_profile_links():
    for helper in ("function crmAvatar(contact", "function crmStageTone(contact)",
                   "function crmWarmth(ms)", "function crmQuietTag(contact)",
                   "function crmProfileTiles(contact, columns)"):
        assert helper in WEB_SCRIPT, helper
    for tone in ("new", "open", "won", "closed"):
        assert f".crm-tone-{tone}" in WEB_VIEW, tone
    # Profile links are only ever https and open outside the dashboard.
    assert 'new URL(url).protocol === "https:"' in WEB_SCRIPT
    tiles = WEB_SCRIPT.split("function crmProfileTiles(", 1)[1].split("\n  }\n", 1)[0]
    assert 'target="_blank" rel="noopener noreferrer"' in tiles
    assert "escapeHtml(url)" in tiles


def test_mastodon_cells_link_to_the_profile_on_the_handles_instance():
    # "@grace@mastodon.social" is https://mastodon.social/@grace: the instance
    # is the host and the name goes at the end of the path.
    helper = WEB_SCRIPT.split("function crmMastodonUrl(text)", 1)[1].split("\n  }\n", 1)[0]
    assert "`https://${handle[2]}/@${handle[1]}`" in helper
    assert "mastodon: () => crmMastodonUrl(text)," in WEB_SCRIPT
    control = WEB_SCRIPT.split("function crmContactControl(", 1)[1].split("\n  }\n", 1)[0]
    assert 'crmProfileUrl(column.field, contact.mastodon)' in control
    assert control.count('target="_blank" rel="noopener noreferrer"') == 2
    assert "a.crm-link" in WEB_VIEW


def test_contacts_can_be_laid_out_as_a_stage_board():
    assert re.search(r'<div data-crm-layout role="group" aria-label="Layout"', WEB_VIEW)
    assert 'data-crm-layout-option="table"' in WEB_VIEW
    assert 'data-crm-layout-option="board"' in WEB_VIEW
    assert "<div data-crm-board hidden" in WEB_VIEW
    assert "<div data-crm-table " in WEB_VIEW
    # The choice is this browser's, per organization, and survives storage
    # being unavailable.
    assert "`forkmesh-crm-layout:${state.crmView.organization}`" in WEB_SCRIPT
    layout = WEB_SCRIPT.split("function crmReadLayout()", 1)[1].split("\n  }\n", 1)[0]
    assert "try {" in layout and "catch" in layout
    # One flow per board: the pipeline tab's, the filter's, or the most common.
    flow = WEB_SCRIPT.split("function crmBoardFlow(contacts)", 1)[1].split("\n  }\n", 1)[0]
    assert 'crmActiveView()?.flow || String($(`[data-crm-flow]`)?.value || "")' in flow
    assert "in other flows" in WEB_SCRIPT


def test_dragging_a_board_card_saves_its_stage_and_keyboard_opens_the_card():
    board = WEB_SCRIPT.split("function renderCrmBoard(contacts)", 1)[1].split("\n  }\n\n", 1)[0]
    assert "data-crm-board-stage" in board
    assert 'role="button" tabindex="0"' in board
    # Shared sheets are read-only, so their cards are not draggable.
    assert '${shared ? "" : \' draggable="true"\'}' in board
    for event in ("dragstart", "dragover", "drop", "dragend"):
        assert f'board?.addEventListener("{event}"' in WEB_SCRIPT, event
    assert "void saveCrmContactStage(id, column.dataset.crmBoardStage);" in WEB_SCRIPT
    assert 'event.key === "Enter" || event.key === " "' in WEB_SCRIPT
    assert "async function saveCrmContactStage(id, stage)" in WEB_SCRIPT
    assert "CRM_BOARD_COLUMN_LIMIT = 50" in WEB_SCRIPT
    # Board, table and phone cards are switched with the hidden attribute, which
    # their own display rules (.crm-board, sm:block, grid) would otherwise beat.
    assert '[data-view="crm"] [hidden] { display: none !important; }' in WEB_VIEW


def test_contacts_inbox_outreach_and_team_are_separate_tabs():
    assert re.search(r'<div data-crm-sections data-crm-org-only role="tablist" aria-label="CRM sections"', WEB_VIEW)
    app = WEB_VIEW.split("<div data-crm-app", 1)[1]
    panels = re.findall(r'<div data-crm-section="([a-z]+)" id="crm-section-\1" role="tabpanel" aria-labelledby="crm-section-tab-\1"', app)
    assert panels == ["contacts", "inbox", "outreach", "team"]
    for section in ("contacts", "inbox", "outreach", "team"):
        assert f'data-crm-section-tab="{section}"' in WEB_VIEW, section

    def panel(name):
        return app.split(f'<div data-crm-section="{name}"', 1)[1].split("<div data-crm-section=", 1)[0]

    assert "data-crm-contact-list" in panel("contacts")
    assert "data-crm-email-list" in panel("inbox")
    assert "data-crm-mail-groups" in panel("inbox")
    assert "data-crm-outreach-form" in panel("outreach")
    assert "data-crm-team-board" in panel("team")
    assert "data-crm-team-chart" in panel("team")
    assert "data-crm-team-table" in panel("team")
    # The open tab is in the address, and arrow keys move between tabs.
    assert 'url.searchParams.set("section", next)' in WEB_SCRIPT
    assert "state.crmView.section = crmSectionFromUrl();" in WEB_SCRIPT
    assert WEB_SCRIPT.index("state.crmView.section = crmSectionFromUrl();") < WEB_SCRIPT.index(
        "select.value = state.crmView.organization; await loadCrm();")
    assert "{ ArrowRight: 1, ArrowLeft: -1 }[event.key]" in WEB_SCRIPT


def test_the_contact_dialog_has_an_input_for_every_field_it_saves():
    """Saving reads `[data-crm-contact-<field>]`.value for every dialog field
    without checking the node is there, so a contact column added without its
    input throws on save rather than skipping that field."""
    editable = WEB_SCRIPT.split("const CRM_CONTACT_COLUMNS = [", 1)[1]
    editable = editable.split("\n  ];", 1)[0]
    fields = re.findall(r'\{ field: "([a-zA-Z]+)"', editable)
    assert "value" in fields and "nextStep" in fields
    # The attribute is written in the field's own spelling; HTML lowercases it
    # and the selector matches case-insensitively.
    lowered = WEB_VIEW.lower()
    for field in fields:
        assert f"data-crm-contact-{field.lower()}" in lowered, field
    assert "body[field] = $(`[data-crm-contact-${field}]`).value" in WEB_SCRIPT


def test_the_deal_value_column_reads_as_money_and_the_next_step_as_text():
    # The worker sends the deal value as a number, so an unset one has to come
    # out as an empty cell rather than a "0" on every contact.
    assert 'if (field === "value") return contact.value ? String(contact.value) : "";' \
        in WEB_SCRIPT
    assert '{ field: "value", label: "Deal value", type: "number"' in WEB_SCRIPT
    assert '{ field: "nextStep", label: "Next step"' in WEB_SCRIPT


def test_reads_retry_a_wedged_isolate_blip_and_writes_are_never_replayed():
    # A poisoned Pyodide isolate fails every request it receives in a few
    # milliseconds (NoGilError 500, or a "hung" cancel) before CRM code runs;
    # the 30-second refresh must not show "CRM is unavailable" for that.
    request = WEB_SCRIPT.split("async function crmRequest(", 1)[1].split(
        "\n  }\n", 1)[0]
    assert "const CRM_READ_RETRY_DELAYS_MS = [400, 1500];" in WEB_SCRIPT
    assert 'method === "GET" ? CRM_READ_RETRY_DELAYS_MS : []' in request
    assert "crmTransientStatus(response.status)" in request
    transient = WEB_SCRIPT.split("function crmTransientStatus(", 1)[1].split(
        "\n  }\n", 1)[0]
    assert "status >= 500" in transient
    assert "429" not in transient


def test_the_team_tab_ranks_members_and_charts_their_days_in_local_time():
    assert 'const CRM_SECTIONS = ["contacts", "inbox", "outreach", "team"];' in WEB_SCRIPT
    assert "tz: String(-new Date().getTimezoneOffset())" in WEB_SCRIPT
    assert "state.crmView.activity = payload.activity" in WEB_SCRIPT
    assert "renderCrmTeam();" in WEB_SCRIPT.split("function renderCrm() {", 1)[1].split("\n  }", 1)[0]
    for option in ("7", "30"):
        assert f'data-crm-team-period-option="{option}"' in WEB_VIEW
    assert "data-crm-team-member" in WEB_VIEW
    assert ":root[data-dashboard-theme=\"dark\"] [data-view=\"crm\"] .crm-team-viz" in WEB_VIEW


def test_the_contacts_tab_opens_with_the_best_person_to_contact():
    import ast
    worker = (CRM_PACKAGE / "content" / "worker" / "crm.py").read_text(encoding="utf-8")

    def worker_constant(name):
        for node in ast.parse(worker).body:
            if isinstance(node, ast.Assign) and any(
                    getattr(target, "id", "") == name for target in node.targets):
                return ast.literal_eval(node.value)
        raise AssertionError(name)

    def worker_set(name):
        block = worker.split(f"\n{name} = frozenset({{", 1)[1].split("})", 1)[0]
        return set(re.findall(r'"([a-z-]+)"', block))

    def web_set(name):
        block = WEB_SCRIPT.split(f"const {name} = new Set([", 1)[1].split("]);", 1)[0]
        return set(re.findall(r'"([a-z-]+)"', block))

    assert web_set("CRM_NEXT_WON_STAGES") == worker_set("CLOSED_DEAL_STAGES")
    assert web_set("CRM_NEXT_ENDED_STAGES") == worker_set("ENDED_STAGES")
    assert web_set("CRM_NEXT_UNSENT_STATUSES") == set(worker_constant("UNSENT_STATUSES"))
    assert (f"const CRM_NEXT_REPLY_DAYS = "
            f"{worker_constant('EMAIL_SUGGESTION_REPLY_DAYS')};") in WEB_SCRIPT
    assert (f"const CRM_NEXT_FOLLOW_UP_DAYS = "
            f"{worker_constant('EMAIL_SUGGESTION_FOLLOW_UP_DAYS')};") in WEB_SCRIPT
    contacts = WEB_VIEW.split('data-crm-section="contacts"', 1)[1]
    assert contacts.index("data-crm-next ") < contacts.index("data-crm-tabs")
    assert "renderCrmNextContact();" in WEB_SCRIPT.split("function renderCrmContacts() {", 1)[1]
    render = WEB_SCRIPT.split("function renderCrmNextContact(", 1)[1].split("\n  }\n", 1)[0]
    assert "CRM_NEXT_MODES.map(crmNextCardHtml)" in render
    modes = WEB_SCRIPT.split("const CRM_NEXT_MODES = [", 1)[1].split("];", 1)[0]
    assert 'mode: "reach", title: "Reach out"' in modes and 'mode: "follow", title: "Follow up"' in modes
    card = WEB_SCRIPT.split("function crmNextCardHtml(", 1)[1].split("\n  }\n", 1)[0]
    assert "<b>Why them:</b>" in card
    assert "data-crm-next-contacted" in card and "data-crm-next-skip" in card
    channels = WEB_SCRIPT.split("function crmNextChannels(", 1)[1].split("\n  }\n", 1)[0]
    for label in ("Email with Zoho", "Message on Mastodon", "Message on X"):
        assert label in channels
    assert "/zm/comp.do?ct=" in WEB_SCRIPT.split("function crmZohoComposeUrl(", 1)[1].split("\n  }\n", 1)[0]
