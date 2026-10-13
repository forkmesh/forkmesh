"""The ForkMesh browser extension: its own files, and its contract with the
CRM package it links mail for (read from the sibling package)."""

import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

import pytest


PACKAGE = Path(__file__).resolve().parents[1]
CRM_PACKAGE = PACKAGE.parent / "crm"
REPO = PACKAGE.parents[2]
EXTENSION = PACKAGE / "content" / "extension"
MANIFEST = json.loads((EXTENSION / "manifest.json").read_text(encoding="utf-8"))
BACKGROUND = (EXTENSION / "background.js").read_text(encoding="utf-8")
CONTENT = (EXTENSION / "content.js").read_text(encoding="utf-8")
PANEL = (EXTENSION / "panel.js").read_text(encoding="utf-8")
POPUP = (EXTENSION / "popup.js").read_text(encoding="utf-8")
POPUP_HTML = (EXTENSION / "popup.html").read_text(encoding="utf-8")
MASTODON = (EXTENSION / "mastodon.js").read_text(encoding="utf-8")
DASHBOARD_BRIDGE = (EXTENSION / "dashboard.js").read_text(encoding="utf-8")
WORKER = (CRM_PACKAGE / "content" / "worker" / "crm.py").read_text(encoding="utf-8")
WEB_VIEW = (CRM_PACKAGE / "content" / "web" / "view.html").read_text(encoding="utf-8")
WEB_SCRIPT = (CRM_PACKAGE / "content" / "web" / "page.js").read_text(encoding="utf-8")
PAGE_SCRIPT = (PACKAGE / "content" / "web" / "page.js").read_text(encoding="utf-8")
PAGE_VIEW = (PACKAGE / "content" / "web" / "view.html").read_text(encoding="utf-8")
CRM_DESKTOP = (CRM_PACKAGE / "content" / "desktop" / "CrmFeature.cpp").read_text(encoding="utf-8")
ENTRY = (REPO / "app" / "src" / "entry.py").read_text(encoding="utf-8")
PACKAGE_MANIFEST = json.loads((PACKAGE / "manifest.json").read_text(encoding="utf-8"))


def test_extension_is_manifest_v3_with_the_narrowest_permissions():
    assert MANIFEST["manifest_version"] == 3
    assert MANIFEST["version"] == PACKAGE_MANIFEST["version"]
    # "scripting" registers one reader per Mastodon instance the user adds;
    # a fediverse host cannot be named in the manifest up front.
    # "alarms" is the vault's idle lock.
    assert sorted(MANIFEST["permissions"]) == ["activeTab", "alarms", "scripting", "sidePanel", "storage", "tabs"]
    # Only the ForkMesh app is reachable by default; a self-hosted server is
    # granted at runtime, never every site up front.
    assert MANIFEST["host_permissions"] == [
        "https://app.forkmesh.com/*", "https://forkmesh.com/*",
        "https://www.reddit.com/*", "https://old.reddit.com/*"], (
        "reddit.com is granted up front, not optionally: reddit.js is already a "
        "content script there, so the install warning is the same one, and the "
        "grant is what lets a walk inject its pacing script")
    assert MANIFEST["optional_host_permissions"] == ["https://*/*"]
    assert MANIFEST["background"] == {"service_worker": "background.js"}
    script = MANIFEST["content_scripts"][0]
    assert "https://mail.google.com/*" in script["matches"]
    assert "https://mail.zoho.com/*" in script["matches"]
    assert "https://mail.zoho.eu/*" in script["matches"]
    # The shared panel loads first; content.js mounts it on the mail page.
    assert script["js"] == ["panel.js", "content.js"]
    # No remote code: every script is a packaged file.
    # The CRM dashboard page gets a bridge: the installed version and a way to
    # open chrome://extensions. Hosts already granted above, no new warning.
    bridge = MANIFEST["content_scripts"][1]
    assert bridge["js"] == ["dashboard.js"]
    assert bridge["matches"] == [
        "https://app.forkmesh.com/dashboard/crm*", "https://app.forkmesh.com/dashboard/vault*",
        "https://app.forkmesh.com/dashboard/extension*", "https://app.forkmesh.com/dashboard/marketing*",
        "https://forkmesh.com/dashboard/crm*", "https://forkmesh.com/dashboard/vault*",
        "https://forkmesh.com/dashboard/extension*", "https://forkmesh.com/dashboard/marketing*"]
    # No content script runs on the open web: the vault reaches a page only
    # through activeTab, after a click in the popup.
    for script in MANIFEST["content_scripts"]:
        assert not any(match in ("<all_urls>", "https://*/*", "*://*/*") for match in script["matches"])
    assert PACKAGE_MANIFEST["entrypoints"]["browser-extension"] == "content/extension/manifest.json"
    assert len(MANIFEST["description"]) <= 132
    for name in ("background.js", "content.js", "mastodon.js", "panel.js",
                 "dashboard.js", "popup.js", "popup.html", "popup-vault.js",
                 "vault.js", "vault-crypto.js", "vault-client.js", "x.js", "reddit.js",
                 "marketing-capture.js", "capture.html", "capture.js", "x-developer.js"):
        assert (EXTENSION / name).is_file()
    for size in ("16", "32", "48", "128"):
        assert (EXTENSION / MANIFEST["icons"][size]).is_file()
    assert "content_security_policy" not in MANIFEST


