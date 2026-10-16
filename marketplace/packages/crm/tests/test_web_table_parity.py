from pathlib import Path
import os
import shutil
import subprocess

import pytest


CRM_PACKAGE = Path(__file__).resolve().parents[1]
WEB_VIEW = (CRM_PACKAGE / "content" / "web" / "view.html").read_text(encoding="utf-8")
WEB_SCRIPT = (CRM_PACKAGE / "content" / "web" / "page.js").read_text(encoding="utf-8")
FEATURE = (CRM_PACKAGE / "content" / "desktop" / "CrmFeature.cpp").read_text(encoding="utf-8")
NODE = shutil.which("node")


def _function(name):
    return WEB_SCRIPT.split(f"function {name}(", 1)[1].split("\n  }\n", 1)[0]


@pytest.mark.skipif(NODE is None, reason="node is not installed")
def test_the_table_parity_helpers_hold_up_when_run():
    result = subprocess.run(
        [NODE, "--no-warnings", str(CRM_PACKAGE / "tests" / "table_parity_checks.js")],
        capture_output=True, text=True, timeout=600,
        env={"PATH": os.environ.get("PATH", "/usr/local/bin:/usr/bin:/bin"), "TZ": "UTC"})
    assert result.returncode == 0, result.stdout[-4000:] + result.stderr[-4000:]
    assert "table parity checks passed" in result.stdout


def test_rows_select_with_ctrl_shift_and_a_checkbox_and_the_menu_acts_on_the_selection():
    render = WEB_SCRIPT.split("function renderCrmContacts()", 1)[1].split("\n  // --- board layout", 1)[0]
    assert 'data-crm-select="${escapeHtml(contact.id)}"' in render
    assert "data-crm-select-all" in render
    assert "crmPaintSelection();" in render
    click = _function("handleCrmSelectionClick")
    assert "event.ctrlKey || event.metaKey || event.shiftKey" in click
    assert "range: event.shiftKey" in click
    assert 'list?.addEventListener("click", handleCrmSelectionClick, true);' in WEB_SCRIPT
    menu = _function("openCrmContactMenu")
    assert "const ids = crmMenuSelection(id);" in menu
    assert "`${contacts.length} contacts`" in menu
    for label in ("Mark contacted now", "Assign owner…", "Merge selected into", "Merge into…",
                  "Copy prompt", "Set stage in "):
        assert label in menu, label
    assert "disabled: contacts.every((one) => one.stage === stage)" in menu
    assert "contacts.every((one) => one.flow === contact.flow)" in menu
    assert "deleteCrmContacts(ids)" in menu
    selection = _function("crmMenuSelection")
    assert "crmSelection.ids = new Set([id]);" in selection
    assert "Ctrl+click and Shift+click pick several contacts" in FEATURE


def test_every_menu_action_is_one_bulk_request_that_shows_before_the_reply():
    patch = _function("crmPatchContacts")
    assert patch.count("crmRequest(") == 1
    assert 'crmOrganizationPath("/contacts/bulk"), "POST", { edits: changes }' in patch
    assert patch.index("crmWithContactEdit") < patch.index("crmRequest(")
    assert patch.count("await loadCrm();") == 2
    edit = _function("crmWithContactEdit")
    assert '"lastContactedAt" in body' in edit and '"owner" in body' in edit
    assert "lastContactedAt: stamp" in _function("crmMarkContacted")
    assert "owner" in _function("assignCrmContactsOwner")


def test_merging_picks_from_every_contact_and_folds_duplicates_one_at_a_time():
    assert "crmMergeCandidates" not in WEB_SCRIPT
    picker = _function("openCrmMergePicker")
    assert "crmMergeTargets(duplicate)" in picker
    assert 'index === 0 && target.score > 0 ? " checked" : ""' in picker
    assert "<dialog data-crm-merge-dialog" in WEB_VIEW and "data-crm-merge-search" in WEB_VIEW
    merge = WEB_SCRIPT.split("async function mergeCrmContacts(", 1)[1].split("\n  }\n", 1)[0]
    assert "for (const id of pending) {\n        await crmRequest(crmOrganizationPath(\"/contacts/merge\"), \"POST\", { from: id, into: keepId });" in merge
    assert "confirm(" in merge


