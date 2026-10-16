"""Desktop plugin lifecycle contracts: proof constants, safe uninstall order."""

import ast
import json
import re
from pathlib import Path


CRM_PACKAGE = Path(__file__).resolve().parents[1]
REPO = CRM_PACKAGE.parents[2]
ENTRY = (REPO / "app" / "src" / "entry.py").read_text(encoding="utf-8")
CRM_WORKER = (CRM_PACKAGE / "content" / "worker" / "crm.py").read_text(
    encoding="utf-8")
INTERNAL = (REPO / "desktop" / "src" / "MainWindowInternal.h").read_text(
    encoding="utf-8")
HOST = (REPO / "desktop" / "src" / "MainWindowMarket.cpp").read_text(
    encoding="utf-8")
FEATURE = (CRM_PACKAGE / "content" / "desktop" / "CrmFeature.cpp").read_text(
    encoding="utf-8")
FEATURE_HEADER = (CRM_PACKAGE / "content" / "desktop" / "CrmFeature.h").read_text(
    encoding="utf-8")
MANIFEST = (CRM_PACKAGE / "manifest.json").read_text(encoding="utf-8")
WEB_VIEW = (CRM_PACKAGE / "content" / "web" / "view.html").read_text(
    encoding="utf-8")
WEB_SCRIPT = (CRM_PACKAGE / "content" / "web" / "page.js").read_text(
    encoding="utf-8")
THEME = (REPO / "desktop" / "src" / "Theme.h").read_text(encoding="utf-8")


def _entry_constant(name, source=None):
    tree = ast.parse(source if source is not None else ENTRY)
    for node in tree.body:
        if isinstance(node, ast.Assign) and any(
                isinstance(target, ast.Name) and target.id == name
                for target in node.targets):
            return ast.literal_eval(node.value)
    raise AssertionError(f"missing {name}")


def _header_constant(name):
    match = re.search(
        name + r"\s*=\s*\n?\s*QStringLiteral\(\"([^\"]+)\"\)", INTERNAL)
    assert match, f"missing {name}"
    return match.group(1)


def test_desktop_proof_constants_match_the_worker_byte_for_byte():
    crm_proof = _entry_constant("REQUEST_PROOF", CRM_WORKER)
    assert f'QStringLiteral("{crm_proof}")' in FEATURE
    assert _header_constant("kPluginRequestProof") == _entry_constant(
        "PLUGIN_REQUEST_PROOF")
    assert crm_proof != _entry_constant("PLUGIN_REQUEST_PROOF")


def test_generic_host_signs_each_api_family_with_its_own_proof():
    assert "lifecycleApi ? forkmesh::ui::kPluginRequestProof" in HOST
    assert ": contribution.requestProof" in HOST
    assert "crm" not in HOST.lower()


def test_package_registers_its_native_surface_from_inside_the_package():
    assert "NativePackageRegistration crmRegistration" in FEATURE
    assert 'QStringLiteral("builtin:crm")' in FEATURE
    assert 'QStringLiteral("/api/crm")' in FEATURE


def test_desktop_uninstall_downloads_the_export_before_deleting_anything():
    body = FEATURE.split("void CrmFeature::uninstallPlugin()", 1)[1]
    body = body.split("\nvoid CrmFeature::", 1)[0]
    # Explicit confirmation, then export, then uninstall - in that order.
    confirm = body.index("QMessageBox::warning")
    export_call = body.index('pluginPath(QStringLiteral("/export"))')
    uninstall_call = body.index('pluginPath(QStringLiteral("/uninstall"))')
    assert confirm < export_call < uninstall_call
    # A failed or cancelled save aborts the uninstall entirely.
    guard = body.index("!saveExportBundle(payload)")
    assert export_call < guard < uninstall_call
    assert body.count("return;") >= 3


def test_desktop_plugin_surfaces_keep_no_settings_or_keys():
    # The CRM page persists no settings and caches no key; the records and
    # pictures it saves are the cache below.
    for source in (FEATURE, FEATURE_HEADER):
        assert "QSettings" not in source
    assert "setEchoMode(QLineEdit::Password)" in FEATURE


def _feature_body(name):
    return FEATURE.split(f"CrmFeature::{name}(", 1)[1].split("\n}\n", 1)[0]


def test_desktop_draws_the_saved_copy_then_only_asks_what_changed():
    # The first open draws the organizations and records last read, before
    # the worker is asked anything.
    refresh = _feature_body("refreshOrganizations")
    assert refresh.index("drawCachedOrganizations()") < refresh.index('"/api/crm"')
    assert "drawCachedSummary(" in _feature_body("drawCachedOrganizations")
    # The saved copy's revision is what the re-read sends, so an unchanged
    # organization answers `unchanged` and nothing is redrawn.
    assert "applyOrganization(summary)" in _feature_body("drawCachedSummary")
    load = _feature_body("loadOrganization")
    assert load.index("drawCachedSummary(organization)") < load.index('QStringLiteral("since"), m_revision')
    unchanged = load.split('"unchanged"', 1)[1].split("return;", 1)[0]
    assert "applyOrganization" not in unchanged
    # A full answer replaces the copy; a copy is only drawn for the account
    # and organization it was saved as, and another account's is cleared.
    assert load.index("applyOrganization(payload)") < load.index("cacheSummary(payload)")
    summary = _feature_body("drawCachedSummary")
    assert 'text(record, "actor") != m_actor' in summary
    assert 'text(record, "organization") != organization' in summary
    assert "applyOrganization(QJsonObject())" in refresh
    # The copy goes with the plugin.
    assert "forgetCachedSummary(organization)" in load
    assert "forgetCachedSummary(name)" in _feature_body("uninstallPlugin")


def test_desktop_cache_files_are_owner_only_and_hold_no_secrets():
    opener = FEATURE.split("bool openCacheFile(QSaveFile &file)", 1)[1].split("\n}\n", 1)[0]
    assert "file.setPermissions(owner)" in opener
    assert "QFileDevice::ReadOwner | QFileDevice::WriteOwner" in opener
    assert "QStandardPaths::CacheLocation" in FEATURE
    # Only the summary the worker already sends is saved, and it never
    # carries the Zoho secrets (test_crm checks the summary itself).
    # A picture comes off disk for a week before its service is asked again.
    avatar = _feature_body("fetchAvatar")
    assert avatar.index("avatarCachePath(source)") < avatar.index("m_network->get")
    assert "kAvatarCacheMs" in avatar
    assert "avatarCachePath(source)" in _feature_body("fetchAvatarImage")


def test_zoho_protocol_stays_in_the_package_behind_a_generic_host_capability():
    assert "zoho" not in ENTRY.lower()
    assert "async def outbound_json(" in ENTRY
    assert "outbound_origin_not_allowed" in ENTRY
    assert "redirect\": \"manual" in ENTRY
    assert "/oauth/v2/token" in CRM_WORKER
    assert "/messages/view?" in CRM_WORKER
    manifest = json.loads(MANIFEST)
    origins = manifest["worker"]["outboundOrigins"]
    assert "https://accounts.zoho.com" in origins
    assert "https://mail.zoho.eu" in origins


def test_web_crm_is_mobile_usable_and_reads_shared_state_only_when_asked():
    assert "data-crm-contact-cards" in WEB_VIEW
    assert "sm:hidden" in WEB_VIEW
    assert "Twitter" in WEB_VIEW
    assert "Mastodon" in WEB_VIEW
    assert "data-crm-contact-delete-row" in WEB_SCRIPT
    assert "data-crm-contact-inline-field" in WEB_SCRIPT
    assert "m_contactsTable->setCellWidget(row, column, actions)" in FEATURE
    assert "updateContactField(id->data(Qt::UserRole).toString(), spec, item->text())" in FEATURE
    assert "data-crm-email-new" in WEB_VIEW
    assert "Compose one-off" in WEB_VIEW
    # Native labels open mobile file providers without relying on a synthetic
    # click on a detached hidden input.
    assert re.search(
        r"<label data-crm-import[^>]*>.*?<input data-crm-import-file",
        WEB_VIEW, re.DOTALL)
    # Refresh is a header button, not a menu item buried under "More": it is
    # the only thing that reads the organization's CRM again.
    assert re.search(r'<button data-crm-refresh[^>]*class="hidden ', WEB_VIEW)
    assert '$(`[data-crm-refresh]`)?.addEventListener("click", () => void loadCrm())' in WEB_SCRIPT
    # Nothing re-reads on a timer, on focus, or on a tab becoming visible.
    assert "setInterval" not in WEB_SCRIPT
    assert 'addEventListener("focus"' not in WEB_SCRIPT
    assert "visibilitychange" not in WEB_SCRIPT
    assert '$(`[data-crm-email-new]`)?.addEventListener' in WEB_SCRIPT


def test_crm_text_does_not_paint_dark_boxes_behind_dialog_labels():
    assert '[data-view="crm"] dialog label { background: transparent !important; }' in WEB_VIEW
    # Both desktop themes make dialog labels inherit their dialog canvas.
    assert THEME.count("QDialog QLabel { background: transparent; }") == 2


def test_all_members_can_refresh_a_configured_shared_zoho_connection():
    sync = FEATURE.split("void CrmFeature::syncZohoMail()", 1)[1]
    sync = sync.split("\nvoid CrmFeature::", 1)[0]
    assert "m_canManageSettings" not in sync
    assert "connected" in sync
    # The desktop page has no timer of its own: it loads when it is opened
    # (refresh()) and when the Refresh button is pressed.
    assert "setInterval(30000)" not in FEATURE
    assert "QTimer" in FEATURE and "->start()" not in FEATURE


def _worker_contact_fields():
    body = CRM_WORKER.split("def _contact_data(", 1)[1].split("\n\n\n", 1)[0]
    return re.findall(r'\("([a-z]+)", \d+,', body)


def test_every_surface_offers_the_same_contact_profile_fields():
    # The profile columns are only useful if a handle typed on the desktop is
    # the same field the worker stores and the web page renders back.
    profile = ["twitter", "mastodon", "bluesky", "linkedin", "github",
               "website", "location"]
    assert set(profile) <= set(_worker_contact_fields())
    desktop = re.findall(r'\{"([a-z]+)", "[^"]+", \d+\}', FEATURE)
    web = re.findall(r'\{ field: "([a-z]+)"', WEB_SCRIPT)
    assert set(profile) <= set(desktop)
    assert set(profile) <= set(web)
    for field in profile:
        assert f"data-crm-contact-{field}" in WEB_VIEW
        assert f'{{QStringLiteral("{field}"), {field}->text()}}' in FEATURE