def test_extension_talks_to_forkmesh_with_a_bearer_and_never_cookies():
    assert 'credentials: "omit"' in BACKGROUND
    assert "headers.authorization = `Bearer ${current.sessionToken}`" in BACKGROUND
    assert '"/api/accounts/login"' in BACKGROUND
    assert '"/api/accounts/logout"' in BACKGROUND
    assert '"/contacts/lookup"' in BACKGROUND
    assert '"/emails/link"' in BACKGROUND
    # A revoked session is forgotten rather than retried forever.
    assert 'chrome.storage.local.remove(["sessionToken", "account"])' in BACKGROUND
    # Only the service worker holds the token: the page script never reads
    # storage or fetches anything itself.
    for script in (CONTENT, MASTODON, PANEL):
        assert "chrome.storage" not in script
        assert "fetch(" not in script
        assert "sessionToken" not in script
    # The linked record carries ids, addresses, subject, and date: no body.
    body = BACKGROUND.split("function linkBody(message)", 1)[1].split("\n}\n", 1)[0]
    for key in ("provider", "messageId", "threadId", "direction", "contactEmail",
                "contactName", "mailbox", "handle", "subject", "sentAt",
                "providerUrl"):
        assert key + ":" in body
    assert "text:" not in body and "html:" not in body
    # "Add to pipeline" rides the same link call: a flow, a stage, and the
    # contact details read off the email, never the body itself.
    for key in ("flow", "stage", "contact"):
        assert key + ":" in body
    assert "flows: payload.flows || []" in BACKGROUND
    assert "password" not in BACKGROUND.split("async function signIn", 1)[0]


def test_content_script_reads_gmail_and_zoho_and_exposes_the_message_to_the_popup():
    assert "data-legacy-message-id" in CONTENT
    assert "https://mail.google.com/mail/u/${accountIndex}/#all/${messageId}" in CONTENT
    assert re.search(r"hash\.match\(/\\/p\\/\(\\d\{6,\}\)/\)", CONTENT)
    assert 'request?.type !== "fm-read-message"' in CONTENT
    assert "attachShadow" in PANEL
    assert 'window.addEventListener("hashchange"' in CONTENT
    # Zoho's Sent folder is mail you wrote; the recipient is the contact.
    assert 'const ZOHO_SENT_FOLDERS = new Set(["sent", "outbox", "drafts"]);' in CONTENT
    assert "function greetingName(lines)" in CONTENT
    assert "function signatureDetails(lines)" in CONTENT
    assert 'data-act="pipeline"' in PANEL
    assert 'data-pipeline="flow"' in PANEL and 'data-pipeline="stage"' in PANEL
    assert "...(choice ? { flow: choice.flow.id, stage: choice.stage } : {})," in PANEL
    assert '{ type: "fm-read-message" }' in POPUP
    assert "chrome.permissions.request" in POPUP
    assert 'type="password"' in POPUP_HTML
    assert 'autocomplete="one-time-code"' in POPUP_HTML


def test_dashboard_offers_mailboxes_the_current_extension_and_its_reload_page():
    assert 'href="https://mail.google.com/mail/"' in WEB_VIEW
    assert "data-crm-open-zoho" in WEB_VIEW
    assert '"com.au": "mail.zoho.com.au"' in WEB_SCRIPT
    # This package's own page names the version it ships, and the zip the
    # deploy stage builds from content/extension carries the same version.
    assert f'const EXTENSION_VERSION = "{MANIFEST["version"]}";' in PAGE_SCRIPT
    assert ("`/dashboard-plugins/browser-extension/browser-extension-${EXTENSION_VERSION}.zip`"
            in PAGE_SCRIPT)
    assert 'source: "forkmesh-extension-page", type: "open-extensions"' in PAGE_SCRIPT
    # The CRM page only needs a build new enough to speak its API.
    minimum = WEB_SCRIPT.split('const CRM_EXTENSION_MINIMUM = "', 1)[1].split('"', 1)[0]
    assert tuple(map(int, minimum.split("."))) <= tuple(map(int, MANIFEST["version"].split(".")))
    # The bridge still stamps the name the CRM page reads, plus its own.
    assert "dataset.forkmeshCrmExtension = version" in DASHBOARD_BRIDGE
    assert "dataset.forkmeshExtension = version" in DASHBOARD_BRIDGE
    assert '["forkmesh-crm-page", "forkmesh-extension-page", "forkmesh-marketing-page"]' in DASHBOARD_BRIDGE
    assert "data-crm-extension-install" in WEB_VIEW
    # Websites cannot open chrome://extensions; the extension's bridge does.
    assert "data-crm-extensions-open" in WEB_VIEW
    assert 'source: "forkmesh-crm-page", type: "open-extensions"' in WEB_SCRIPT
    assert 'event.source !== window || event.origin !== location.origin' in DASHBOARD_BRIDGE
    assert '{ type: "fm-open-extensions" }' in DASHBOARD_BRIDGE
    assert '"fm-open-extensions": openExtensions' in BACKGROUND
    assert 'chrome.tabs.create({ url: "chrome://extensions" })' in BACKGROUND
    for forbidden in ("chrome.storage", "fetch(", "sessionToken", "vaultKeyset"):
        assert forbidden not in DASHBOARD_BRIDGE