def test_column_headings_sort_and_the_plus_heading_adds_a_column():
    render = WEB_SCRIPT.split("function renderCrmContacts()", 1)[1].split("\n  // --- board layout", 1)[0]
    assert "aria-sort" in render and "\\u25BC" in render and "\\u25B2" in render
    assert "data-crm-add-column" in render
    head = WEB_SCRIPT.split("function wireCrmTableParity()", 1)[1].split("\n  }\n", 1)[0]
    assert 'event.target.closest("[data-crm-add-column]")) { void addCrmCustomColumn(); return; }' in head
    assert "crmSortBy(cell.dataset.crmColumn)" in head
    assert "crmSort.descending = crmSort.field === field ? !crmSort.descending : false;" in _function("crmSortBy")
    assert "return crmSortContacts(matched);" in _function("crmFilteredContacts")
    add = _function("addCrmCustomColumn")
    assert "crmCustomColumnKey(label)" in add
    assert "await restoreCrmColumn(key)" in add and "crmShowColumn(key)" in add
    assert "crmOrganizationPath(`/views/${view.id}`), \"PATCH\", { columns: layout }" in add
    assert "{ field: key, label: crmCustomColumnLabel(key), custom: true, hidden: false }" in add
    assert "crmSessionColumns.labels.set(key" in add
    stored = _function("crmStoredActiveView")
    assert "pipeline:|no-pipeline" in stored and "view.virtual" in stored
    assert WEB_SCRIPT.count('{ label: "Add column…", run: () => void addCrmCustomColumn() }') == 2


def test_rows_and_cards_show_the_contacts_picture_read_lazily_and_cached_for_a_week():
    sources = _function("crmPictureSources")
    order = [sources.index(kind) for kind in ("`forkmesh:", "`mastodon:", "`bluesky:", "`github:", "`gravatar:")]
    assert order == sorted(order)
    read = _function("crmReadPicture")
    assert "avatars.githubusercontent.com/" in read
    assert "gravatar.com/avatar/${await crmSha256Hex(account)}?s=${CRM_PICTURE_SIZE}&d=404" in read
    assert "profile?.avatar_static || profile?.avatar" in read and "crmPictureUrl(" in read
    assert "app.bsky.actor.getProfile" in read
    assert "const CRM_PICTURE_CACHE_MS = 7 * 24 * 60 * 60 * 1000;" in WEB_SCRIPT
    assert "const CRM_PICTURES_IN_FLIGHT = 4;" in WEB_SCRIPT
    assert "localStorage.setItem(CRM_PICTURE_CACHE_KEY" in _function("crmSavePictureCache")
    assert "IntersectionObserver" in _function("crmWatchPictures")
    assert "crmPictures.inFlight < CRM_PICTURES_IN_FLIGHT" in _function("crmPumpPictures")
    render = WEB_SCRIPT.split("function renderCrmContacts()", 1)[1].split("\n  // --- board layout", 1)[0]
    assert 'class="crm-photo-button">${crmPicture(contact)}</button>' in render
    assert "crmPicture(contact, \"lg\")" in _function("crmNextCardHtml")
    assert ".crm-pic img { position: absolute; inset: 0;" in WEB_VIEW
    users = _function("crmForkMeshUsers")
    assert 'crmRequest("/api/accounts/users")' in users and "avatarPng" in users
    assert "state.session?.isAdmin === true" in _function("crmAdmin")


def test_the_people_grid_lists_who_has_a_picture():
    assert "data-crm-people-toggle" in WEB_VIEW and "<div data-crm-people hidden" in WEB_VIEW
    for option in ('value="recent">Most recent', 'value="contacted">Last contacted', 'value="name">Name'):
        assert option in WEB_VIEW, option
    people = _function("renderCrmPeople")
    assert "contacts with an account or email address show a picture." in people
    assert '"On no list"' in people and '"No pipeline"' in people
    assert 'data-crm-contact-detail="${escapeHtml(contact.id)}"' in people
    rows = _function("crmPeopleRows")
    assert "crmContactMatchesQuery(contact, query)" in rows


def test_mastodon_and_linked_message_columns_are_optional_and_read_like_the_desktop():
    assert 'field: "mastodon-latest", label: "Mastodon last update"' in WEB_SCRIPT
    assert 'field: "linked-emails", label: "Linked emails"' in WEB_SCRIPT
    assert 'field: "linked-toots", label: "Linked toots"' in WEB_SCRIPT
    for column_id in ("mastodon-latest", "linked-emails", "linked-toots"):
        assert f'[] = "{column_id}";' in FEATURE, column_id
    assert '"mastodon-latest"' not in WEB_SCRIPT.split("const CRM_DEFAULT_VIEW_FIELDS = [", 1)[1].split("];", 1)[0]
    read = WEB_SCRIPT.split("async function readCrmMastodonLatest(", 1)[1].split("\n  }\n", 1)[0]
    assert "crmMastodonUpdates({ key, reuseId: true }, 1)" in read
    assert "error.status === 429" in read and "crmScheduleNextRender();" in read
    assert "const CRM_MASTODON_PER_HOST = 2;" in WEB_SCRIPT
    assert "const CRM_REACH_MASTODON_READS = 40;" in WEB_SCRIPT
    assert "const CRM_MASTODON_FRESH_MS = 15 * 60 * 1000;" in WEB_SCRIPT
    assert "const CRM_MASTODON_IN_FLIGHT = 4;" in WEB_SCRIPT
    assert "data-crm-mastodon-refresh" in _function("crmMastodonLatestCell")
    assert "`  (+${messages.length - 1} more)`" in _function("crmLinkedCellText")
    linked = _function("openCrmLinked")
    assert "messages.length === 1" in linked and "openCrmMenu(event" in linked