def test_contact_rows_render_one_cell_per_table_heading():
    head = WEB_VIEW.split("<tr data-crm-contact-head>", 1)[1].split("</tr>", 1)[0]
    headings = re.findall(r"<th[^>]*>([^<]+)</th>", head)
    # A contact column may be camelCase ("nextStep"), and the web list is read
    # from the editable-column table itself so the worker-owned history column
    # is not counted twice.
    editable = WEB_SCRIPT.split("const CRM_CONTACT_COLUMNS = [", 1)[1].split("\n  ];", 1)[0]
    web = re.findall(r'\{ field: "([a-zA-Z]+)"', editable)
    # Editable columns, then the two the worker owns: history and row actions.
    assert len(headings) == len(web) + 2
    assert headings[-2:] == ["Last contacted", "Actions"]
    desktop = re.findall(r'\{"([a-zA-Z]+)", "[^"]+", \d+\}', FEATURE)
    assert desktop == web
    # The desktop table is laid out from a column list: the built-in fields,
    # then any custom field a contact carries, then the computed Mastodon and
    # last-contacted columns, the row actions, and the "+" that adds a column.
    layout = FEATURE.split("void CrmFeature::rebuildContactColumns()", 1)[1]
    layout = layout.split("\nint CrmFeature::columnIndex(", 1)[0]
    order = []
    for kind in re.findall(r"ContactColumnSpec::(\w+)", layout):
        if kind in ("Field", "CustomField", "MastodonLatest", "LastContacted",
                    "LinkedEmails", "LinkedToots", "Actions", "AddColumn") and kind not in order:
            order.append(kind)
    assert order == ["Field", "CustomField", "MastodonLatest", "LastContacted",
                     "LinkedEmails", "LinkedToots", "Actions", "AddColumn"]
    assert 'QStringLiteral("Mastodon last update")' in layout
    # Action buttons are laid out inside a row tall enough to show them whole,
    # which is what stopped the Email/Edit/Delete labels being sliced in half.
    assert "verticalHeader()->setDefaultSectionSize(36)" in FEATURE
    assert "setFixedHeight(24)" not in FEATURE


def test_desktop_stage_cells_and_mastodon_row_refresh():
    fill = FEATURE.split("void CrmFeature::fillContactRows()", 1)[1].split("\n}\n", 1)[0]
    # A stage cell carries the same color its chevron has in the stage strip.
    assert "CrmStageStrip::stageColor(" in fill
    assert "flowStages(source.flow).indexOf(value)" in fill
    assert "item->setIcon(*dot)" in fill
    # Each Mastodon cell carries its own refresh button that reads that row's
    # account again, and rests while the read is queued or in flight. Besides
    # it, only the Reach out card queues reads: drawing a row never reaches an
    # instance.
    assert "m_mastodonRefresh" not in FEATURE
    assert "item->setData(kRowButtonRole, true)" in fill
    assert "[this, key] { refreshMastodonNow(key); }" in fill
    now = FEATURE.split("void CrmFeature::refreshMastodonNow(const QString &key)", 1)[1].split("\n}\n", 1)[0]
    assert "latest->fetchedAt = 0" in now and "refreshMastodonUpdates()" in now
    assert "m_mastodonQueue.append(key)" in now
    updates = FEATURE.split("void CrmFeature::refreshMastodonUpdates()", 1)[1].split("\n}\n", 1)[0]
    assert "m_mastodonQueue.takeFirst()" in updates
    assert "m_snapshot.rows" not in updates
    assert "refresh->setEnabled(!m_mastodonPending.contains(key) &&" in updates
    assert FEATURE.count("m_mastodonQueue.append(") == 2
    reach = FEATURE.split("void CrmFeature::queueReachMastodon(const QStringList &ranked)", 1)[1].split("\n}\n", 1)[0]
    assert "m_mastodonQueue.append(key)" in reach and "kReachMastodonReads" in reach
    assert "kMastodonPerHost" in updates and "m_mastodonBackoff.value(host)" in updates
    fetch = FEATURE.split("void CrmFeature::fetchMastodonLatest(const QString &key)", 1)[1].split("\n}\n", 1)[0]
    assert "markExpectedHttpStatus(lookupRequest, 429)" in fetch and 'rawHeader("Retry-After")' in fetch
    assert "m_mastodonIds.value(key)" in fetch and "scheduleNextRender();" in fetch


def test_desktop_contact_names_carry_pictures_read_on_load_only():
    fill = FEATURE.split("void CrmFeature::fillContactRows()", 1)[1].split("\n}\n", 1)[0]
    # The name cell wears the contact's picture, or its initials meanwhile.
    assert "item->setIcon(contactAvatar(source))" in fill
    assert "item->setData(kAvatarRole, true)" in fill
    assert "m_network" not in fill
    # Only a load (opening the page, or Refresh) asks for pictures.
    assert FEATURE.count("m_avatarsWanted = true") == 1
    load = FEATURE.split("void CrmFeature::loadOrganization()", 1)[1].split("\n}\n", 1)[0]
    assert "m_avatarsWanted = true" in load
    sources = FEATURE.split("QStringList CrmFeature::avatarSources(", 1)[1].split("\n}\n", 1)[0]
    assert sources.index('"mastodon:"') < sources.index('"bluesky:"') < sources.index('"github:"')
    assert "avatars.githubusercontent.com" in FEATURE
    assert "app.bsky.actor.getProfile" in FEATURE
    catalog = json.loads((REPO / "desktop" / "resources" / "network" /
                          "endpoint-catalog.json").read_text(encoding="utf-8"))
    patterns = {profile["pattern"] for profile in catalog["endpoints"]}
    assert {"avatars.githubusercontent.com/*",
            "public.api.bsky.app/xrpc/app.bsky.actor.getProfile",
            "cdn.bsky.app/img/avatar/*"} <= patterns


def test_desktop_contact_pictures_start_with_the_forkmesh_user_and_end_with_gravatar():
    pictures = _feature_body("pictureSources")
    assert "QStringList sources = avatarSources(contact);" in pictures
    assert 'sources.prepend(QStringLiteral("forkmesh:") + email);' in pictures
    assert 'sources.append(QStringLiteral("gravatar:") + email);' in pictures
    assert "row.avatarSources = pictureSources(contact);" in FEATURE
    card = _feature_body("showContactDetail")
    assert "const QStringList sources = avatarSources(contact);" in card
    assert "reads->photoSources = pictureSources(contact);" in card
    assert "for (const QString &source : std::as_const(reads->photoSources))" in card
    directory = _feature_body("readForkMeshAvatars")
    assert "m_userRecordsAvailable()" in directory
    assert "kForkMeshAvatarsMs" in directory
    assert "self->keepForkMeshAvatars(users);" in directory
    assert "self->queueAvatars();" in directory
    assert "readForkMeshAvatars();" in _feature_body("loadOrganization")
    assert "keepForkMeshAvatars(users);" in _feature_body("finishForkMeshUsers")
    assert 'text(user, "avatarPng")' in _feature_body("keepForkMeshAvatars")
    assert "m_forkMeshAvatarsReading" in _feature_body("queueAvatars")
    fetch = _feature_body("fetchAvatar")
    assert fetch.index('QLatin1String("forkmesh")') < fetch.index("avatarCachePath(source)")
    assert "m_network" not in fetch.split('QLatin1String("forkmesh")', 1)[1].split("return;", 1)[0]
    assert "gravatarPicture(account, kAvatarKeep)" in fetch
    assert "saved.size() == 0" in fetch
    assert "forkmesh::networkReplyStatusIsExpected(reply)" in _feature_body("fetchAvatarImage")
    gravatar = FEATURE.split("QUrl gravatarPicture(", 1)[1].split("\n}\n", 1)[0]
    assert "QCryptographicHash::Sha256" in gravatar
    assert "https://gravatar.com/avatar/%1?s=%2&d=404" in gravatar
    catalog = json.loads((REPO / "desktop" / "resources" / "network" /
                          "endpoint-catalog.json").read_text(encoding="utf-8"))
    patterns = {profile["pattern"] for profile in catalog["endpoints"]}
    assert "gravatar.com/avatar/*" in patterns


def test_desktop_contact_names_open_a_detail_card_with_a_bigger_picture():
    # A click on the picture or the name opens everything the organization
    # knows about the person, the way a click on the address opens a compose
    # window; the name is still edited in place with a double click or F2.
    clicks = FEATURE.split("&QTableWidget::itemClicked", 1)[1].split(
        "&QTableWidget::itemDoubleClicked", 1)[0]
    assert 'spec.key == QLatin1String("name")' in clicks
    assert "showContactDetail(id->data(Qt::UserRole).toString())" in clicks
    assert clicks.index('QLatin1String("name")') < clicks.index('QLatin1String("email")')
    fill = FEATURE.split("void CrmFeature::fillContactRows()", 1)[1].split("\n}\n", 1)[0]
    assert "Click the picture or the name to open everything on this" in fill
    assert "Press F2 to edit the name here" in fill
    # The card's circle is four times the one on a row, and the initials sit
    # under it, so the card reads before a picture lands and when none does.
    assert "constexpr int kAvatarSize = 24;" in FEATURE
    assert "constexpr int kDetailPhotoSize = 96;" in FEATURE
    card = FEATURE.split("void CrmFeature::showContactDetail(", 1)[1].split("\n}\n", 1)[0]
    assert "initialsPixmap(contactInitials(contact), contactAvatarTone(contact)" in card
    assert "circlePixmap(cached, kDetailPhotoSize, devicePixelRatioF())" in card
    # Every field the table can show, then the custom keys this record fills
    # in, then who added it and who last touched it.
    assert "for (const ContactColumn &column : contactColumns())" in card
    assert "for (const QString &key : std::as_const(customKeys))" in card
    assert 'contact.value(QStringLiteral("fields")).toObject().keys()' in card
    assert 'text(contact, "createdBy")' in card and 'text(contact, "updatedBy")' in card
    # The messages and toots linked to the record, newest first.
    assert "m_contactEmails.value(contactId)" in card
    assert "linked.append(m_contactToots.value(contactId))" in card
    assert "linkedEmailLine(email)" in card
    # The card is the third column beside the table, not a window: it
    # replaces the contact it showed and hands off to Email and Edit directly.
    assert "m_detailPanel->show();" in card
    assert "m_detailBody->deleteLater();" in card
    assert "composeEmail(contactById(contactId))" in card
    assert "editContact(contactById(contactId))" in card
    assert "QDialog" not in card