def test_mail_page_shows_the_crm_record_above_the_email_and_the_panel_lives_on_the_icon():
    # A compact header above the open email carries the record: pipeline,
    # stage, owner, last email, and whatever else the contact has.
    # The header is panel.js's, so the mail and Mastodon readers show the
    # same record; each one only says which node it goes above.
    for label in ('"Pipeline"', '"Stage"', '"Owner"', '"Linked"'):
        assert f"chip({label}" in PANEL
    assert 'chip(thread ? "Last contact" : "Last email"' in PANEL
    assert "anchor.parentNode.insertBefore(host, anchor)" in PANEL
    assert "lastEmailAt(contact, ui.emails, ui.message)" in PANEL
    for script in (CONTENT, MASTODON):
        assert "crm.header({ read:" in script
        assert "place(anchor)" in script
        assert "new MutationObserver(" in script
        # Following the page is free; the CRM is not asked about the message
        # until Look up is pressed. Only a change made in the popup reads the
        # record again ("asked").
        assert "void panel.present(message)" in script
        assert "if (asked) { asked = false; void panel.load(message); }" in script
    assert 'data-strip="lookup"' in PANEL and 'data-act="lookup"' in PANEL
    assert 'else if (act === "lookup") void panel.lookup();' in PANEL
    assert 'else if (act === "lookup") void lookupContact();' in PANEL
    # Showing a message sets the "not looked up" state; a lookup clears it.
    present = PANEL.split("async function present(message)", 1)[1].split("\n    }\n", 1)[0]
    assert "ui.stale = Boolean(message?.messageId)" in present
    assert '"fm-lookup"' not in present
    assert "ui.emails = []; ui.stale = false;" in PANEL
    # No floating button: the header's FM button opens the panel in the page,
    # and the toolbar icon's popup mounts the same panel.
    assert "position: fixed" not in CONTENT and "position: fixed" not in PANEL
    assert 'data-strip="toggle"' in PANEL
    assert '<script src="panel.js"></script>' in POPUP_HTML
    assert "ForkMeshCrm.create({" in POPUP and "embedded: true" in POPUP
    # A change made in the popup redraws the page's header.
    assert '{ type: "fm-refresh-page" }' in POPUP
    assert 'request?.type === "fm-refresh-page"' in CONTENT


def test_a_mastodon_conversation_links_like_an_email_on_an_instance_granted_once():
    # No fediverse host is in the manifest: each instance is added in the
    # popup, granted by Chrome in that click, and registered as its own reader.
    assert "https://mastodon.social/*" not in json.dumps(MANIFEST)
    assert "chrome.permissions.request({ origins: [`https://${host}/*`] })" in POPUP
    assert '{ type: "fm-mastodon-add", host }' in POPUP
    assert "data-mastodon-host" in POPUP_HTML and "data-mastodon-list" in POPUP_HTML
    assert "chrome.scripting.registerContentScripts" in BACKGROUND
    assert 'js: ["panel.js", "mastodon.js"]' in BACKGROUND
    # A permission taken back in Chrome's own settings stops its reader.
    assert "chrome.permissions.onRemoved.addListener" in BACKGROUND
    assert "chrome.permissions.contains" in BACKGROUND
    for key in ("fm-mastodon-hosts", "fm-mastodon-add", "fm-mastodon-remove"):
        assert f'"{key}"' in BACKGROUND and f'"{key}"' in POPUP

    # A toot on an instance not added yet gets a record button: the click is
    # the grant, and the open tab gets the reader without a reload.
    assert "data-toot-record" in POPUP_HTML and "data-toot-host" in POPUP_HTML
    assert "MASTODON_POST.test(url.pathname)" in POPUP
    assert '{ type: "fm-mastodon-add", host, tabId: tab.id }' in POPUP
    assert "chrome.scripting.executeScript({ target, files: MASTODON_FILES.js })" in BACKGROUND

    # The reader names the conversation and its people the way Mastodon does.
    assert ".detailed-status" in MASTODON
    # The header sits where an email's does: under the author, over the text.
    assert 'detailed.querySelector(".content-warning, .status__content")' in MASTODON
    assert "anchor: body?.parentNode ? body : detailed" in MASTODON
    assert "initial-state" in MASTODON
    assert 'provider: "mastodon"' in MASTODON
    assert "time[datetime]" in MASTODON
    # A remote mention reads "@grace" but Mastodon links it relatively
    # (/@grace@aus.social): the resolved link names the instance, never the
    # instance being browsed.
    assert "String(mention?.href" in MASTODON
    assert 'fullHandle(text(mention))' not in MASTODON
    # A handle, not an address: the panel looks the contact up by it and the
    # CRM matches the record carrying it.
    assert "handle: ui.message.handle" in PANEL
    assert 'data-field="handle"' in PANEL
    assert "async def _matching_box(" in WORKER
    assert 'details["mastodon"] = "@" + handle' in WORKER
    assert '"mastodon conversation" if provider == "mastodon"' in WORKER
    # The dashboard shows a linked thread beside the linked mail.
    assert 'mastodon: "Mastodon"' in WEB_SCRIPT
    assert '["gmail", "zoho", "mastodon"].includes(email.provider)' in WEB_SCRIPT


