"""The contact detail card: what opens it, what it shows, and what it reads.

A click on a row's picture or name opens one card holding every field of the
record, the activity tied to it, and the newest posts on the accounts it
names. The posts and the picture are read from those services by the browser
the card is open in, so the contracts here also pin down that those reads
happen on a card opening and nowhere else.
"""

from pathlib import Path
import re
import shutil
import subprocess

import pytest


CRM_PACKAGE = Path(__file__).resolve().parents[1]
WEB_VIEW = (CRM_PACKAGE / "content" / "web" / "view.html").read_text(
    encoding="utf-8")
WEB_SCRIPT = (CRM_PACKAGE / "content" / "web" / "page.js").read_text(
    encoding="utf-8")
NODE = shutil.which("node")


def _function(name):
    return WEB_SCRIPT.split(f"function {name}(", 1)[1].split("\n  }\n", 1)[0]


@pytest.mark.skipif(NODE is None, reason="node is not installed")
def test_the_cards_handle_parsers_and_update_lines_hold_up_when_run():
    result = subprocess.run(
        [NODE, "--no-warnings", str(CRM_PACKAGE / "tests" / "contact_detail_checks.js")],
        capture_output=True, text=True, timeout=600,
        env={"PATH": "/usr/local/bin:/usr/bin:/bin"})
    assert result.returncode == 0, result.stdout[-4000:] + result.stderr[-4000:]
    assert "contact detail checks passed" in result.stdout


def test_a_rows_picture_and_name_open_the_card():
    render = WEB_SCRIPT.split("function renderCrmContacts()", 1)[1].split(
        "\n  // --- board layout", 1)[0]
    # Both are buttons, so the keyboard reaches them as well as the mouse.
    assert 'data-crm-contact-detail="${escapeHtml(contact.id)}" title="Open the detail card" aria-label="Open ${escapeHtml(crmContactLabel(contact))}" class="crm-photo-button"' in render
    # A truncated name still reads in full on hover, and says what a click
    # on it does.
    assert 'data-crm-contact-detail="${escapeHtml(contact.id)}" title="${escapeHtml(crmContactLabel(contact))} · open the detail card" class="crm-name-open truncate"' in render
    # The table rows and the phone cards use the same two.
    assert render.count("photo(contact)") == 2
    assert "name(contact, column)" in render and "name(contact, crmBaseColumn(\"name\"))" in render
    # A shared sheet is another organization's read-only copy of a few
    # columns, so its rows have no record of ours to open.
    assert render.count("shared\n      ? crmAvatar(contact)") == 1
    assert "shared\n      ? crmContactControl(contact, column)" in render
    # A board card, and Enter on one, open the same card.
    board = WEB_SCRIPT.split("function renderCrmBoard(contacts)", 1)[1].split(
        "\n  }\n\n", 1)[0]
    assert '<article data-crm-contact-detail="${escapeHtml(contact.id)}"' in board
    assert "openCrmContactDetail(item.dataset.crmBoardCard)" in WEB_SCRIPT
    # The click handler the three containers share opens the card first.
    assert 'const detail = event.target.closest("[data-crm-contact-detail]"); if (detail) { openCrmContactDetail(detail.dataset.crmContactDetail); return; }' in WEB_SCRIPT
    # Nothing carries the old "open the edit form" hook any more.
    assert "data-crm-contact-open" not in WEB_SCRIPT
    for hook in ("[data-crm-contact-list]", "[data-crm-contact-cards]"):
        assert f'$(`{hook}`)?.addEventListener("click", handleContactAction)' in WEB_SCRIPT
    assert 'board?.addEventListener("click", handleContactAction)' in WEB_SCRIPT


def test_the_card_shows_the_picture_at_a_size_worth_looking_at():
    assert '<dialog data-crm-contact-detail-dialog' in WEB_VIEW
    assert "data-crm-detail-head" in WEB_VIEW and "data-crm-detail-body" in WEB_VIEW
    # The card's own circle is more than three times the row's 1.75rem.
    assert '.crm-avatar-xl { width: 6rem; height: 6rem;' in WEB_VIEW
    assert '.crm-photo { position: relative;' in WEB_VIEW
    assert '.crm-photo img { position: absolute; inset: 0; width: 100%; height: 100%; border-radius: 999px; object-fit: cover; }' in WEB_VIEW
    detail = _function("renderCrmContactDetail")
    # The initials sit under the photo, so the card reads before a picture
    # lands and still reads when none does.
    assert 'class="crm-photo">${crmAvatar(contact, "xl")}<img data-crm-detail-photo alt="" hidden />' in detail
    photo = _function("crmApplyDetailPhoto")
    assert "photo.onerror = () => { photo.hidden = true; };" in photo
    assert 'photo.dataset.crmPhoto === picture' in photo
    # The picture is whichever account answered with one, in the order the
    # rows try them.
    assert "crmLiveAccounts(contact)" in photo and ".find(Boolean)" in photo