def test_desktop_and_web_read_the_same_accounts_for_a_cards_recent_updates():
    # The same three services, in the same order, on both surfaces: the order
    # is also the order a picture is taken from.
    assert 'const CRM_LIVE_SERVICES = ["mastodon", "bluesky", "github"];' in WEB_SCRIPT
    sources = FEATURE.split("QStringList CrmFeature::avatarSources(", 1)[1].split("\n}\n", 1)[0]
    assert sources.index('"mastodon:"') < sources.index('"bluesky:"') < sources.index('"github:"')
    card = FEATURE.split("void CrmFeature::showContactDetail(", 1)[1].split("\n}\n", 1)[0]
    assert "const QStringList sources = avatarSources(contact);" in card
    # Every read is bound to an object the panel deletes with the card it
    # replaces, so nothing lands in a card that has gone.
    assert "readAccountUpdates(source, reads, show)" in card
    assert "auto *reads = new DetailReads(panel);" in card
    # Opening a card reads what this session has not read yet; Read again
    # reads afresh.
    assert "reads->readAccounts(reread);" in card
    assert "reads->readAccounts(true);" in card
    assert "m_accountReads.insert(source, AccountRead{error, picture, updates});" in card
    assert "Read again" in FEATURE
    # One public, unauthenticated read per service, the same endpoints the
    # page reads in the browser.
    for surface, calls in (
        (FEATURE, ('"/api/v1/accounts/lookup"', '"/statuses"',
                   '"app.bsky.feed.getAuthorFeed"', '"/users/%1/events/public"')),
        (WEB_SCRIPT, ("/api/v1/accounts/lookup?acct=", "/statuses",
                      "app.bsky.feed.getAuthorFeed", "/events/public")),
    ):
        for call in calls:
            assert call in surface, call
    for key in ("exclude_replies", "exclude_reblogs", "posts_no_replies"):
        assert key in FEATURE and key in WEB_SCRIPT, key
    # A card closed mid-read drops what comes back, and nothing a read
    # brought lands on the record or on disk.
    for reader in ("readMastodonUpdates", "readBlueskyUpdates", "readGithubUpdates",
                   "readContactPhoto"):
        body = FEATURE.split(f"void CrmFeature::{reader}(", 1)[1].split("\n}\n", 1)[0]
        assert "QPointer<QObject> guard(context)" in body, reader
        # A card that has gone hears nothing back. The picture read still
        # files what it fetched, so reopening the card shows it at once.
        assert ("if (!guard)" in body if reader != "readContactPhoto"
                else "if (guard)" in body), reader
        assert "QSettings" not in body and "QSaveFile" not in body, reader
    # The reads the download's endpoint catalog has to describe.
    catalog = json.loads((REPO / "desktop" / "resources" / "network" /
                          "endpoint-catalog.json").read_text(encoding="utf-8"))
    patterns = {profile["pattern"] for profile in catalog["endpoints"]}
    assert {"public.api.bsky.app/xrpc/app.bsky.feed.getAuthorFeed",
            "api.github.com/users/*/events/public"} <= patterns
    described = [profile for profile in catalog["endpoints"]
                 if any("readGithubUpdates" in source or "readBlueskyUpdates" in source
                        for source in profile["source"])]
    assert len(described) == 3
    for profile in described:
        assert "detail card" in profile["trigger"] or "detail card" in profile["description"]


def test_desktop_contact_rows_copy_an_outreach_prompt_for_their_sheet():
    fill = FEATURE.split("void CrmFeature::fillContactRows()", 1)[1].split("\n}\n", 1)[0]
    assert 'QStringLiteral("Copy prompt")' in fill
    assert "copyOutreachPrompt(contactById(contactId))" in fill
    assert 'setObjectName(QStringLiteral("crmRowActions"))' in fill
    assert "QWidget#crmRowActions QPushButton" in FEATURE
    prompt = FEATURE.split("QString CrmFeature::outreachPrompt(", 1)[1].split("\n}\n", 1)[0]
    assert "Tell me about ForkMesh" in prompt
    assert "toot" in prompt
    # The sheet the row is open in picks the angle: a journalist gets a story,
    # an investor the opportunity.
    assert 'text(activeSheet(), "name")' in prompt
    assert '"journalist"' in prompt and '"investor"' in prompt
    assert "clipboard->setText(outreachPrompt(contact))" in FEATURE
    menu = FEATURE.split("void CrmFeature::showContactMenu(", 1)[1].split("\n}\n", 1)[0]
    assert "copyOutreachPrompt(contactById(id))" in menu


def test_desktop_contact_table_is_sortable_hideable_reorderable_and_extensible():
    setup = FEATURE.split("m_contactsTable = new QTableWidget(0, 0);", 1)[1]
    setup = setup.split("contactsPageLayout->addWidget(m_contactsBody)", 1)[0]
    # Drag to reorder, click to sort, right-click for the column menu.
    for call in ("setSectionsMovable(true)", "setSectionsClickable(true)",
                 "setSortIndicatorShown(true)",
                 "setContextMenuPolicy(Qt::CustomContextMenu)"):
        assert call in setup, call
    # The page keeps no search box of its own: the window's top search box is
    # the filter, and lists CRM contacts from anywhere in the app.
    assert "m_search = lineEdit()" not in FEATURE
    assert "void CrmFeature::setHostSearchQuery(const QString &query)" in FEATURE
    assert "CrmFeature::searchHits(const QString &query, int limit) const" in FEATURE
    assert "void CrmFeature::activateSearchHit(const QString &id)" in FEATURE
    assert "virtual void setHostSearchQuery(const QString &) {}" in (
        REPO / "desktop" / "src" / "NativePackageExtension.h").read_text(
        encoding="utf-8")
    # A single click into a cell opens its editor; the email cell is a link
    # instead and composes in the organization's mail provider (Zoho unless
    # settings pick Gmail), so its text is edited with a double click or F2.
    assert "QTableWidget::itemClicked" in FEATURE
    assert "m_contactsTable->editItem(item)" in FEATURE
    assert "font.setUnderline(true)" in FEATURE
    assert 'QStringLiteral("mail.zohocloud.ca")' in FEATURE
    assert 'QStringLiteral("https://mail.google.com/mail/")' in FEATURE
    assert '{QStringLiteral("mailProvider"), mailProvider->currentData().toString()}' in FEATURE
    # Right-click menu: hide the clicked column, toggle any, restore, add.
    menu = FEATURE.split("void CrmFeature::showColumnMenu(", 1)[1]
    menu = menu.split("\nvoid CrmFeature::", 1)[0]
    assert "setCheckable(true)" in menu
    assert "Show all columns" in menu
    assert "Add column" in menu
    # The "+" header adds a custom column stored inside the contact's fields.
    assert 'QStringLiteral("+")' in FEATURE
    assert 'QJsonObject{{QStringLiteral("fields"), QJsonObject{{column.key, value}}}}' in FEATURE
    # The Mastodon column asks the contact's own instance, in memory only.
    assert "/api/v1/accounts/lookup" in FEATURE
    # An unknown handle's 404 is the lookup's answer, not a logged failure.
    assert "forkmesh::markExpectedHttpStatus(lookupRequest, 404)" in FEATURE
    assert "QNetworkRequest lookupRequest = instanceRequest(lookup);" in FEATURE
    assert "m_network->get(instanceRequest(statuses))" in FEATURE
    assert "forkmesh::markExpectedTransportFailure(request);" in FEATURE
    assert "/statuses" in FEATURE
    assert "exclude_reblogs" in FEATURE
    # Tabs: one per list and pipeline (no every-contact tab), one activity
    # log, an import tab.
    assert 'QStringLiteral("Relationships' not in FEATURE
    assert 'QStringLiteral("All activity")' in FEATURE
    assert 'QStringLiteral("Import CSV list")' in FEATURE


def test_a_partial_csv_import_never_blanks_stored_contact_fields():
    merge = CRM_WORKER.split("async def _import_contacts(", 1)[1]
    assert "supplied = {key: value for key, value in incoming.items()" in merge
    assert "_contact_data(dict(supplied, views=[view_id])," in merge
    assert "existing if isinstance(existing, dict) else {})" in merge


def test_the_dashboard_wires_sheet_tabs_and_a_right_click_column_menu():
    # The tab strip, the per-sheet heading row, the floating menu, and the
    # rename/share dialog all need a home in the markup the page renders into.
    for hook in ("data-crm-tabs", "data-crm-contact-head", "data-crm-menu",
                 "data-crm-sheet-dialog", "data-crm-sheet-name",
                 "data-crm-sheet-visibility", "data-crm-sheet-members",
                 "data-crm-sheet-new", "data-crm-sheet-delete"):
        assert hook in WEB_VIEW, hook

    # Right-click reaches both surfaces: a column heading and a sheet tab.
    assert 'head?.addEventListener("contextmenu"' in WEB_SCRIPT
    assert 'tabs?.addEventListener("contextmenu"' in WEB_SCRIPT
    for action in ("Move left", "Move right", "Remove from this view",
                   "Rename or share…", "Delete list…"):
        assert action in WEB_SCRIPT, action
    # A column is also moved by dragging its heading.
    assert 'draggable="true"' in WEB_SCRIPT
    for event in ("dragstart", "dragover", "drop", "dragend"):
        assert f'head?.addEventListener("{event}"' in WEB_SCRIPT, event

    assert 'crmOrganizationPath("/settings"), "PUT", { tableViews: views }' in WEB_SCRIPT
    assert "localStorage.setItem(crmTableViewKey(), id)" in WEB_SCRIPT
    assert "data-crm-table-view" in WEB_VIEW

    # An import names its tab after the file and lands the user on it.
    assert 'fileName: file.name || "contacts.csv"' in WEB_SCRIPT
    assert "if (payload.viewId) state.crmView.activeView = payload.viewId;" in WEB_SCRIPT
    # Columns the CSV brought that the CRM has no built-in field for stay
    # editable through the same inline-edit path as the built-in ones.
    assert "{ fields: { [field]: input.value } }" in WEB_SCRIPT


def test_a_sheet_tab_can_be_renamed_moved_and_shows_who_it_is_shared_with():
    # Renaming: from the tab menu, and by double-clicking the tab itself.
    assert "Rename…" in WEB_SCRIPT
    assert 'tabs?.addEventListener("dblclick"' in WEB_SCRIPT
    assert 'crmOrganizationPath(`/views/${view.id}`), "PATCH", { name }' in WEB_SCRIPT

    # Moving: from the tab menu, and by dragging one tab onto another. The order
    # belongs to the organization, so it is saved against the worker.
    assert "data-crm-tab-drag" in WEB_SCRIPT
    for event in ("dragstart", "dragover", "drop", "dragend"):
        assert f'tabs?.addEventListener("{event}"' in WEB_SCRIPT, event
    assert 'crmOrganizationPath("/views/order"), "POST", { order }' in WEB_SCRIPT

    # Sharing: a count on the tab, and the list of who they are on hover.
    assert "data-crm-share-card" in WEB_VIEW
    assert "data-crm-share=" in WEB_SCRIPT
    assert 'tabs?.addEventListener("mouseover"' in WEB_SCRIPT
    assert "function crmSheetShareNames(" in WEB_SCRIPT
    assert "function showCrmShareCard(" in WEB_SCRIPT


def test_a_field_hidden_on_a_tab_stays_hidden_and_can_always_be_brought_back():
    # A layout that hides everything leaves no heading to right-click, so the
    # field list also hangs off the tab menu and the table falls back to the
    # built-in columns rather than rendering nothing at all.
    assert "function crmFieldMenuItems(" in WEB_SCRIPT
    assert '{ heading: "Fields" }' in WEB_SCRIPT
    assert "visible.length ? visible : columns.filter((column) => !column.custom)" in WEB_SCRIPT
    # A sheet the worker opens shows its columns; only the ones a CSV did not
    # carry start hidden.
    assert '"custom": False, "hidden": bool(leading)' in CRM_WORKER
    # The layout is saved on the list and re-read only when asked, so nothing
    # can put the hidden field back behind the member's back.
    assert "saving" not in WEB_SCRIPT