def test_the_icon_files_any_page_under_a_contact_and_the_date_is_editable():
    # Any site at all: the popup takes the tab's address and title onto a
    # contact's notes, appended to what the record already holds.
    assert "data-note-section" in POPUP_HTML and "data-note-contact" in POPUP_HTML
    assert '{ type: "fm-contacts" }' in POPUP
    assert 'type: "fm-note-link", contactId, url: notePage.url' in POPUP
    assert '"fm-note-link": noteLink' in BACKGROUND
    assert "async function noteLink(" in BACKGROUND
    assert "const notes = existing ? `${existing}\\n${line}` : line;" in BACKGROUND
    # Only the address bar: the page itself is never read for this.
    assert "chrome.scripting.executeScript" not in POPUP
    assert "notePage = /^https?:\\/\\//i.test(tab?.url || \"\")" in POPUP

    # "Last contacted" is a date on the record, editable from the panel and
    # from the CRM page's own column.
    assert 'data-contact="lastContactedAt"' in PANEL
    assert 'data-act="contacted-now"' in PANEL
    assert 'act === "contacted-now") void setLastContacted(Date.now())' in PANEL
    assert 'type: "fm-contact-update", contactId: contact.id' in PANEL
    assert '"fm-contact-update": contactPatch' in BACKGROUND
    assert 'field: "lastContactedAt", label: "Last contacted"' in WEB_SCRIPT
    assert "readonly: true" not in WEB_SCRIPT.split("CRM_HISTORY_COLUMN", 1)[1][:200]
    assert 'data-crm-contact-inline-field="lastContactedAt"' in WEB_SCRIPT
    assert "function crmDateInputStamp(value)" in WEB_SCRIPT
    # The CRM already takes the date on a contact patch.
    assert 'if "lastContactedAt" in data or not existing:' in WORKER


def test_an_email_links_to_a_contact_picked_from_the_crm():
    assert 'data-strip="choose"' in PANEL
    assert 'else if (act === "choose") { panel.ui.open = true; void panel.choose(); }' in PANEL
    assert 'const result = await request({ type: "fm-contacts" });' in PANEL
    assert '...(picked ? { contactId: picked.id } : {}),' in PANEL
    assert 'contactId: String(message.contactId || ""),' in BACKGROUND
    assert 'flow: String(contact.flow || ""), stage: String(contact.stage || ""),' in BACKGROUND
    assert "row = await _contact_row(runtime, org_bi, contact_id)" in WORKER
    assert '"other_emails"' in WORKER


def test_the_x_profile_feeds_marketing_without_touching_the_page():
    x_reader = (EXTENSION / "x.js").read_text(encoding="utf-8")
    script = MANIFEST["content_scripts"][2]
    assert script == {"matches": ["https://x.com/*", "https://twitter.com/*"],
                      "js": ["x.js"], "run_at": "document_idle"}
    # Reading only: nothing is injected into X, fetched from it, or logged
    # where the page could see it; failures go to the popup's widget.
    for forbidden in ("chrome.storage", "fetch(", "sessionToken", "console.",
                      "appendChild", ".append(", "insertAdjacent", "innerHTML",
                      "setAttribute", "XMLHttpRequest", "postMessage", "throw "):
        assert forbidden not in x_reader
    assert 'type: "fm-x-error"' in x_reader and 'type: "fm-x-observe"' in x_reader
    assert "chrome.runtime.lastError" in x_reader
    # The counts the profile shows.
    for selector in ('article[data-testid="tweet"]', '[data-testid="retweet"]',
                     '"/notifications/mentions"', "photos?", "article:not([data-testid])",
                     '[data-engagement-action="like"]', "snowflakeTime(match[2])"):
        assert selector in x_reader
    # The service worker keeps them, shows them in the popup, and records a
    # Marketing snapshot for the X channel only when "Sync now" is pressed.
    for key in ("fm-x-handle", "fm-x-observe", "fm-x-error", "fm-x-stats",
                "fm-x-sync", "fm-x-set-handle", "fm-x-clear-errors", "fm-open-x"):
        assert f'"{key}"' in BACKGROUND
    for key in ("fm-x-stats", "fm-x-sync", "fm-x-set-handle", "fm-open-x"):
        assert f'"{key}"' in POPUP
    assert 'const X_DEFAULT_HANDLE = "forkmesh";' in BACKGROUND
    # An observation is stored and nothing else: no schedule, no snapshot.
    observe = BACKGROUND.split("async function xObserve(", 1)[1].split("\n}\n", 1)[0]
    assert "xSave(state)" in observe and "xSync(" not in observe
    assert "X_SYNC_EVERY" not in BACKGROUND
    assert BACKGROUND.count("await xSync(state)") == 1
    assert "xSync(state)" in BACKGROUND.split("const xSyncNow =", 1)[1]
    assert 'channel: "x", ...counters' in BACKGROUND
    assert 'source: "x-profile"' in BACKGROUND
    assert "`/api/marketing/${encodeURIComponent(organization)}/metrics`" in BACKGROUND
    assert "data-x-section" in POPUP_HTML and "data-x-errors" in POPUP_HTML
    marketing = (REPO / "marketplace" / "packages" / "marketing" / "content"
                 / "worker" / "marketing.py").read_text(encoding="utf-8")
    for key in ("following", "posts", "replies", "reposts", "likes", "mentions", "media"):
        assert f'"{key}"' in marketing.split("SOCIAL_METRICS = (", 1)[1].split(")", 1)[0]