def test_the_card_holds_every_field_and_saves_through_the_tables_own_path():
    detail = _function("renderCrmContactDetail")
    # Every column the table can show, plus the custom keys the records
    # carry, as the same inline control the table saves with.
    assert "[...CRM_BASE_COLUMNS, ...crmCustomFieldKeys()" in detail
    assert 'crmContactControl(contact, column, "0")' in detail
    assert 'detailDialog?.addEventListener("change", handleInlineContactField);' in WEB_SCRIPT
    # Who added the record and who last touched it, which no column shows.
    for stamp in ("contact.createdAt", "contact.createdBy",
                  "contact.updatedAt", "contact.updatedBy"):
        assert stamp in detail, stamp
    # Email, Deck and the full form are dialogs of their own, so the card
    # closes before handing over instead of stacking on top of them.
    assert 'if (!event.target.closest("[data-crm-contact-edit], [data-crm-contact-compose], [data-crm-contact-deck]")) return;\n      detailDialog.close();\n      handleContactAction(event);' in WEB_SCRIPT


def test_the_card_lists_the_activity_tied_to_the_record_newest_first():
    activity = _function("crmContactActivity")
    assert "crmLinkedEmails(contact.id)" in activity
    assert "crmDeckFor(contact.id)" in activity
    for label in ("Last contacted", "Record edited", "Record added"):
        assert f'label: "{label}"' in activity, label
    assert "(second.at || 0) - (first.at || 0)" in activity
    line = _function("crmDetailLine")
    assert 'target="_blank" rel="noopener noreferrer"' in line
    assert "escapeHtml(item.text)" in line and "escapeHtml(item.url)" in line


def test_the_card_reads_the_connected_accounts_and_stores_nothing():
    assert 'const CRM_LIVE_SERVICES = ["mastodon", "bluesky", "github"];' in WEB_SCRIPT
    read = _function("crmReadPublicJson")
    # Public reads: no cookies, no referrer, and a timeout so a dead instance
    # leaves the rest of the card alone.
    assert 'credentials: "omit"' in read and 'referrerPolicy: "no-referrer"' in read
    assert "AbortSignal.timeout?.(CRM_UPDATE_TIMEOUT_MS)" in read
    assert "const CRM_UPDATE_LIMIT = 5;" in WEB_SCRIPT
    # The three reads, each against the service's own public API.
    assert "/api/v1/accounts/lookup?acct=" in WEB_SCRIPT
    assert "exclude_replies=true&exclude_reblogs=true" in WEB_SCRIPT
    assert "public.api.bsky.app/xrpc/app.bsky.actor.getProfile" in WEB_SCRIPT
    assert "public.api.bsky.app/xrpc/app.bsky.feed.getAuthorFeed" in WEB_SCRIPT
    assert "api.github.com/users/${encodeURIComponent(account.key)}/events/public" in WEB_SCRIPT
    # Opening a card is the only thing that reads them, and "Read again" is
    # the only way to ask a second time.
    assert WEB_SCRIPT.count("loadCrmContactUpdates(") == 3
    opener = _function("openCrmContactDetail")
    assert "void loadCrmContactUpdates(contact)" in opener
    assert "crmActiveShared()" in opener
    assert "data-crm-detail-reload" in WEB_VIEW or "data-crm-detail-reload" in WEB_SCRIPT
    load = _function("loadCrmContactUpdates")
    # A card closed or reopened meanwhile drops what comes back.
    assert "state.crmView.detailContactId !== contact.id" in load
    assert "nothing is stored" in WEB_SCRIPT
    # Nothing about a read is written to the record or the browser's storage.
    updates = "".join(_function(name) for name in (
        "crmMastodonUpdates", "crmBlueskyUpdates", "crmGithubUpdates",
        "loadCrmContactUpdates", "renderCrmContactUpdates"))
    assert "localStorage" not in updates
    assert "crmRequest" not in updates