def test_desktop_lists_mirror_the_web_tabs_pipelines_and_their_sharing():
    # A tab shows one list, a pipeline list holding every contact in its flow,
    # like the web page's tabs.
    assert "m_sheet = new QComboBox;" in FEATURE
    assert "bool CrmFeature::contactInSheet(" in FEATURE
    membership = FEATURE.split("bool CrmFeature::contactInSheet(", 1)[1].split("\n}\n", 1)[0]
    assert 'if (text(contact, "flow") != flow)' in membership
    assert "pipelineLists.value(flow, QStringList{id})" in membership
    assert "if (contactInSheet(contact, sheet, lists))" in FEATURE
    # Renaming and moving a list write the same endpoints the web tabs use.
    rename = FEATURE.split("void CrmFeature::renameSheet(", 1)[1].split("\nvoid CrmFeature::", 1)[0]
    assert 'QStringLiteral("/views/") + id' in rename
    assert 'QJsonObject{{QStringLiteral("name"), name}}' in rename
    move = FEATURE.split("void CrmFeature::moveSheet(", 1)[1].split("\nvoid CrmFeature::", 1)[0]
    assert 'organizationPath(QStringLiteral("/views/order"))' in move
    assert 'QJsonArray::fromStringList(order)' in move
    # Who a list reaches: its picked names plus the roster while it is open to
    # the organization, never counting its owner.
    assert 'payload.value(QStringLiteral("members")).toArray()' in FEATURE
    share = FEATURE.split("QStringList CrmFeature::sheetShareNames(", 1)[1].split("\n}\n", 1)[0]
    assert "m_members" in share and '"owner"' in share
    # A pipeline is a flow in the shared settings plus a list bound to it.
    pipeline = FEATURE.split("void CrmFeature::newPipeline()", 1)[1].split("\nvoid CrmFeature::", 1)[0]
    assert pipeline.index('organizationPath(QStringLiteral("/settings"))') < pipeline.index(
        'organizationPath(QStringLiteral("/views"))')
    assert '{QStringLiteral("flow"), flowId}' in pipeline
    assert "m_canManageSettings" in pipeline
    # The mailboxes open on the organization's Zoho data center.
    assert "void CrmFeature::openMailbox(bool gmail)" in FEATURE
    compose = FEATURE.split("QUrl CrmFeature::mailComposeUrl(", 1)[1].split("\n}\n", 1)[0]
    assert "zohoMailHost()" in compose
    assert 'QStringLiteral("/zm/comp.do")' in compose
    assert "compose.do" not in compose


def test_desktop_table_views_are_default_all_and_custom_ones_shared_in_settings():
    assert 'QStringLiteral("name"), QStringLiteral("email"), QStringLiteral("mastodon"),' in FEATURE
    assert 'QStringLiteral("bluesky"), QStringLiteral("twitter"), QStringLiteral("linkedin"),' in FEATURE
    assert 'QString m_tableViewId = QStringLiteral("default");' in FEATURE_HEADER
    hidden = FEATURE.split("bool CrmFeature::columnHiddenInView(", 1)[1].split("\n}\n", 1)[0]
    assert "allColumnsView()" in hidden and "!sheetCarriesField(column.key)" in hidden
    assert "!tableViewFields().contains(viewField(column))" in hidden
    save = FEATURE.split("void CrmFeature::saveTableViews(", 1)[1].split("\nvoid CrmFeature::", 1)[0]
    assert 'organizationPath(QStringLiteral("/settings"))' in save
    assert 'QJsonObject{{QStringLiteral("tableViews"), views}}' in save
    change = FEATURE.split("void CrmFeature::changeViewFields(", 1)[1].split("\nvoid CrmFeature::", 1)[0]
    assert "createTableView" not in change and "m_tableViewDraft = draft;" in change
    assert "fields == builtinViewFields()" in change
    rendered = FEATURE.split("void CrmFeature::renderTableViews(", 1)[1].split("\nvoid CrmFeature::", 1)[0]
    assert '"%1 (modified)"' in rendered and "m_tableViewSave->setVisible(modified);" in rendered
    draft = FEATURE.split("void CrmFeature::saveTableViewDraft(", 1)[1].split("\nvoid CrmFeature::", 1)[0]
    assert "QInputDialog" not in draft and 'QStringLiteral("View %1")' in draft
    assert "addTableView(name, fields," in draft
    assert '[this] { saveTableViewDraft(); }' in FEATURE
    select = FEATURE.split("void CrmFeature::selectTableView(", 1)[1].split("\nvoid CrmFeature::", 1)[0]
    assert "m_tableViewDraft.clear();" in select
    sheet = FEATURE.split("void CrmFeature::addColumnToSheet(", 1)[1].split("\nvoid CrmFeature::", 1)[0]
    assert 'QJsonObject{{QStringLiteral("columns"), layout}}' in sheet
    assert "m_saving" not in FEATURE
    menu = FEATURE.split("void CrmFeature::showColumnMenu(", 1)[1].split("\nvoid CrmFeature::", 1)[0]
    assert "shown > 1" in menu and "Save as new view" in menu


def test_desktop_csv_import_confirms_the_column_mapping_before_writing():
    import_body = FEATURE.split("void CrmFeature::importCsv()", 1)[1].split("\nbool CrmFeature::", 1)[0]
    preview = import_body.index('{QStringLiteral("preview"), true}')
    mapped = import_body.index("mapImportColumns(")
    upload = import_body.index("sendImportPart();")
    assert preview < mapped < upload
    mapping = FEATURE.split("bool CrmFeature::mapImportColumns(", 1)[1].split("\nQJsonObject CrmFeature::", 1)[0]
    # Every target the worker accepts: its own column, nothing, a contact
    # column, or one half of a split name.
    for target in ('QStringLiteral("custom")', 'QStringLiteral("first_name")',
                   'QStringLiteral("last_name")', 'QStringLiteral("lastContactedAt")'):
        assert target in mapping, target
    assert "Import a column as Name, Email or Company" in mapping
    assert '{QStringLiteral("mapping"), m_import.mapping}' in FEATURE
    # A finished import opens the list it filled, like the web lands on its tab.
    part = FEATURE.split("void CrmFeature::sendImportPart()", 1)[1].split("\nvoid CrmFeature::", 1)[0]
    landing = part.index("m_pendingSheet = m_import.viewId;")
    assert part.index("finishImport();") < landing < part.index("loadOrganization();")


def test_desktop_contacts_multi_select_and_move_to_a_pipeline_from_a_right_click():
    setup = FEATURE.split("m_contactsTable = new QTableWidget(0, 0);", 1)[1]
    setup = setup.split("contactsPageLayout->addWidget(m_contactsBody)", 1)[0]
    # Ctrl/Shift+click extends the selection; a modified click never opens
    # the cell editor or the email link.
    assert "setSelectionMode(QAbstractItemView::ExtendedSelection)" in setup
    assert "QAbstractItemView::SelectedClicked" not in setup
    clicked = FEATURE.split("QTableWidget::itemClicked", 1)[1].split("QTableWidget::itemDoubleClicked", 1)[0]
    assert clicked.index("Qt::ControlModifier") < clicked.index("openMailCompose")
    assert "QTableWidget::customContextMenuRequested, this,\n            [this](const QPoint &position) { showContactMenu(position); }" in FEATURE
    # The periodic re-render keeps every selected row, not just the current one.
    render = FEATURE.split("void CrmFeature::renderContacts()", 1)[1].split("\nvoid CrmFeature::", 1)[0]
    assert "selectedContacts()" in render
    fill = FEATURE.split("void CrmFeature::fillContactRows()", 1)[1].split("\nbool CrmFeature::", 1)[0]
    assert "QItemSelectionModel::Select | QItemSelectionModel::Rows" in fill
    menu = FEATURE.split("void CrmFeature::showContactMenu(", 1)[1].split("\nvoid CrmFeature::", 1)[0]
    for label in ("Move to pipeline", "Move to list", "Add to list",
                  "Set stage in %1", "Remove from ", "Manage pipelines"):
        assert label in menu, label
    # A pipeline holds its flow's contacts, so a move sets the flow (and a
    # stage that flow has); a plain list is added through `views`, which the
    # worker merges.
    move = FEATURE.split("void CrmFeature::moveContactsToSheet(", 1)[1].split("\nvoid CrmFeature::", 1)[0]
    assert 'body.insert(QStringLiteral("flow"), flowId)' in move
    assert "stages.first()" in move
    assert 'body.insert(QStringLiteral("views"), QJsonArray{sheetId})' in move
    assert "result[\"views\"] = _view_ids(data.get(\"views\"), result.get(\"views\"))" in CRM_WORKER
    # "Move to list" replaces the memberships and "Remove from" drops one, both
    # through the verbs the worker added for them, because `views` only adds.
    only = FEATURE.split("void CrmFeature::moveContactsToListOnly(", 1)[1]
    only = only.split("\nvoid CrmFeature::", 1)[0]
    assert 'QStringLiteral("setViews")' in only and "QJsonArray{sheetId}" in only
    assert 'text(sheet, "flow").isEmpty()' in only
    drop = FEATURE.split("void CrmFeature::removeContactsFromSheet(", 1)[1]
    drop = drop.split("\nvoid CrmFeature::", 1)[0]
    assert 'QStringLiteral("removeViews")' in drop and "QJsonArray{sheetId}" in drop
    for verb in ('if "setViews" in data:', 'if "removeViews" in data:'):
        assert verb in CRM_WORKER, verb
    # Every bulk edit routes through one batch helper, which changes the rows
    # locally, puts the progress message up, and only then sends the whole
    # selection as one request, standing the shared refresh down until it
    # answers.
    for caller in ("moveContactsToSheet", "moveContactsToListOnly",
                   "removeContactsFromSheet", "setContactsStage"):
        body = FEATURE.split(f"void CrmFeature::{caller}(", 1)[1]
        body = body.split("\nvoid CrmFeature::", 1)[0]
        assert "patchContacts(" in body, caller
        assert "request(" not in body, caller
    batch = FEATURE.split("void CrmFeature::patchContacts(", 1)[1]
    batch = batch.split("\nvoid CrmFeature::", 1)[0]
    assert batch.index("setStatus(") < batch.index("request(")
    assert batch.index("withContactEdit(") < batch.index("request(")
    assert batch.count("request(") == 1 and "/contacts/bulk" in batch
    assert 'if item == "bulk":' in CRM_WORKER