def test_install_text_names_the_folder_chrome_loads_from_a_checkout():
    assert "manifest_version" not in PACKAGE_MANIFEST
    checkout = "marketplace/packages/browser-extension/content/extension"
    assert (REPO / checkout / "manifest.json").is_file()
    assert f"<code>{checkout}</code>" in PAGE_VIEW
    assert checkout in CRM_DESKTOP
    assert "the one with manifest.json directly inside" in PAGE_SCRIPT


def test_mail_you_send_is_tracked_on_the_recipient_who_has_a_record():
    assert '"fm-track-sent": trackSent' in BACKGROUND
    track = BACKGROUND.split("async function trackSent(", 1)[1].split("\n}\n", 1)[0]
    assert "createContact: false" in track
    assert 'direction: "outbound"' in track
    assert 'error?.code === "contact_not_found"' in track
    assert "if (!state.track || !current.sessionToken || !current.organization)" in track
    assert "createContact: message.createContact !== false" in BACKGROUND
    assert "return { track: stored.track !== false" in BACKGROUND
    assert '{ type: "fm-track-sent", message: { ...message, messageId, providerUrl } }' in CONTENT
    assert 'document.addEventListener("click", (event) => {' in CONTENT
    assert '#link_vsm[param]' in CONTENT
    assert 'attr(control, "id").startsWith("link_undo")' in CONTENT
    assert 'window.addEventListener("pagehide", trackSent);' in CONTENT
    assert "const SEND_GRACE = 10000;" in CONTENT
    assert "data-track-sent" in POPUP_HTML
    assert '{ type: "fm-sent-set-track", track: event.target.checked }' in POPUP


def test_a_zoho_subject_is_never_the_folder_name_and_stays_editable():
    assert "const ZOHO_FOLDER_NAMES = /^(inbox|sent|sent items|spam|" in CONTENT
    assert "bare === folder || ZOHO_FOLDER_NAMES.test(bare)" in CONTENT
    assert "subject = subjectOf(rows.flatMap((row) => [...row.querySelectorAll(SUBJECT_SELECTORS.join(\",\"))]));" in CONTENT
    assert "subject = zohoSubject(document.title" in CONTENT
    assert "zohoName(node, email, pane)" in CONTENT
    assert '<input class="subject" data-field="subject"' in PANEL
    assert "if (ui.message && !ui.message.contactName && contact.name) ui.message.contactName = contact.name;" in PANEL
    assert 'else _clean(data.get("contactName") or contact_data.get("name"), 120)),' in WORKER