def test_addresses_open_in_the_organizations_mail_provider():
    assert "data-crm-settings-mail-provider" in WEB_VIEW and "Email addresses open in" in WEB_VIEW
    assert 'mailProvider: $(`[data-crm-settings-mail-provider]`).value' in WEB_SCRIPT
    compose = _function("crmMailComposeUrl")
    assert "https://mail.google.com/mail/?view=cm&fs=1&to=" in compose
    assert "crmZohoComposeUrl(to)" in compose
    assert "crmMailComposeUrl(email)" in _function("crmNextChannels")
    assert "crmMailTile(contact)" in _function("crmContactControl")


def test_copy_prompt_last_contacted_now_and_search_match_the_desktop():
    render = WEB_SCRIPT.split("function renderCrmContacts()", 1)[1].split("\n  // --- board layout", 1)[0]
    assert render.count("data-crm-contact-prompt=") == 2
    prompt = _function("crmOutreachPrompt")
    assert '"Tell me about ForkMesh (https://forkmesh.com).",' in prompt
    assert 'QStringLiteral("Tell me about ForkMesh (https://forkmesh.com).")' in FEATURE
    assert "data-crm-contacted-now" in _function("crmContactControl")
    for field in ("phone", "source", "notes", "nextStep", "website", "location", "owner"):
        assert f'"{field}"' in WEB_SCRIPT.split("const CRM_SEARCH_FIELDS = [", 1)[1].split("];", 1)[0], field


def test_imports_and_the_lists_overview_live_in_the_more_menu():
    menu = WEB_VIEW.split("<details data-crm-actions", 1)[1].split("</details>", 1)[0]
    for hook in ("data-crm-github-import", "data-crm-users-import", "data-crm-lists-open", "data-crm-mastodon-import"):
        assert hook in menu, hook
    commits = _function("crmGithubCommits")
    assert "https://api.github.com/repos/${repository}/commits?per_page=100" in commits
    assert 'response.headers.get("x-ratelimit-remaining") === "0"' in commits
    assert "pages < CRM_GITHUB_COMMIT_PAGES" in commits
    github = WEB_SCRIPT.split("async function submitCrmGithub(", 1)[1].split("\n  }\n", 1)[0]
    assert "`github-contributors-${repository.replace(\"/\", \"-\")}.csv`" in github
    assert "`Contributors of ${repository}`.slice(0, 60)" in github
    assert 'QStringLiteral("Contributors of %1")' in FEATURE
    users = WEB_SCRIPT.split("async function importCrmForkMeshUsers(", 1)[1].split("\n  }\n", 1)[0]
    assert 'mapping: ["name", "email", "custom", "custom"], sheetName: "ForkMesh users"' in users
    assert "target: crmForkMeshUsersSheetId()" in users
    assert 'const CRM_FORKMESH_USERS_FILE = "forkmesh-users.csv";' in WEB_SCRIPT
    assert "view.fileName === CRM_FORKMESH_USERS_FILE && view.canManage" in _function("crmForkMeshUsersSheetId")
    followers = WEB_SCRIPT.split("async function submitCrmMastodon(", 1)[1].split("\n  }\n", 1)[0]
    assert "`mastodon-followers-${account.user}@${account.host}.csv`" in followers
    assert "`Followers of ${handle}`.slice(0, 60)" in followers
    assert "one import takes at most ${CRM_MASTODON_MAX}" in followers
    lists = _function("renderCrmListsOverview")
    for label in ("(pipeline)", "Every contact in ", "Only its owner", "Whole organization · ", " of "):
        assert label in lists, label
    assert "<dialog data-crm-lists-dialog" in WEB_VIEW
    for heading in ("List", "File", "Contacts", "Columns", "Shared with", "Owner", "Imported"):
        assert f'font-semibold">{heading}</th>' in WEB_VIEW.split("<dialog data-crm-lists-dialog", 1)[1], heading