def test_a_list_carries_a_pipeline_type_on_both_surfaces():
    # Which pipeline a list reads off is set from the list's own menu, and the
    # worker keeps it on the sheet, so the two surfaces agree on the field.
    assert 'result["flow"] = _slug(data.get("flow"), "")' in CRM_WORKER
    tab = FEATURE.split("void CrmFeature::showSheetTabMenu(", 1)[1]
    tab = tab.split("\nQJsonObject CrmFeature::", 1)[0]
    assert 'QStringLiteral("Pipeline type")' in tab
    assert "No pipeline (plain list)" in tab
    assert "setSheetFlow(id, QString())" in tab
    # Only owners and admins change it, the way _update_view gates it.
    assert "type->setEnabled(m_canManageSettings);" in tab
    setter = FEATURE.split("void CrmFeature::setSheetFlow(", 1)[1]
    setter = setter.split("\nvoid CrmFeature::", 1)[0]
    assert 'QStringLiteral("flow"), flowId' in setter
    assert "/views/" in setter
    sheet_menu = WEB_SCRIPT.split("function openCrmSheetMenu(", 1)[1]
    sheet_menu = sheet_menu.split("\n  function ", 1)[0]
    assert "Pipeline type" in sheet_menu
    assert "No pipeline (plain list)" in sheet_menu
    assert "crmSetSheetFlow(view, flow.id)" in sheet_menu
    assert 'crmSetSheetFlow(view, "")' in sheet_menu


def test_the_web_contact_row_offers_the_same_right_click_moves():
    menu = WEB_SCRIPT.split("function openCrmContactMenu(", 1)[1]
    menu = menu.split("\n  }\n", 1)[0]
    for label in ("Move to pipeline", "Move to list", "Add to list",
                  "Remove from ", "Manage pipelines"):
        assert label in menu, label
    # The same three membership verbs the desktop uses.
    assert "setViews: [view.id]" in menu
    assert "views: [view.id]" in menu
    assert "removeCrmContactsFromSheet(ids, open.id)" in menu
    assert "removeViews: [sheetId]" in WEB_SCRIPT
    # A shared sheet is read-only, so it gets no move menu at all.
    assert "crmActiveShared()" in menu
    assert 'addEventListener("contextmenu", handleContactMenu)' in WEB_SCRIPT


def test_desktop_header_opens_the_mailboxes_and_the_browser_extension():
    header = FEATURE.split('title->setObjectName(QStringLiteral("pageTitle"));', 1)[1]
    header = header.split("m_status = new QLabel", 1)[0]
    for label in ("Zoho Mail", "Gmail", "Extension"):
        assert f'QStringLiteral("{label}")' in header, label
    assert "button->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);" in header
    # The extension is its own Market package: the button opens that
    # package's page and this one pins no extension build.
    assert "kExtensionVersion" not in FEATURE
    assert 'kExtensionPage[] = "https://app.forkmesh.com/dashboard/extension"' in FEATURE
    opener = FEATURE.split("void CrmFeature::openBrowserExtension()", 1)[1].split("\n}\n", 1)[0]
    assert "QDesktopServices::openUrl(QUrl(QLatin1String(kExtensionPage)))" in opener
    assert "browser-extension folder" in opener


def test_desktop_lists_open_as_tabs_with_a_share_count_and_imports_name_them():
    tabs = FEATURE.split("void CrmFeature::renderSheetTabs(", 1)[1].split("\nvoid CrmFeature::", 1)[0]
    assert "m_tabs->insertTab(position, page, QString())" in tabs
    assert "renderSheetList();" in tabs
    side = FEATURE.split("void CrmFeature::renderSheetList()", 1)[1].split("\nvoid CrmFeature::", 1)[0]
    assert "m_sheetList->setItemWidget(item, row)" in side
    assert "badge->setText(QString::number(names.size()))" in side
    assert "sheetShareNames(sheet)" in side
    # The one table moves into the open list's tab and filters to that list.
    sync = FEATURE.split("void CrmFeature::syncSheetTab()", 1)[1].split("\nvoid CrmFeature::", 1)[0]
    assert "page->layout()->addWidget(m_contactsBody)" in sync
    assert "m_sheet->setCurrentIndex(sheetIndex)" in sync
    # The import step asks for the tab's name and every part carries it; the
    # worker only uses it when the import opens a new tab.
    mapping = FEATURE.split("bool CrmFeature::mapImportColumns(", 1)[1].split("\nQJsonObject CrmFeature::", 1)[0]
    assert 'QStringLiteral("Tab name")' in mapping
    assert "importButton->setEnabled(identified && named)" in mapping
    assert '{QStringLiteral("sheetName"), m_import.sheetName}' in FEATURE
    assert '_clean(data.get("sheetName"), 60) or _sheet_name(file_name)' in CRM_WORKER
    assert "data-crm-import-map-name" in WEB_VIEW
    assert "sheetName, viewId, mapping" in WEB_SCRIPT



def test_desktop_imports_github_contributors_without_masked_emails():
    for declaration in (
            "static QString githubRepositoryPath(const QString &value);",
            "static bool maskedGitEmail(const QString &email);",
            "void importGithubContributors();",
            "GithubContributorsJob m_contributors;"):
        assert declaration in FEATURE_HEADER
    assert FEATURE.count("[this] { importGithubContributors(); }") == 3
    assert 'toolButton(QStringLiteral("repo"), QStringLiteral("Import GitHub")' in FEATURE
    assert "connect(m_githubButton, &QToolButton::clicked" in FEATURE
    assert "{m_importButton, m_githubButton," in FEATURE
    start = FEATURE.split("void CrmFeature::importGithubContributors()", 1)[1].split("\n}\n", 1)[0]
    for argument in ('"credential.helper="', '"--bare"', '"--filter=tree:0"',
                     '"--no-tags"', '"log.mailmap=false"', '"--all"', '"-z"',
                     "trailers:key=Co-authored-by", "std::make_shared<QTemporaryDir>"):
        assert argument in start, argument
    assert 'QStringLiteral("https://github.com/%1.git").arg(repository)' in start
    run = FEATURE.split("void CrmFeature::runContributorsGit(", 1)[1].split("\n}\n", 1)[0]
    assert '"GIT_TERMINAL_PROMPT"), QStringLiteral("0")' in run
    assert "&QProcess::kill" in run
    masked = FEATURE.split("bool CrmFeature::maskedGitEmail(", 1)[1].split("\n}\n", 1)[0]
    pattern = re.search(r'QStringLiteral\("(.+?)"\)', masked).group(1).replace("\\\\", "\\")
    for email in ("1+ada@users.noreply.github.com", "noreply@github.com",
                  "ada@users.noreply.gitlab.com", "no-reply@example.com",
                  "donotreply@example.com", "node@users.noreply.forkmesh.com"):
        assert re.search(pattern, email, re.I), email
    for email in ("ada@example.com", "replyguy@example.com", "noreplyfan@example.com"):
        assert not re.search(pattern, email, re.I), email
    finish = FEATURE.split("void CrmFeature::finishGithubContributors(", 1)[1].split("\n}\n", 1)[0]
    assert "githubContributorsCsv(log, kGithubContributorsMax, &found)" in finish
    assert "startCsvImport(job.path," in finish
    assert '"Name,Email,Commits,Last commit\\r\\n"' in FEATURE


def test_desktop_imports_forkmesh_users_into_one_list_it_refreshes():
    for declaration in (
            "static QString forkMeshUsersCsv(const QJsonArray &users, int limit,",
            "static QString forkMeshUsersSheetId(const QJsonArray &views);",
            "void importForkMeshUsers();",
            "bool importBusy() const;"):
        assert declaration in FEATURE_HEADER
    assert "[this] { return m_isAdmin; }" in HOST
    assert "fetchNativePackageUserRecords(std::move(done));" in HOST
    assert FEATURE.count("[this] { importForkMeshUsers(); }") == 4
    assert 'toolButton(QStringLiteral("person"), QStringLiteral("Import users")' in FEATURE
    tab_menu = FEATURE.split("void CrmFeature::showSheetTabMenu(", 1)[1].split("\nvoid CrmFeature::", 1)[0]
    assert 'QStringLiteral("Refresh ForkMesh users")' in tab_menu
    assert "id == forkMeshUsersSheetId(m_views)" in tab_menu
    finish = FEATURE.split("void CrmFeature::finishForkMeshUsers(", 1)[1].split("\n}\n", 1)[0]
    assert "forkMeshUsersSheetId(m_views)" in finish
    assert ('QJsonArray{QStringLiteral("name"), QStringLiteral("email"),\n'
            '                              QStringLiteral("custom"), QStringLiteral("custom")}') in finish
    assert '"Name,Email,Signed up,Email notifications\\r\\n"' in FEATURE
    start = FEATURE.split("void CrmFeature::startCsvImport(", 1)[1].split("\n}\n", 1)[0]
    assert start.index("if (!mapping.isEmpty())") < start.index('{QStringLiteral("preview"), true}')
    for guarded in ("importCsv()", "importMastodonFollowers()",
                    "importGithubContributors()", "importForkMeshUsers()"):
        body = FEATURE.split("void CrmFeature::" + guarded, 1)[1].split("\n}\n", 1)[0]
        assert "if (importBusy())" in body, guarded
    worker_keys = ("CONTACT_ID_RE.fullmatch(view_id)", "raw.get(\"importId\")")
    assert all(key in CRM_WORKER for key in worker_keys)

def test_web_contact_rows_read_like_a_table():
    render = WEB_SCRIPT.split("function renderCrmContacts()", 1)[1].split("\n  function ", 1)[0]
    # Read-mode rows (pull #228): every cell is a quiet crm-cell control, the
    # name cell carries the avatar and the record's mail chips underneath, and
    # the row actions only show on hover.
    assert '<td class="px-3 py-2 align-top">' in render
    assert "crmAvatar(contact)" in render and "crmNameMeta(contact, columns)" in render
    assert '<tr data-crm-contact-row="${escapeHtml(contact.id)}" class="crm-row">' in render
    assert "crm-row-actions" in render
    # Notes still edit in a one-line field when a caller asks for the compact
    # form; the mobile cards keep the textarea.
    field = WEB_SCRIPT.split("function crmInlineContactField(", 1)[1].split("\n  }\n", 1)[0]
    assert 'column.field === "notes" && !line' in field
    assert 'class="crm-cell"' in field


def test_marketplace_page_utilities_are_compiled_into_the_dashboard_stylesheet():
    # Package views mount into dashboard pages and use that page's compiled
    # stylesheet. When the build did not scan marketplace packages, the
    # phone-only contact cards (sm:hidden) stayed visible on desktop and the
    # stat grid never reached four columns.
    config = (REPO / "app" / "tailwind.dashboard.config.js").read_text(
        encoding="utf-8")
    assert '"../marketplace/packages/*/content/web/**/*.{html,js}"' in config
    assert 'data-crm-menu hidden role="menu" style="min-width:12rem"' in WEB_VIEW
    assert 'style="cursor:grab"' in WEB_SCRIPT
    tailwind = (REPO / "app" / "public" / "dashboard" / "tailwind.css").read_text(
        encoding="utf-8")
    for utility in ("fixed", "z-50", "max-h-80", "overflow-y-auto", "shrink-0",
                    "items-end", "overflow-x-auto", "border-b-2", r"sm\:hidden",
                    "border-destructive"):
        assert f".{utility}" in tailwind, utility
    # Dashboard focus rings are blue, never the green primary.
    assert "focus:ring-primary" not in WEB_VIEW + WEB_SCRIPT