def test_the_reddit_desk_is_filled_from_pages_you_open_without_touching_reddit():
    reader = (EXTENSION / "reddit.js").read_text(encoding="utf-8")
    script = MANIFEST["content_scripts"][3]
    assert script == {"matches": ["https://www.reddit.com/*", "https://old.reddit.com/*"],
                      "js": ["reddit.js"], "run_at": "document_idle"}
    for forbidden in ("chrome.storage", "fetch(", "sessionToken", "console.",
                      "appendChild", ".append(", "insertAdjacent", "innerHTML",
                      "setAttribute", "XMLHttpRequest", "postMessage", "throw ",
                      "textContent =", "innerText =", ".click(", "dispatchEvent",
                      "rulesChecked", "disclosed", "/api/"):
        assert forbidden not in reader, forbidden
    for message in ('type: "fm-reddit-watch"', 'type: "fm-reddit-observe"',
                    'type: "fm-reddit-error"'):
        assert message in reader
    assert "chrome.runtime.lastError" in reader
    assert 'document.visibilityState === "visible"' in reader
    assert "key !== lastSent" in reader
    for selector in ("shreddit-post", '"post-title"', '"comment-count"', '"created-timestamp"',
                     'div.thing[data-fullname^="t3_"]', '"data-comments-count"', "a.title",
                     'a[href*="/comments/"]', ".subreddit-rule-item", ".rule-title",
                     ".rule-description"):
        assert selector in reader, selector
    assert 'section === "about" && String(parts[3] || "").toLowerCase() === "rules"' in reader
    assert "watch.subreddits.has(subreddit)" in reader
    assert "watch.queries.has(searched)" in reader

    for key in ("fm-reddit-watch", "fm-reddit-observe", "fm-reddit-error", "fm-reddit-stats",
                "fm-reddit-refresh", "fm-reddit-sync", "fm-reddit-clear-errors",
                "fm-open-reddit-desk"):
        assert f'"{key}"' in BACKGROUND, key
    for key in ("fm-reddit-stats", "fm-reddit-refresh", "fm-reddit-sync",
                "fm-reddit-clear-errors", "fm-open-reddit-desk"):
        assert f'"{key}"' in POPUP, key
        assert f'"{key}"' in BACKGROUND.split("const POPUP_ONLY = new Set(", 1)[1].split(");", 1)[0]
    assert "POPUP_ONLY.has(request.type)) && !mayUseVault(sender)" in BACKGROUND
    observe = BACKGROUND.split("async function redditObserve(", 1)[1].split("\n}\n", 1)[0]
    assert "redditSave(state)" in observe
    for forbidden in ("api(", "redditSync(", "redditFetchWatch("):
        assert forbidden not in observe
    watch = BACKGROUND.split("const redditWatch = async", 1)[1].split("\n};\n", 1)[0]
    assert "api(" not in watch and "redditFetchWatch(" not in watch
    assert BACKGROUND.count("await redditSync(state)") == 2, (
        'only "Send to Reddit desk" and the end of a walk send the desk anything')
    assert "redditSync(state)" in BACKGROUND.split("const redditSyncNow =", 1)[1]
    assert "redditSync(state)" in BACKGROUND.split("async function redditWalkRun(", 1)[1]
    assert BACKGROUND.count("await redditFetchWatch(state)") == 8
    assert "await redditFetchWatch(current)" in BACKGROUND.split("async function redditWalkStart(", 1)[1]
    for caller in ("const redditStatsNow =", "const redditRefresh =",
                   "async function redditSync(state)", "const redditMonitorAdd =",
                   "const redditMonitorRemove =", "async function redditCheck()",
                   "async function redditProfileUpdate("):
        assert "await redditFetchWatch(state)" in BACKGROUND.split(caller, 1)[1].split("\n}", 1)[0]
    assert '"fm-reddit-profile-update"' in BACKGROUND.split("const POPUP_ONLY = new Set(", 1)[1].split(");", 1)[0]
    assert 'request?.type === "fm-reddit-profile-read" && sender?.id === chrome.runtime.id && !sender.tab' in reader
    assert "if (profile) return watch.accounts.has(profile.account.toLowerCase()) ? profile : null;" in reader
    mine = BACKGROUND.split("async function redditSendMine(", 1)[1].split("\n}\n", 1)[0]
    assert "`${base}/posts`" in mine and "api(" in mine
    for key in ("fm-reddit-monitor-add", "fm-reddit-monitor-remove", "fm-reddit-check",
                "fm-reddit-auto-check"):
        assert f'"{key}"' in POPUP and f'"{key}"' in BACKGROUND, key
        assert f'"{key}"' in BACKGROUND.split("const POPUP_ONLY = new Set(", 1)[1].split(");", 1)[0]
    assert 'request?.type !== "fm-reddit-read" || sender?.id !== chrome.runtime.id || sender.tab' in reader
    assert "chrome.tabs.create({ url: entry.commentUrl || entry.url, active: false })" in BACKGROUND
    sync = BACKGROUND.split("async function redditSync(state)", 1)[1].split("\n}\n", 1)[0]
    assert 'status: "to_review", source: "extension"' in sync
    assert 'error.code === "reddit_thread_exists"' in sync
    assert "`${base}/reddit`" in sync and "`${base}/reddit-rules`" in sync
    for marker in ("rulesChecked", "disclosed", "replyUrl"):
        assert marker not in BACKGROUND
    assert "`/api/marketing/${encodeURIComponent(organization)}/reddit`" in BACKGROUND
    assert "/dashboard/marketing?" in BACKGROUND and 'page: "reddit"' in BACKGROUND
    assert "https://old.reddit.com/r/${name}/about/rules" in BACKGROUND
    assert "data-reddit-section" in POPUP_HTML and "data-reddit-errors" in POPUP_HTML
    for label in ("Refresh watch list", "Send to Reddit desk", "Open Reddit desk"):
        assert label in POPUP_HTML
    permission = next(item for item in PACKAGE_MANIFEST["permissions"]
                      if item["id"] == "browser.reddit.read")
    assert permission["risk"] == "low"

    marketing = (REPO / "marketplace" / "packages" / "marketing" / "content"
                 / "worker" / "marketing.py").read_text(encoding="utf-8")
    assert '"reddit-rules"' in marketing
    assert 'REDDIT_SOURCES = ("manual", "extension")' in marketing


@pytest.mark.skipif(shutil.which("node") is None, reason="node is not installed")
def test_the_reddit_reader_and_desk_sync_run_against_the_marketing_worker():
    result = subprocess.run(
        [shutil.which("node"), "--no-warnings",
         str(PACKAGE / "tests" / "background_reddit_harness.js")],
        capture_output=True, text=True, timeout=300,
        env={"PATH": "/usr/local/bin:/usr/bin:/bin", "PYTHON": sys.executable})
    assert result.returncode == 0, result.stdout[-3000:] + result.stderr[-2000:]
    assert "checks passed" in result.stdout


@pytest.mark.skipif(shutil.which("node") is None, reason="node is not installed")
def test_scrolling_the_x_profile_adds_its_posts_as_marketing_links():
    result = subprocess.run(
        [shutil.which("node"), "--no-warnings",
         str(PACKAGE / "tests" / "background_x_posts_harness.js")],
        capture_output=True, text=True, timeout=300,
        env={"PATH": "/usr/local/bin:/usr/bin:/bin", "PYTHON": sys.executable})
    assert result.returncode == 0, result.stdout[-3000:] + result.stderr[-2000:]
    assert "checks passed" in result.stdout