def test_every_post_and_profile_link_on_the_card_is_https():
    https = _function("crmHttpsUrl")
    assert 'url.protocol === "https:"' in https
    updates = _function("renderCrmContactUpdates")
    assert 'target="_blank" rel="noopener noreferrer"' in updates
    # Post addresses come from the service, so each one goes through the
    # https check before it is shown.
    for reader in ("crmMastodonUpdates", "crmBlueskyUpdates"):
        assert "crmHttpsUrl(" in _function(reader) or "bsky.app/profile/" in _function(reader), reader
    assert re.search(r"picture: crmPictureUrl\(", WEB_SCRIPT)


def test_a_contact_has_a_link_that_opens_its_card():
    url = _function("crmContactUrl")
    assert '"org"' not in url
    assert 'url.searchParams.set("contact", contactId);' in url
    resolve = _function("crmResolveLink")
    assert "/api/crm/-/open/${id}" in resolve
    assert "new URL(location.pathname, location.origin)" in url
    opened = _function("openCrmContactDetail")
    assert "crmSetContactParam(contact.id);" in opened
    assert 'crmSetContactParam(""); });' in WEB_SCRIPT
    render = _function("renderCrmContactDetail")
    assert 'data-crm-contact-link="${id}"' in render and ">Copy link</button>" in render
    assert "copyCrmContactLink(link.dataset.crmContactLink, link)" in WEB_SCRIPT
    menu = _function("openCrmContactMenu")
    assert '{ label: "Copy link", run: () => void copyCrmContactLink(id) },' in menu
    copy = _function("copyCrmContactLink")
    assert "navigator.clipboard.writeText(crmContactUrl(contact.id))" in copy
    from_url = _function("openCrmContactFromUrl")
    assert 'searchParams.get("contact")' in from_url
    assert "!state.crmView.installed" in from_url
    assert "openCrmContactDetail(id)" in from_url
    assert 'crmSetContactParam("");' in from_url
    init = WEB_SCRIPT.split("async function initCrmPage()", 1)[1]
    assert "openCrmContactFromUrl();\n  }\n\n  return initCrmPage();" in init


def test_a_heading_right_click_deletes_custom_and_builtin_columns():
    menu = _function("openCrmColumnMenu")
    assert "crmDeleteColumnItems(columns.find((column) => column.field === field))" in menu
    assert "crmRestoreColumnItems()" in menu
    items = _function("crmDeleteColumnItems")
    assert "CRM_KEPT_COLUMNS.has(column.field)" in items
    assert "disabled: !manages" in items
    assert 'const CRM_KEPT_COLUMNS = new Set(["name", "email", "flow", "stage", "lastContactedAt"]);' in WEB_SCRIPT
    delete = _function("deleteCrmColumn")
    assert 'crmOrganizationPath("/contacts/delete-column"), "POST", { field }' in delete
    assert "contact[field]" in delete
    restore = _function("restoreCrmColumn")
    assert 'crmOrganizationPath("/settings"), "PUT", { removedColumns: removed }' in restore
    assert "crmShowColumn(field);" in restore


def test_a_list_has_a_link_that_opens_it():
    url = _function("crmListUrl")
    assert 'url.searchParams.set("list", viewId);' in url
    assert "crmSetListParam(state.crmView.activeView);" in _function("selectCrmSheet")
    assert "crmSetListParam(state.crmView.activeShared);" in _function("selectCrmSharedSheet")
    assert '{ label: "Copy link", run: () => void copyCrmListLink(view) },' in _function("openCrmSheetMenu")
    assert '{ label: "Copy link", run: () => void copyCrmListLink(view) },' in _function("openCrmSharedMenu")
    assert "data-crm-sheet-link" in WEB_VIEW
    assert "copyCrmListLink(\n      state.crmView.views.find" in WEB_SCRIPT
    assert "navigator.clipboard.writeText(crmListUrl(view.id))" in _function("copyCrmListLink")
    from_url = _function("openCrmListFromUrl")
    assert 'searchParams.get("list")' in from_url
    assert "crmTabViews().some((view) => view.id === id)) { selectCrmSheet(id); return; }" in from_url
    assert "state.crmView.sharedViews.some((view) => view.id === id)) { void selectCrmSharedSheet(id); return; }" in from_url
    assert 'crmSetListParam("");' in from_url
    assert 'url.searchParams.delete("list"); history.replaceState' in WEB_SCRIPT
    assert "openCrmListFromUrl();\n    openCrmContactFromUrl();" in WEB_SCRIPT