def test_desktop_contacts_live_in_pipeline_tabs_and_a_tab_deletes_its_pipeline():
    # No every-contact tab: each pipeline is a tab, a client-side one when no
    # list is bound to it, and "No pipeline" catches contacts whose flow went.
    assert 'QStringLiteral("All contacts' not in FEATURE
    virtual = FEATURE.split("void CrmFeature::rebuildVirtualSheets()", 1)[1].split("\n}\n", 1)[0]
    assert "kPipelineTabPrefix" in virtual and "kNoPipelineTabId" in virtual
    search = FEATURE.split("void CrmFeature::activateSearchHit(", 1)[1].split("\n}\n", 1)[0]
    assert "m_tabs->setCurrentIndex(0)" not in search
    sheet = FEATURE.split("void CrmFeature::addColumnToSheet(", 1)[1].split("\nvoid CrmFeature::", 1)[0]
    assert "!storedSheet(sheet)" in sheet
    # Right-click on a tab deletes its pipeline: the flow leaves the shared
    # settings and the lists bound to it go, but never the last flow, which
    # the worker would answer with its defaults.
    tab = FEATURE.split("void CrmFeature::showSheetTabMenu(", 1)[1].split("\nQJsonObject CrmFeature::", 1)[0]
    assert "Delete pipeline" in tab and "deletePipeline(id)" in tab
    assert "remove->setEnabled(m_canManageSettings && !lastPipeline);" in tab
    remove = FEATURE.split("void CrmFeature::deletePipeline(", 1)[1].split("\nvoid CrmFeature::", 1)[0]
    assert 'organizationPath(QStringLiteral("/settings"))' in remove
    assert 'QByteArrayLiteral("DELETE")' in remove
    assert "QMessageBox::question" in remove and "remaining.isEmpty()" in remove
    assert "return result or _default_flows()" in CRM_WORKER


def test_desktop_contact_tabs_build_off_the_ui_thread_and_fill_in_chunks():
    snapshot = FEATURE.split("void CrmFeature::refreshContactSnapshot()", 1)[1].split("\nvoid CrmFeature::", 1)[0]
    assert "m_workers->start(" in snapshot
    assert "Qt::QueuedConnection" in snapshot and "if (guard)" in snapshot
    # The pool thread reads its own copies, never the widget.
    body = snapshot.split("m_workers->start(", 1)[1]
    assert "this" not in body.split("QMetaObject::invokeMethod", 1)[0]
    apply = FEATURE.split("void CrmFeature::applyContactSnapshot(", 1)[1].split("\nvoid CrmFeature::", 1)[0]
    assert "snapshot.generation != m_snapshotGeneration" in apply
    fill = FEATURE.split("void CrmFeature::fillContactRows()", 1)[1].split("\nbool CrmFeature::", 1)[0]
    assert "kFillChunkRows" in fill and "QTimer::singleShot(0" in fill
    assert "generation == m_fillGeneration" in fill
    # Lookups are indexed rather than a scan of every contact per row.
    lookup = FEATURE.split("QJsonObject CrmFeature::contactById(", 1)[1].split("\n}\n", 1)[0]
    assert "m_contactIndex.constFind(id)" in lookup
    assert "setMaxThreadCount(1)" in FEATURE


def test_desktop_mastodon_cells_open_the_profile_on_the_handles_instance():
    # "@grace@mastodon.social" opens https://mastodon.social/@grace: the host
    # comes from after the last "@" and the name ends the path.
    assert "static QUrl mastodonProfileUrl(const QString &value);" in FEATURE_HEADER
    helper = FEATURE.split("QUrl CrmFeature::mastodonProfileUrl(", 1)[1].split("\n}\n", 1)[0]
    assert "url.setHost(key.mid(at + 1));" in helper
    assert 'url.setPath(QStringLiteral("/@") + key.left(at));' in helper
    clicked = FEATURE.split("&QTableWidget::itemClicked", 1)[1].split("\n    });\n", 1)[0]
    assert "const QUrl url = mastodonProfileUrl(item->text());" in clicked
    assert "const QUrl profile = mastodonProfileUrl(value);" in FEATURE


def test_desktop_pipeline_tabs_head_with_stage_counts_that_filter():
    desktop = CRM_PACKAGE / "content" / "desktop"
    build = json.loads((desktop / "build.json").read_text(encoding="utf-8"))
    assert "CrmStageStrip.cpp" in build["sources"]
    strip = (desktop / "CrmStageStrip.cpp").read_text(encoding="utf-8")
    assert "void CrmStageStrip::paintEvent" in strip
    assert "QColor CrmStageStrip::stageColor" in strip
    render = FEATURE.split("void CrmFeature::renderContacts()", 1)[1]
    render = render.split("\nvoid CrmFeature::", 1)[0]
    assert "renderStageStrip();" in render
    body = FEATURE.split("void CrmFeature::renderStageStrip()", 1)[1]
    body = body.split("\n}\n", 1)[0]
    assert "stripStages(&counts)" in body
    stages = FEATURE.split("QStringList CrmFeature::stripStages(", 1)[1].split("\n}\n", 1)[0]
    assert "flowStages(flow)" in stages
    assert "m_snapshot.sheetRows" in stages
    assert "m_stageStrip->onPicked" in FEATURE
    assert "CrmStageStrip *m_stageStrip" in FEATURE_HEADER


def test_desktop_list_opens_on_its_new_stage():
    first = FEATURE.split("void CrmFeature::selectFirstStage()", 1)[1].split("\n}\n", 1)[0]
    assert "sheetId == m_stageSheet" in first
    assert 'stage.compare(QLatin1String("new"), Qt::CaseInsensitive) == 0' in first
    assert "m_stage->setCurrentIndex(qMax(0, m_stage->findData(first)));" in first
    changed = FEATURE.split("connect(m_sheet, &QComboBox::currentIndexChanged", 1)[1]
    changed = changed.split("\n    });\n", 1)[0]
    assert "selectFirstStage();" in changed
    tab = FEATURE.split("connect(tabs, &QTabWidget::currentChanged", 1)[1].split("\n    });\n", 1)[0]
    assert "m_stageSheet.clear();" in tab
    assert "selectFirstStage();" in tab
    snapshot = FEATURE.split("void CrmFeature::applyContactSnapshot(", 1)[1].split("\n}\n", 1)[0]
    assert "selectFirstStage();" in snapshot
    hit = FEATURE.split("void CrmFeature::activateSearchHit(", 1)[1].split("\n}\n", 1)[0]
    assert 'm_stage->findData(text(contact, "stage"))' in hit
    assert "QString m_stageSheet;" in FEATURE_HEADER


def test_desktop_activity_names_the_contact_and_links_the_email_id():
    assert 'QStringLiteral("Contact"), QStringLiteral("Status"),' in FEATURE
    assert 'QStringLiteral("Email ID")' in FEATURE
    render = FEATURE.split("void CrmFeature::renderActivity()", 1)[1].split("\n}\n", 1)[0]
    assert 'contactById(text(email, "contactId"))' in render
    assert 'text(email, "fromEmail")' in render
    assert "column == kActivityIdColumn && entry.url.isValid()" in render
    assert "font.setUnderline(true);" in render
    clicked = FEATURE.split("connect(m_emailsTable, &QTableWidget::itemClicked", 1)[1]
    clicked = clicked.split("\n    });\n", 1)[0]
    assert "item->column() == kActivityIdColumn" in clicked
    assert "openProviderSend();" in clicked


def test_desktop_table_splits_linked_toots_and_edits_last_contacted_with_now():
    # Mastodon conversations get their own column beside the linked mail.
    assert 'constexpr char kLinkedTootsColumnId[] = "linked-toots";' in FEATURE
    assert 'QStringLiteral("Linked toots")' in FEATURE
    assert '== QLatin1String("mastodon") ? m_contactToots : m_contactEmails' in FEATURE
    assert "QHash<QString, QList<QJsonObject>> m_contactToots;" in FEATURE_HEADER
    # Last contacted opens a date picker with Now beside it, and the row menu
    # marks a whole selection contacted now; both ride the bulk edit, which
    # the worker lets carry the date.
    assert "new QDateEdit(editor)" in FEATURE
    assert 'now->setText(QStringLiteral("Now"))' in FEATURE
    assert "item->setData(kContactedStampRole, source.lastContactedAt);" in FEATURE
    assert 'QStringLiteral("Mark contacted now")' in FEATURE
    assert '{QStringLiteral("lastContactedAt"), QJsonValue(stamp)}' in FEATURE
    assert '"lastContactedAt")' in CRM_WORKER.split("BULK_CONTACT_FIELDS = (", 1)[1][:120]


def test_pipelines_carry_a_type_icon_lists_are_assigned_and_stages_are_edited():
    # The worker keeps a type per flow and never loses it to a client that
    # saves flows without one.
    assert "FLOW_ICONS = (" in CRM_WORKER
    assert '"icon": _flow_icon(item.get("icon"), flow_id, existing)' in CRM_WORKER
    assert '_flows(data.get("flows"), result.get("flows"))' in CRM_WORKER
    # The desktop draws the same kinds and shows them on tabs and pickers.
    for kind in ("pipeline", "investors", "hiring", "partners", "customers",
                 "sales", "support", "projects", "events", "grants"):
        assert f'{{"{kind}", ' in FEATURE
        assert f'"{kind}"' in CRM_WORKER.split("FLOW_ICONS = (", 1)[1].split(")", 1)[0]
    tabs = FEATURE.split("void CrmFeature::renderSheetTabs(", 1)[1].split("\nvoid CrmFeature::", 1)[0]
    assert "m_tabs->setTabIcon(index" in tabs and "flowIcon(" in tabs
    tab_menu = FEATURE.split("void CrmFeature::showSheetTabMenu(", 1)[1].split("\nQJsonObject CrmFeature::", 1)[0]
    assert "addEditStagesAction(&menu, flowId)" in tab_menu
    assert "flowIcon(typeFlowId)" in tab_menu
    assert "m_stageStrip->setContextMenuPolicy(Qt::CustomContextMenu)" in FEATURE
    # One dialog edits name, type and ordered stages, and binds lists.
    manage = FEATURE.split("void CrmFeature::managePipelines(", 1)[1].split("\nvoid CrmFeature::", 1)[0]
    for needle in ('QStringLiteral("Pipelines and stages")', 'QStringLiteral("Lists")',
                   'QStringLiteral("Add stage")', "QAbstractItemView::InternalMove",
                   'QStringLiteral("icon"), draft.icon', "No pipeline (plain list)"):
        assert needle in manage
    # Contacts at a renamed stage follow it, and lists bind after the flows
    # they name are saved.
    assert "renamedStages" in manage and '"/contacts/bulk"' in manage
    save = manage.split('QByteArrayLiteral("PUT")', 1)[1]
    assert 'QStringLiteral("/views/") + listId' in save
    assert "void managePipelines(const QString &focusFlow = QString());" in FEATURE_HEADER
    # The web keeps the type through its flow editor and shows it on tabs.
    assert 'flows.push({ id, name: label, icon: draft.icon || "pipeline", stages: slugs });' in WEB_SCRIPT
    assert "crmFlowIcon(crmFlow(view.flow).icon)" in WEB_SCRIPT