def test_every_marketing_channel_updates_from_its_own_page_in_one_click():
    reader = (EXTENSION / "marketing-capture.js").read_text(encoding="utf-8")
    capture_page = (EXTENSION / "capture.js").read_text(encoding="utf-8")
    for script in MANIFEST["content_scripts"]:
        assert "marketing-capture.js" not in script["js"]
    assert 'files: ["marketing-capture.js"]' in BACKGROUND
    for forbidden in ("chrome.", "fetch(", "sessionToken", "XMLHttpRequest", "postMessage",
                      "appendChild", ".append(", "insertAdjacent", "innerHTML", "setAttribute",
                      ".click(", "console.", "localStorage"):
        assert forbidden not in reader
    assert "globalThis.__forkmeshMarketingRead" in reader
    start = BACKGROUND.split("async function captureStart(", 1)[1].split("\n}\n", 1)[0]
    assert "captureChannels(org)" in start and "target.url" in start
    assert "url:" not in DASHBOARD_BRIDGE.split('type: "fm-marketing-capture"', 1)[1].split("})", 1)[0]
    assert "chrome.permissions.contains({ origins: [captureOrigin(target.url)] })" in start
    assert "chrome.permissions.request({ origins: [origin] }" in capture_page
    assert MANIFEST["optional_host_permissions"] == ["https://*/*"]
    assert '"fm-marketing-capture": captureStart' in BACKGROUND
    assert 'url.pathname.startsWith("/dashboard/marketing")' in BACKGROUND
    assert 'const CAPTURE_PAGE_ONLY = new Set(["fm-marketing-capture-job", "fm-marketing-capture-continue"]);' in BACKGROUND
    assert '"fm-marketing-capture-tab", "fm-marketing-setup"' in BACKGROUND
    assert "`/api/marketing/${encodeURIComponent(organization)}/capture`" in BACKGROUND
    assert 'dataset.forkmeshMarketingCapture = version' in DASHBOARD_BRIDGE
    assert '"fm-marketing-capture-update"' in DASHBOARD_BRIDGE
    for hook in ("data-marketing-section", "data-marketing-send", "data-marketing-allow", "data-marketing-history"):
        assert hook in POPUP_HTML
    assert 'type: "fm-marketing-capture-tab"' in POPUP
    assert "chrome.permissions.request({ origins: patterns }" in POPUP


def test_reddit_and_channel_sites_open_the_popup_on_their_own_buttons():
    assert '<div data-here class="row here" hidden></div>' in POPUP_HTML
    assert "data-refresh" not in POPUP_HTML
    assert POPUP_HTML.index("data-here") < POPUP_HTML.index("data-vault-section")
    here = POPUP.split("async function renderHere()", 1)[1].split("\n}\n", 1)[0]
    assert "MAIL_URL.test(url)" in here and "FORKMESH_HOST.test(host)" in here
    assert 'send({ type: "fm-marketing-setup" })' in here
    assert 'send({ type: "fm-reddit-stats" })' in here
    assert "const channel = setup.ok ? channelFor(setup, url) : null;" in here
    plan = POPUP.split("function channelPlan(", 1)[1].split("\n}\n", 1)[0]
    assert "label: `Update ${name} stats from this page`" in plan
    assert "const update = covered ? [] :" in plan and plan.count("actions: update") == 2 and "...update,\n" in plan
    assert "plan?.refreshes?.includes(channel.id)" in here
    assert 'ForkMeshRefreshView.render($("[data-here]"), here.plan' in POPUP
    for message in ('hereReddit("fm-reddit-sync")', 'hereReddit("fm-reddit-refresh")',
                    'type: "fm-open-reddit-desk"', 'type: "fm-marketing-capture-tab", tabId: tab.id',
                    'type: "fm-marketing-capture", channel: channel.id', 'type: "fm-x-sync"'):
        assert message in POPUP, message
    assert "https://old.reddit.com/r/${encodeURIComponent(page.name)}/about/rules" in POPUP
    assert "innerHTML" not in here
    stats = BACKGROUND.split("async function redditStats()", 1)[1].split("\n}\n", 1)[0]
    assert "watched:" in stats and "watchedQueries:" in stats


@pytest.mark.skipif(shutil.which("node") is None, reason="node is not installed")
def test_the_channel_reader_and_capture_flow_run_against_the_marketing_worker():
    result = subprocess.run(
        [shutil.which("node"), "--no-warnings",
         str(PACKAGE / "tests" / "background_marketing_capture_harness.js")],
        capture_output=True, text=True, timeout=300,
        env={"PATH": "/usr/local/bin:/usr/bin:/bin", "PYTHON": sys.executable})
    assert result.returncode == 0, result.stdout[-3000:] + result.stderr[-2000:]
    assert "checks passed" in result.stdout


def test_the_x_setup_guide_runs_only_on_the_developer_console_and_only_when_started():
    guide = (EXTENSION / "x-developer.js").read_text(encoding="utf-8")
    assert MANIFEST["content_scripts"][4] == {
        "matches": ["https://console.x.com/*", "https://developer.x.com/*", "https://developer.twitter.com/*"],
        "js": ["x-developer.js"], "run_at": "document_idle"}
    for forbidden in ("fetch(", "XMLHttpRequest", "console.", "localStorage", "sessionToken", "innerHTML", "postMessage"):
        assert forbidden not in guide
    assert 'attachShadow({ mode: "closed" })' in guide
    start = guide.rsplit('void send({ type: "fm-x-setup-state" })', 1)[1]
    assert "if (!response?.setup?.active) return;" in start
    assert guide.index("function watch()") < guide.rindex('void send({ type: "fm-x-setup-state" })')
    assert 'const X_SETUP_PORTAL_ONLY = new Set(["fm-x-setup-keys", "fm-x-setup-mark", "fm-x-setup-forget", "fm-x-setup-back"]);' in BACKGROUND
    assert '(request.type === "fm-x-setup-start" && !mayUseVault(sender) && !captureFromDashboard(sender))' in BACKGROUND
    assert "`/api/marketing/${encodeURIComponent(setup.organization)}/x-connection`" in BACKGROUND
    assert "chrome.storage.session.set({ [X_SETUP_KEY]: setup })" in BACKGROUND
    block = BACKGROUND.split("const X_SETUP_KEY", 1)[1].split("const POPUP_ONLY", 1)[0]
    assert "chrome.storage.local" not in block
    assert 'type: "fm-x-setup-start"' in DASHBOARD_BRIDGE
    assert 'reply({ type: "marketing-x-setup-update", setup: message.setup })' in DASHBOARD_BRIDGE
    assert "dataset.forkmeshMarketingXSetup = version" in DASHBOARD_BRIDGE