def test_top_search_box_reaches_every_installed_package():
    """The CRM is the reference; every other package page answers the same box."""
    socket = (REPO / "desktop" / "src" / "NativePackageExtension.h").read_text(
        encoding="utf-8")
    assert "std::function<void()> searchIndexChanged;" in socket
    assert "virtual void prepareSearch() { refresh(); }" in socket

    # Any package that ships a native page must list its records in the top
    # search box, land on the chosen one, and say when its records change.
    pages = []
    for header in sorted((REPO / "marketplace" / "packages").glob(
            "*/content/desktop/*.h")):
        text = header.read_text(encoding="utf-8")
        if "public forkmesh::market::NativePackageWidget" not in text:
            continue
        pages.append(header.parent.parent.parent.name)
        source = header.with_suffix(".cpp").read_text(encoding="utf-8")
        assert "searchHits(const QString &query, int limit) const override;" in text, header
        assert "void activateSearchHit(const QString &id) override;" in text, header
        assert "m_searchIndexChanged(host.searchIndexChanged)" in source, header
        assert "m_searchIndexChanged();" in source, header
    assert {"crm", "calories", "marketing"} <= set(pages)

    # A page loads its records when opened, so the first query warms the ones
    # the user has not visited; the dropdown redraws when they answer.
    host = (REPO / "desktop" / "src" / "MainWindowMarket.cpp").read_text(encoding="utf-8")
    assert "[this] { nativePackageSearchIndexChanged(); }," in host
    warm = host.split("void MainWindow::prepareNativePackageSearch()", 1)[1].split("\n}\n", 1)[0]
    assert "page.widget->prepareSearch();" in warm and "page.searchPrepared = true;" in warm
    redraw = host.split("void MainWindow::nativePackageSearchIndexChanged()", 1)[1].split("\n}\n", 1)[0]
    assert "m_globalSearch->hasFocus()" in redraw and "m_globalSearchTimer->start();" in redraw

    # Packages hosted by the window itself answer from the same dropdown.
    popup = (REPO / "desktop" / "src" / "MainWindowRepoDetail.cpp").read_text(encoding="utf-8")
    assert "prepareNativePackageSearch();" in popup
    assert "prepareBuiltinPackageSearch();" in popup
    assert "builtinPackageSearchHits(query, 8)" in popup
    assert "activateBuiltinPackageSearchHit(s1, s2);" in popup
    builtin = (REPO / "desktop" / "src" / "MainWindowSearch.cpp").read_text(encoding="utf-8")
    hits = builtin.split("MainWindow::builtinPackageSearchHits(", 1)[1].split("\n}\n", 1)[0]
    for records in ("m_localNotes", "m_cloudNotes", "m_organizationTasks",
                    "m_calendarEvents", "m_timersFeature->ordered(now)",
                    "m_packageRegistry->isInstalled(manifest.id)"):
        assert records in hits, records



def test_desktop_page_is_a_list_sidebar_a_table_and_a_contact_panel():
    setup = FEATURE.split("CrmFeature::CrmFeature(", 1)[1].split("\n}\n", 1)[0]
    # The tab strip is gone: lists run down the left, the table sits in the
    # middle, and the contact opens in a third column.
    assert "tabs->tabBar()->hide();" in setup
    assert setup.index("body->addWidget(side);") < setup.index("body->addWidget(tabs);") < \
        setup.index("body->addWidget(detail);")
    assert "showSheetTabMenu(id, m_sheetList->viewport()->mapToGlobal(position))" in setup
    # Each list shows the faces on it and its pipeline's stages as a bar.
    side = FEATURE.split("void CrmFeature::renderSheetList()", 1)[1].split("\nvoid CrmFeature::", 1)[0]
    assert "kListAvatars" in side and "contactPicture(contact, kListAvatarSize)" in side
    assert "const QStringList stages = flowStages(flow);" in side
    assert "new ListStageBar(segments)" in side
    # The toolbar's People button opens a grid of only the contacts with a
    # picture, sortable by most recent, each card naming its lists and pipeline.
    people = FEATURE.split("void CrmFeature::renderPeople()", 1)[1].split("\nvoid CrmFeature::", 1)[0]
    assert "if (!m_avatarImages.contains(avatarSource(row)))" in people
    assert "return left.createdAt > right.createdAt;" in people
    assert "listLine" in people and "pipeline" in people
    assert 'QStringLiteral("Most recent"), QStringLiteral("recent")' in setup


def test_desktop_lists_merge_and_columns_delete_outright():
    merge = FEATURE.split("void CrmFeature::mergeSheetInto(", 1)[1].split("\nvoid CrmFeature::", 1)[0]
    assert 'organizationPath(QStringLiteral("/contacts/bulk"))' in merge
    assert 'QJsonObject{{QStringLiteral("columns"), layoutColumns}}' in merge
    assert 'QByteArrayLiteral("PATCH")' in merge and 'QByteArrayLiteral("DELETE")' in merge
    assert "saveLayout();" in merge and "finish();" in merge
    menu = FEATURE.split("void CrmFeature::showColumnMenu(", 1)[1].split("\nvoid CrmFeature::", 1)[0]
    assert "deleteBuiltinColumn(key)" in menu and "restoreBuiltinColumn(field)" in menu
    assert "removedColumns" in FEATURE.split("void CrmFeature::rebuildContactColumns()", 1)[1]
    assert "if field in DELETABLE_COLUMN_FIELDS and field not in source[:index]" in CRM_WORKER


def test_add_members_suggests_forkmesh_users_outside_the_organization():
    extension = (REPO / "desktop" / "src" / "NativePackageExtension.h").read_text(
        encoding="utf-8")
    assert "std::function<QStringList()> userDirectory;" in extension
    assert "virtual void userDirectoryChanged() {}" in extension
    assert "return ledgerRecipientCandidates();" in HOST
    assert "[widget] { widget->userDirectoryChanged(); }" in HOST

    completer = FEATURE.split("class ShareNameCompleter", 1)[1].split("\n};\n", 1)[0]
    assert "QStringList splitPath(" in completer
    assert "QString pathFromIndex(" in completer
    assert 'QStringLiteral("[\\\\s,;]")' in completer

    dialog = FEATURE.split("void CrmFeature::addSheetMembers(", 1)[1].split("\n}\n", 1)[0]
    assert "m_userDirectory()" in dialog
    assert "m_members.contains(QJsonValue(name))" in dialog
    assert "others->setCompleter(completer);" in dialog
    assert dialog.index("dialog.exec()") < dialog.index("m_userDirectoryListener = nullptr;")
    assert "m_userDirectory(host.userDirectory)" in FEATURE
    assert "void userDirectoryChanged() override;" in FEATURE_HEADER


def test_desktop_team_leaderboard_ranks_members_and_charts_their_days():
    desktop = CRM_PACKAGE / "content" / "desktop"
    build = json.loads((desktop / "build.json").read_text(encoding="utf-8"))
    assert {"CrmTeamChart.h", "CrmTeamChart.cpp"} <= set(build["sources"])
    chart = (desktop / "CrmTeamChart.cpp").read_text(encoding="utf-8")
    assert "void CrmTeamChart::paintEvent" in chart
    assert "QEvent::ToolTip" in chart
    assert 'tabs->addTab(teamPage, QStringLiteral("Team leaderboard"));' in FEATURE
    assert 'm_activity = payload.value(QStringLiteral("activity")).toObject();' in FEATURE
    assert "offsetFromUtc() / 60" in FEATURE
    apply = FEATURE.split("void CrmFeature::applyOrganization(", 1)[1].split("\n}\n", 1)[0]
    assert "renderTeam();" in apply
    render = FEATURE.split("void CrmFeature::renderTeam()", 1)[1].split("\n}\n", 1)[0]
    assert "a.touches() > b.touches()" in render
    assert "m_teamChart->setDays(daily);" in render
    assert "CrmTeamChart *m_teamChart" in FEATURE_HEADER


def test_desktop_copies_a_link_that_opens_the_contact_on_the_web():
    assert "void copyContactLink(const QJsonObject &contact);" in FEATURE_HEADER
    assert 'constexpr char kCrmPage[] = "https://app.forkmesh.com/dashboard/crm";' in FEATURE
    assert '[this, id] { copyContactLink(contactById(id)); }' in FEATURE
    body = FEATURE.split("void CrmFeature::copyContactLink(const QJsonObject &contact)", 1)[1].split("\n}\n", 1)[0]
    link = FEATURE.split("QString crmPageLink(", 1)[1].split("\n}\n", 1)[0]
    assert "?org=" not in link
    assert link.count("QUrl::toPercentEncoding(") == 1
    assert 'crmPageLink(QStringLiteral("contact"), id)' in body


def test_desktop_copies_a_link_that_opens_a_list_on_the_web():
    assert "void copySheetLink(const QString &id);" in FEATURE_HEADER
    assert FEATURE.count("[this, id] { copySheetLink(id); }") == 2
    body = FEATURE.split("void CrmFeature::copySheetLink(const QString &id)", 1)[1].split("\n}\n", 1)[0]
    assert "if (!storedSheet(sheet) || organization.isEmpty())" in body
    assert 'crmPageLink(QStringLiteral("list"), id)' in body
def test_desktop_list_cards_mark_the_open_list_merge_by_submenu_and_show_access():
    side = FEATURE.split("void CrmFeature::renderSheetList()", 1)[1].split("\nvoid CrmFeature::", 1)[0]
    assert 'card->setObjectName(QStringLiteral("crmSheetRow"))' in side
    assert 'QFrame#crmSheetRow[current=\\"true\\"]' in side
    assert "QFrame#crmSheetRow QLabel, QFrame#crmSheetRow QToolButton { background: transparent; }" in side
    assert side.count("m_sheetList->setItemWidget(item, row)") == 2
    assert "access << text(sheet, \"owner\")" in side and "access += names;" in side
    assert "kAccessAvatars" in side and "initialsPixmap(contactInitials(person)" in side
    sync = FEATURE.split("void CrmFeature::syncSheetList()", 1)[1].split("\n}\n", 1)[0]
    assert 'card->setProperty("current", here)' in sync
    assert "card->style()->polish(card)" in sync
    assert "m_sheetList->setCurrentItem(open)" in sync

    tab = FEATURE.split("void CrmFeature::showSheetTabMenu(", 1)[1].split("\nQJsonObject CrmFeature::", 1)[0]
    assert 'QMenu *merge = menu.addMenu(QStringLiteral("Merge into"))' in tab
    assert "mergeSheetInto(id, otherId)" in tab
    assert "!merge->isEmpty()" in tab
    merge = FEATURE.split("void CrmFeature::mergeSheetInto(", 1)[1].split("\nvoid CrmFeature::", 1)[0]
    assert "QMessageBox::question" in merge
    assert "void mergeSheetInto(const QString &id, const QString &targetId);" in FEATURE_HEADER

    header = FEATURE.split('title->setObjectName(QStringLiteral("pageTitle"));', 1)[1]
    header = header.split("m_status = new QLabel", 1)[0]
    assert 'toolButton(QStringLiteral("workflow"), QStringLiteral("Pipelines")' in header
    assert "connect(m_pipelinesButton, &QToolButton::clicked" in FEATURE
    assert "m_pipelinesButton->setEnabled(m_canManageSettings);" in FEATURE
    assert "m_peopleButton, m_pipelinesButton})" in FEATURE
    assert "QToolButton *m_pipelinesButton = nullptr;" in FEATURE_HEADER


def test_desktop_rail_button_counts_contacts_never_contacted():
    socket = (REPO / "desktop" / "src" / "NativePackageExtension.h").read_text(encoding="utf-8")
    assert "std::function<void()> navigationBadgeChanged;" in socket
    assert "virtual int navigationBadge() const { return 0; }" in socket
    assert "virtual void prepareNavigationBadge() {}" in socket
    assert "[this, id] { syncNativePackageBadge(id); }," in HOST
    assert "widget->prepareNavigationBadge();" in HOST
    assert "rail->setBadgeCount(qMax(0, page.widget->navigationBadge()));" in HOST
    assert "m_navigationBadgeChanged(host.navigationBadgeChanged)," in FEATURE
    snapshot = FEATURE.split("void CrmFeature::refreshContactSnapshot()", 1)[1].split("\n}\n", 1)[0]
    assert 'value(QStringLiteral("lastContactedAt")).toDouble() <= 0' in snapshot
    assert "m_navigationBadgeChanged();" in snapshot
    assert "return m_uncontacted;" in FEATURE


def test_every_account_lookup_reads_a_dead_instance_as_an_answer():
    lookups = FEATURE.count('setPath(QStringLiteral("/api/v1/accounts/lookup"))')
    assert lookups >= 3
    for status in (404, 406, 410):
        marked = FEATURE.count("markExpectedHttpStatus(lookupRequest, %d)" % status) \
            + FEATURE.count("markExpectedHttpStatus(request, %d)" % status)
        assert marked >= lookups, status


def test_desktop_detail_card_writes_an_investor_email_with_the_current_agent():
    extension = (REPO / "desktop" / "src" / "NativePackageExtension.h").read_text(
        encoding="utf-8")
    assert "std::function<QString()> agentName;" in extension
    assert ("std::function<void(const QString &, NativePackageAgentReplyHandler)> askAgent;"
            in extension)
    assert "askNativePackageAgent(prompt, std::move(done));" in HOST
    ask = HOST.split("void MainWindow::askNativePackageAgent(", 1)[1].split("\n}\n", 1)[0]
    assert "defaultAgentProvider()" in ask
    assert "new ClaudeStreamSession(this)" in ask and "new CodexAppServerSession(this)" in ask
    assert "activeAgentAccountEnv(" in ask
    assert "m_askAgent(host.askAgent)" in FEATURE
    detail = FEATURE.split("void CrmFeature::showContactDetail(", 1)[1].split("\n}\n", 1)[0]
    assert "draftInvestorEmail(contactById(contactId))" in detail
    assert "deckActivityLine(deck)" in detail
    draft = FEATURE.split("void CrmFeature::draftInvestorEmail(", 1)[1].split("\n}\n", 1)[0]
    assert 'organizationPath(QStringLiteral("/decks"))' in draft
    prompt = FEATURE.split("QString CrmFeature::investorEmailPrompt(", 1)[1].split("\n}\n", 1)[0]
    for part in ("contactColumns()", "customColumnLabel(", "contactListNames(",
                 "m_contactEmails", "m_accountReads", "deckUrl", "founders@forkmesh.com"):
        assert part in prompt, part
    ask_crm = FEATURE.split("void CrmFeature::askAgentForEmail(", 1)[1].split("\n}\n", 1)[0]
    assert "parseEmailDraft(reply, deckUrl, &subject, &body)" in ask_crm
    assert "composeEmail(current, subject, body)" in ask_crm
    parse = FEATURE.split("bool CrmFeature::parseEmailDraft(", 1)[1].split("\n}\n", 1)[0]
    assert "!bodyText.contains(deckUrl)" in parse
    assert "m_decks = payload.value(QStringLiteral(\"decks\")).toArray();" in FEATURE


def _desktop_set(name):
    block = FEATURE.split(f"const QSet<QString> {name}{{", 1)[1].split("};", 1)[0]
    return set(re.findall(r'QStringLiteral\("([^"]+)"\)', block))


def test_desktop_list_heads_with_a_lead_to_reach_and_one_to_follow_up():
    def worker_set(name):
        block = CRM_WORKER.split(f"\n{name} = frozenset({{", 1)[1].split("})", 1)[0]
        return set(re.findall(r'"([a-z-]+)"', block))

    assert _desktop_set("kWonStages") == worker_set("CLOSED_DEAL_STAGES")
    assert _desktop_set("kEndedStages") == worker_set("ENDED_STAGES")
    assert _desktop_set("kUnsentStatuses") == set(_entry_constant("UNSENT_STATUSES", CRM_WORKER))
    assert f"kNextReplyDays = {_entry_constant('EMAIL_SUGGESTION_REPLY_DAYS', CRM_WORKER)};" in FEATURE
    assert (f"kNextFollowUpDays = "
            f"{_entry_constant('EMAIL_SUGGESTION_FOLLOW_UP_DAYS', CRM_WORKER)};") in FEATURE
    rank = FEATURE.split("NextContact CrmFeature::nextContact(NextMode mode) const", 1)[1].split("\n}\n", 1)[0]
    assert "heardAt > wroteAt && heardAt >= last" in rank
    assert "kWonStages.contains(stage) || kEndedStages.contains(stage)" in rank
    assert "if (waiting || closed || last > 0)" in rank
    assert "std::make_tuple(tier, step.isEmpty() ? 1 : 0, order, id)" in rank
    assert "step.isEmpty() ? 1 : 0, -marks.value," in rank
    assert "posted > 0 ? 0 : 1, -posted," in rank
    assert "best.upcoming << reachRanked.at(at).id" in rank
    assert "m_nextSkipped.contains(id)" in rank
    marks = FEATURE.split("LeadMarks leadMarks(const QJsonObject &contact)", 1)[1].split("\n}\n", 1)[0]
    assert 'endsWith(QLatin1String("_rank"))' in marks and 'endsWith(QLatin1String("_score"))' in marks
    pair = FEATURE.split("void CrmFeature::renderNextContact()", 1)[1].split("\nvoid CrmFeature::", 1)[0]
    assert "nextContactCard(NextMode::ReachOut)" in pair and "nextContactCard(NextMode::FollowUp)" in pair
    assert "queueReachMastodon(nextContact(NextMode::ReachOut).ranked)" in pair
    card = FEATURE.split("QWidget *CrmFeature::nextContactCard(NextMode mode)", 1)[1].split("\nvoid CrmFeature::", 1)[0]
    assert "<b>Why them:</b>" in card
    assert "nextPostLabel(contact, false)" in card and "nextPostLabel(person, true)" in card
    assert "openMailCompose(" in card and "markContacted(" in card and "showContactDetail(" in card
    assert "contactColumns()" in card and "contactListNames(contactId)" in card
    render = FEATURE.split("void CrmFeature::renderContacts()", 1)[1].split("\n}\n", 1)[0]
    assert "renderNextContact();" in render
    assert 'QStringLiteral("crmNextContact")' in FEATURE


def test_desktop_detail_card_logs_a_toot_by_its_link():
    detail = FEATURE.split("void CrmFeature::showContactDetail(", 1)[1].split("\n}\n", 1)[0]
    assert 'new QPushButton(QStringLiteral("Log toot"))' in detail
    assert "logToot(contactId)" in detail
    assert "void logToot(const QString &contactId);" in FEATURE_HEADER
    log = FEATURE.split("void CrmFeature::logToot(", 1)[1].split("\n}\n", 1)[0]
    assert "QInputDialog::getText(" in log
    assert 'organizationPath(QStringLiteral("/emails/toot"))' in log
    assert '{QStringLiteral("contactId"), contactId}' in log
    assert "loadOrganization();" in log
    assert 'mastodonErrorText(text(payload, "error"), error)' in log
    assert '("send", "link", "toot", "toots")' in CRM_WORKER


def test_desktop_checks_the_mastodon_account_once_per_load():
    load = FEATURE.split("void CrmFeature::loadOrganization()", 1)[1].split("\n}\n", 1)[0]
    assert load.count("syncToots(true);") == 2
    sync = FEATURE.split("void CrmFeature::syncToots(", 1)[1].split("\n}\n", 1)[0]
    assert 'organizationPath(QStringLiteral("/emails/toots"))' in sync
    assert 'QJsonObject{{QStringLiteral("auto"), true}}' in sync
    assert '.value(QStringLiteral("connected")).toBool()' in sync
    assert "if (logged > 0) {" in sync and "loadOrganization();" in sync
    assert "if (!automatic)\n                setStatus(" in sync
    assert "bool m_tootSyncRunning = false;" in FEATURE_HEADER


def test_desktop_settings_connect_the_mastodon_account_for_managers():
    settings = FEATURE.split("void CrmFeature::editSettings()", 1)[1].split("\n}\n", 1)[0]
    for label in ('"Mastodon account"', '"Mastodon access token"', '"Disconnect"',
                  '"Check toots now"'):
        assert label in settings, label
    assert "mastodonToken->setEchoMode(QLineEdit::Password);" in settings
    assert "read:statuses" in settings and "Preferences → Development" in settings
    assert "mastodonSave->setEnabled(m_canManageSettings);" in settings
    assert '{QStringLiteral("mastodonDisconnect"), true}' in settings
    assert "syncToots(false, showMastodon);" in settings
    state = FEATURE.split("QString mastodonStateLine(const QJsonObject &mastodon)", 1)[1].split(
        "\n}\n", 1)[0]
    assert '"lastSyncAt"' in state and '"lastError"' in state
    errors = FEATURE.split("QString mastodonErrorText(", 1)[1].split("\n}\n", 1)[0]
    for code in ("invalid_toot_url", "toot_contact_not_found", "mastodon_not_connected",
                 "mastodon_token_rejected", "mastodon_unavailable",
                 "mastodon_account_not_found", "invalid_mastodon_account"):
        assert f'"{code}"' in errors, code
        assert f'"{code}"' in CRM_WORKER, code