@pytest.mark.skipif(shutil.which("node") is None, reason="node is not installed")
def test_the_x_setup_guide_saves_the_keys_it_reads_in_marketing():
    result = subprocess.run(
        [shutil.which("node"), "--no-warnings",
         str(PACKAGE / "tests" / "background_x_setup_harness.js")],
        capture_output=True, text=True, timeout=300,
        env={"PATH": "/usr/local/bin:/usr/bin:/bin", "PYTHON": sys.executable})
    assert result.returncode == 0, result.stdout[-3000:] + result.stderr[-2000:]
    assert "checks passed" in result.stdout


def test_the_refresh_panel_follows_the_tab_without_touching_any_channel_site():
    assert not (EXTENSION / "refresh-bar.js").exists()
    assert MANIFEST["side_panel"] == {"default_path": "refresh-panel.html"}
    for name in ("refresh-panel.html", "refresh-panel.js", "refresh-view.js"):
        assert (EXTENSION / name).is_file()
    assert [script["js"] for script in MANIFEST["content_scripts"]] == [
        ["panel.js", "content.js"], ["dashboard.js"], ["x.js"], ["reddit.js"], ["x-developer.js"]]
    for gone in ("fm-bar-", "BAR_", "refresh-bar", "barMark", "fm-refresh-bar"):
        assert gone not in BACKGROUND, gone
    assert BACKGROUND.count("chrome.scripting.registerContentScripts(") == 1
    assert "registerContentScripts(missing.map((host) => ({" in BACKGROUND
    assert BACKGROUND.count("executeScript(") == 6, (
        "three for a channel capture, two to pace one page of a Reddit walk, "
        "one to load the Reddit reader into a profile tab opened before the extension")
    section = BACKGROUND.split('const REFRESH_SETUP_KEY = "refreshSetup";', 1)[1].split("const X_SETUP_KEY", 1)[0]
    update = section.split("async function refreshChannel(", 1)[1].split("\n}\n", 1)[0]
    planned = section.replace(update, "")
    for forbidden in ("executeScript", "registerContentScripts", "insertCSS", "tabs.sendMessage",
                      "permissions.contains", "captureTab(", "captureRead("):
        assert forbidden not in planned, forbidden
    assert update.count("captureTab(") == 1 and "captureRead(" not in update
    assert update.index("if (!target.refreshable && !onPage) throw") < update.index("captureTab(")
    step = section.split("async function refreshStep(", 1)[1].split("\n}\n", 1)[0]
    for reused in ("await captureRefresh(job)", "await xSyncNow()", "await redditSyncNow()"):
        assert reused in step, reused
    assert "refreshable: refreshable.has(item.id)" in BACKGROUND
    assert "new Set(payload.refreshableChannels || [])" in BACKGROUND
    assert "`/api/marketing/${encodeURIComponent(job.organization)}/refresh`" in BACKGROUND
    assert '"fm-refresh-plan", "fm-refresh-run", "fm-refresh-channel"]);' in BACKGROUND
    assert "const REFRESH_NEXT_MS = 60 * 60 * 1000;" in BACKGROUND
    assert '"fm-refresh-channel": refreshChannel' in BACKGROUND
    assert "chrome.action.setBadgeText({ tabId," in section
    assert "chrome.tabs.onActivated" in section and "chrome.tabs.onUpdated" in section
    panel = (EXTENSION / "refresh-panel.js").read_text(encoding="utf-8")
    view = (EXTENSION / "refresh-view.js").read_text(encoding="utf-8")
    panel_html = (EXTENSION / "refresh-panel.html").read_text(encoding="utf-8")
    assert "chrome.tabs.update(tab.id, { url })" in panel
    assert "chrome.tabs.onActivated.addListener" in panel and "chrome.tabs.onUpdated.addListener" in panel
    assert '"fm-refresh-changed"' in panel
    assert "innerHTML" not in view + panel
    assert '<script src="refresh-view.js"></script>' in panel_html and '<script src="refresh-view.js"></script>' in POPUP_HTML
    assert "chrome.sidePanel.open({ windowId: here.tab.windowId })" in POPUP
    assert "Open refresh panel" in view


@pytest.mark.skipif(shutil.which("node") is None, reason="node is not installed")
def test_the_refresh_panel_plans_and_runs_against_the_marketing_worker():
    result = subprocess.run(
        [shutil.which("node"), "--no-warnings",
         str(PACKAGE / "tests" / "background_refresh_panel_harness.js")],
        capture_output=True, text=True, timeout=300,
        env={"PATH": "/usr/local/bin:/usr/bin:/bin", "PYTHON": sys.executable})
    assert result.returncode == 0, result.stdout[-3000:] + result.stderr[-2000:]
    assert "checks passed" in result.stdout
