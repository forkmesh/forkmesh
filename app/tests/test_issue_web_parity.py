"""Contract coverage for the browser/Qt issue lifecycle parity surface."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
ISSUE_IO = (ROOT / "public/dashboard/js/03-issues-profile-io.js").read_text()
REPO_UI = (ROOT / "public/dashboard/js/06-repo-content.js").read_text()
WIRING = (ROOT / "public/dashboard/js/08-repo-detail-network.js").read_text()
ENTRY = (ROOT / "src/entry.py").read_text()
EVENTS = (ROOT / "src/events.py").read_text()


def _slice(source, start, end):
    begin = source.index(start)
    finish = source.index(end, begin)
    return source[begin:finish]


def test_browser_signs_every_user_facing_qt_issue_mutation():
    content = _slice(
        ISSUE_IO,
        "function webIssueEventContent(type, fields = {})",
        "function webIssueEventId(type, ts)",
    )
    for event_type in (
        "edit", "title", "status", "labels", "milestone", "dates",
        "priority", "progress", "assignees", "delete", "vote",
    ):
        assert f'type === "{event_type}"' in content

    submit = _slice(
        ISSUE_IO,
        "async function submitWebIssueEvent(repo, number, type, fields = {})",
        "async function setWebIssueSubscription",
    )
    assert '${webEventSigningPrefix("issue-event", repo)}${type}' in submit
    assert "ownerAccount: state.session?.nodeName" in submit
    assert "sessionToken: state.session?.sessionToken" in submit


def test_browser_issue_timestamps_use_epoch_milliseconds():
    open_signer = _slice(
        ISSUE_IO,
        "async function signWebIssueOpenEvent",
        "async function submitWebIssue",
    )
    comment_signer = _slice(
        ISSUE_IO,
        "async function submitWebIssueComment",
        "function webIssueEventContent",
    )
    mutation_signer = _slice(
        ISSUE_IO,
        "async function submitWebIssueEvent",
        "async function setWebIssueSubscription",
    )
    for signer in (open_signer, comment_signer, mutation_signer):
        assert "const ts = Date.now();" in signer
        assert "Math.floor(Date.now() / 1000)" not in signer


def test_issue_detail_exposes_github_style_lifecycle_controls():
    detail = _slice(
        REPO_UI,
        "function renderRepoIssueDetail(repo, number, parsed)",
        "function setRepoIssueRecordOpen(open)",
    )
    composer = _slice(
        REPO_UI,
        "function renderIssueCommentForm(number)",
        "function applyIssueCommentFormat(textarea, format)",
    )
    # Close / reopen sit beside Comment, and read "... with comment" once a
    # draft exists.
    for marker in ("Reopen issue", "Close issue", "Close with comment", "Reopen with comment"):
        assert marker in composer
    assert "${renderIssueCommentForm(number)}" in detail
    for marker in (
        "data-repo-issue-title-edit",
        "data-repo-issue-description-edit",
        "data-repo-issue-delete",
        "data-repo-issue-vote",
        "data-repo-issue-subscription",
        "data-repo-issue-metadata-form",
        "data-repo-issue-assignees",
        "data-repo-issue-labels",
        "data-repo-issue-milestone-edit",
        "data-repo-issue-priority",
        "data-repo-issue-progress",
        "data-repo-issue-start-date",
        "data-repo-issue-end-date",
    ):
        assert marker in detail


def test_issue_delete_is_offered_to_every_authorized_issue_manager():
    detail = _slice(
        REPO_UI,
        "function renderRepoRecordDetail(repo, kind, number, parsed)",
        "async function loadRepoRecordDetail",
    )
    detail = _slice(
        REPO_UI,
        "function renderRepoIssueDetail(repo, number, parsed)",
        "function setRepoIssueRecordOpen(open)",
    )
    gate = _slice(detail, "data-repo-issue-sidebar-actions", "</aside>")
    assert "${canManage ? `<button type=\"button\" data-repo-issue-delete" in gate
    assert "const canManage = Boolean(parsed.issueMutationAuthorized);" in detail
    # The browser's throwaway signing key proves nothing about repository
    # authority. Gating Delete on it hid the control from owners for every
    # issue they had not filed from that exact browser profile.
    assert "webIssuePublicKey" not in REPO_UI
    assert "issueCreator" not in gate


def test_issue_moves_to_a_task_before_it_is_deleted():
    detail = _slice(
        REPO_UI,
        "function renderRepoIssueDetail(repo, number, parsed)",
        "function setRepoIssueRecordOpen(open)",
    )
    gate = _slice(detail, "data-repo-issue-sidebar-actions", "</aside>")
    assert "${canManage ? `<button type=\"button\" data-repo-issue-move-to-task" in gate
    assert "moveWebIssueToTask(" in WIRING

    move = _slice(
        ISSUE_IO,
        "async function moveWebIssueToTask(button = null)",
        "async function handleWebIssueTitleSubmit(form)",
    )
    create = move.index('orgApiRequest(\n        "POST", "/api/tasks"')
    delete = move.index('submitWebIssueEvent(repo, number, "delete", { target: "self" })')
    undo = move.index('"DELETE", `/api/tasks/${encodeURIComponent(task.id)}`')
    leave = move.index("location.assign(`/dashboard/tasks?task=")
    assert create < delete < undo < leave
    assert move[create:delete].count("return;") == 1
    assert move[undo:leave].count("return;") == 1

    body = _slice(
        ISSUE_IO,
        "function webIssueTaskBody(repo, number, values, body)",
        "async function moveWebIssueToTask(",
    )
    assert ".slice(0, 160)" in body
    assert "const room = 4000 - source.length - 2;" in body
    assert "/^[A-Za-z0-9._-]{1,100}$/" in body


def test_desktop_issue_moves_to_a_task_before_it_is_deleted():
    issues = (ROOT.parent / "desktop/src/MainWindowIssues.cpp").read_text()
    move = _slice(
        issues,
        "void MainWindow::moveCurrentIssueToTask()",
        "void MainWindow::deleteCurrentIssueWithHistory(int number)",
    )
    create = move.index('QByteArrayLiteral("POST"), QStringLiteral("/api/tasks")')
    remove = move.index("store.removeIssue(number, &removeError)")
    undo = move.index('QByteArrayLiteral("DELETE"),')
    opened = move.index("openCreatedOrganizationTask(task);")
    assert create < remove < undo < opened
    assert "if (!store.canWrite())" in move
    assert "moveCurrentIssueToTask(); });" in issues
    assert "&MainWindow::moveCurrentIssueToTask);" in issues


def test_phone_issue_moves_create_the_task_before_the_delete():
    native = ROOT.parent / "native"
    swift = (native / "ios/ForkMeshCore/Sources/FMCode/Collab/IssueTaskMove.swift").read_text()
    kotlin = (native / "android/core/src/main/kotlin/com/forkmesh/nativeapp/collab/IssueTaskMove.kt").read_text()
    for source, create, delete, undo in (
        (swift, 'tasks("POST", ""', "writer.sendIssue(issue.number, .deleteOwnIssue())", '"DELETE", "/\\(id)"'),
        (kotlin, 'tasks("POST", ""', "writer.sendIssue(issue.number, IssueEdit.deleteOwnIssue())", 'tasks("DELETE", "/$id"'),
    ):
        assert source.index(create) < source.index(delete) < source.index(undo)


def test_owner_delete_removes_the_issue_on_every_materializer():
    store = (ROOT.parent / "desktop/src/IssueStore.cpp").read_text()
    apply_remote = store[store.index("bool IssueStore::applyRemoteEvent("):]
    assert 'ev.type == "delete" && ev.target == "self"' in apply_remote
    assert "return removeIssue(number, error);" in apply_remote

    intake = _slice(
        (ROOT.parent / "server/intake.go").read_text(),
        "func writeIssueItem(",
        "func applyIssueProjection(",
    )
    assert 'eventType == "delete" && stringValue(item.Event["target"]) == "self"' in intake
    assert "removeTreeUnder(filepath.Join(root, \".forkmesh\", \"issues\"), filepath.Dir(existingPath))" in intake


def test_issue_detail_names_the_opening_author_once():
    """The opening comment owns the author; the hero above must not repeat them.

    The detail page used to introduce the reporter twice - avatar and name in
    the hero, then the same avatar and name again on the opening card - so one
    person filing one issue read as two participants (issue #651). As on
    GitHub, the hero is the title and state; the opening card names the author.
    """
    detail = _slice(
        REPO_UI,
        "function renderRepoIssueDetail(repo, number, parsed)",
        "function setRepoIssueRecordOpen(open)",
    )
    hero = _slice(detail, "<header data-repo-record-hero", "</header>")
    opening = _slice(detail, "const openingCard = `", "const subIssues =")
    assert "issueActorAvatarHtml(author" not in hero
    assert "escapeHtml(author)" not in hero
    assert opening.count("issueActorAvatarHtml(author") == 1
    assert opening.count("issueCardAuthorHtml(author") == 1


def test_issue_detail_keeps_an_empty_description_compact():
    """No body means no padded card - and no invented copy to fill it.

    An empty description was replaced with placeholder prose, which guaranteed
    the opening card always had something to pad out. It now collapses to a
    single line, or to an "Add a description" control for someone allowed to
    write one (issue #651).
    """
    detail = _slice(
        REPO_UI,
        "function renderRepoIssueDetail(repo, number, parsed)",
        "function setRepoIssueRecordOpen(open)",
    )
    assert 'const body = parsed.body || "";' in detail
    assert "const hasBody = Boolean(String(body).trim());" in detail
    assert "No description was committed for this record." not in REPO_UI
    opening = _slice(detail, "const description = hasBody", "const openingCard = `")
    # The bodied branch is the only one that pads a body; both empty branches
    # stay one row tall, and the authorized one still reaches the same editor.
    assert 'data-repo-record-description="body"' in opening
    assert 'data-repo-record-description="add"' in opening
    assert 'data-repo-record-description="empty"' in opening
    assert opening.count("data-repo-record-body") == 1
    assert opening.count("data-repo-issue-description-edit") == 1
    assert opening.count("descriptionForm") == 2
    assert "data-repo-issue-description-form" in detail
    # The conversation column opts out of the row stretch the way the sidebar
    # does, so the opening card is never inflated to the sidebar's height.
    assert '<div class="grid min-w-0 gap-4 self-start">' in detail
    assert '<aside data-repo-record-sidebar class="min-w-0 self-start">' in detail


def test_issue_mutation_controls_are_delegated_to_handlers():
    for marker in (
        "handleWebIssueTitleSubmit(issueTitleForm)",
        "handleWebIssueDescriptionSubmit(issueDescriptionForm)",
        "handleWebIssueMetadataSubmit(issueMetadataForm)",
        'handleWebIssueAction("vote")',
        'handleWebIssueAction("delete-issue")',
        '"edit-comment"',
        '"delete-comment"',
    ):
        assert marker in WIRING


def test_structural_issue_events_require_repository_authority():
    handler = _slice(
        ENTRY,
        "async def _accept_issue_submission(",
        "async def discussions_handler",
    )
    assert "owner_event_types = {" in handler
    for event_type in (
        "edit", "title", "status", "labels", "milestone", "dates",
        "priority", "progress", "bounty", "assignees", "agent", "delete",
    ):
        assert f'"{event_type}"' in handler
    assert "await _authorize_issue_manager(env, owner, data, request)" in handler
    # Open/comment/vote stay relay-capable and are deliberately outside the
    # repository-owner structural-event set.
    owner_set = _slice(handler, "owner_event_types = {", "if event_type in owner_event_types")
    for public_type in ("open", "comment", "vote"):
        assert f'"{public_type}"' not in owner_set


def test_issue_manager_supports_personal_and_org_repository_owners():
    authorization = _slice(
        ENTRY,
        "async def _authorize_issue_manager",
        "async def _inbox_author_over_quota",
    )
    assert "await _authorize_owner_account" in authorization
    assert "await _org_row" in authorization
    assert "await _org_role" in authorization
    assert 'role in ("owner", "admin")' in authorization


def test_subscription_can_be_set_and_reloaded_with_account_session():
    backend = _slice(
        ENTRY,
        "async def subscribe_handler",
        "async def send_notification_digests",
    )
    assert 'action == "status"' in backend
    assert "session_authorized" in backend
    assert '"subscribed": bool(subscription' in backend
    assert "async function loadWebIssueSubscription" in ISSUE_IO
    assert "loadWebIssueSubscription(repo, number)," in REPO_UI
    assert "[parsed.issueMutationAuthorized, parsed.issueSubscribed] = await issueAccess;" in REPO_UI


def test_worker_accepts_only_known_qt_issue_events_and_matches_agent_signing():
    for event_type in (
        "open", "comment", "edit", "title", "status", "labels",
        "milestone", "dates", "priority", "progress", "bounty",
        "assignees", "agent", "reaction", "delete", "vote",
    ):
        assert f'"{event_type}"' in EVENTS
    assert "event_type not in ISSUE_EVENT_TYPES" in EVENTS
    agent = _slice(EVENTS, 'if t == "agent":', 'if t == "reaction":')
    assert '"pr" if ev.get("agentCreatePr") else "no-pr"' in agent


def test_worker_reaction_event_content_matches_the_desktop_store():
    # IssueStore::contentForSigning joins the reacted-to event id and the
    # reaction slug with a NUL, and a reaction with neither is not an event any
    # verifier should accept.
    reaction = _slice(EVENTS, 'if t == "reaction":', 'if t == "delete":')
    assert 'ev.get("target", "") or ""' in reaction
    assert 'ev.get("reaction", "") or ""' in reaction
    assert '"\\x00".join' in reaction
    verify = _slice(EVENTS, "async def verify_issue_event", "DISCUSSION_CATEGORIES")
    assert 'if event_type in ("edit", "delete", "reaction"):' in verify
    assert 'if event_type == "reaction":' in verify


def test_web_issue_thread_draws_reactions_instead_of_activity_lines():
    # A model bot reacts to every mention it picks up, so the web view has to
    # fold those events into chips; left as activity rows they would bury the
    # conversation they were meant to acknowledge.
    assert "function foldIssueReactions(events)" in REPO_UI
    assert "function renderIssueReactionChips(tallies)" in REPO_UI
    timeline = _slice(REPO_UI, "function renderIssueTimeline(events)",
                      "function projectJsonPath(number)")
    assert 'if (ev.type === "reaction") continue;' in timeline
    assert 'reactionIds.has(String(ev.target))) continue;' in timeline
    assert "_reactions: renderIssueReactionChips(" in timeline
    assert "eyes: " in REPO_UI


def test_issue_timeline_draws_activity_on_a_rail_and_comments_as_cards():
    timeline = _slice(
        REPO_UI,
        "function renderIssueTimeline(events)",
        "function projectJsonPath(number)",
    )
    # Every non-comment event, mentions and pull request links included, is
    # one line on the rail.
    assert timeline.count("renderIssueTimelineEvent(") == 3
    assert "ISSUE_PULL_LINK_LINE.test(line)" in timeline
    assert "renderIssueTimelineComment(ev)" in timeline
    event = _slice(
        REPO_UI,
        "function renderIssueTimelineEvent(",
        "function issueEventDescriptionHtml(ev)",
    )
    assert "data-repo-issue-event" in event
    assert "rounded-full border-2" in event
    # Labels, priority and milestone are drawn as what they are; everything
    # else keeps the shared plain sentence.
    rich = _slice(REPO_UI, "function issueEventDescriptionHtml(ev)", "// Full issue activity timeline")
    assert "renderIssueLabelChip(label)" in rich
    assert "escapeHtml(issueEventDescription(ev))" in rich
    # A comment header is neutral: green is kept for primary actions.
    comment = _slice(REPO_UI, "function renderIssueTimelineComment(ev)", "function renderIssueTimelineEvent(")
    assert "bg-primary/10" not in comment
    assert "bg-[rgb(var(--dashboard-subtle-rgb))]" in comment

    shell = (ROOT / "public/dashboard/shell.html").read_text()
    assert '[data-repo-issue-timeline][data-empty="false"]::before' in shell
    assert "[data-repo-issue-timeline] > [data-repo-issue-comment] > *" in shell


def test_issue_sidebar_edits_through_pickers_that_submit_the_metadata_form():
    detail = _slice(
        REPO_UI,
        "function renderRepoIssueDetail(repo, number, parsed)",
        "function setRepoIssueRecordOpen(open)",
    )
    # The always-open "Manage issue" form is gone; its inputs stay, hidden, as
    # the one place a change is diffed and signed.
    assert "Manage issue" not in detail
    assert "<form data-repo-issue-metadata-form hidden>" in detail
    for field in ("assignees", "labels", "fields", "milestone"):
        assert f'sidebarSection("' in detail and f', "{field}")' in detail
    assert 'data-repo-issue-picker-open="${picker}"' in detail
    assert 'picker = "", editable = canManage' in detail
    assert "picker && editable" in detail
    assert 'sidebarSection("Development", developmentValue, "development", Boolean(state.session?.nodeName))' in detail

    pickers = _slice(REPO_UI, "let issuePicker = null;", "function mountIssueStickyHeader(container)")
    assert "form.requestSubmit();" in pickers
    assert 'selected.length < 10' in pickers
    wiring = (ROOT / "public/dashboard/js/08-repo-detail-network.js").read_text()
    assert "openIssuePicker(field)" in wiring
    assert 'closeIssuePicker({ apply: true });' in wiring
    assert '"[data-repo-issue-picker], [data-repo-issue-picker-open]"' in wiring


def test_an_issue_opens_onto_its_page_frame_instead_of_a_loading_line():
    loader = _slice(
        REPO_UI,
        "async function loadRepoRecordDetail(repo, kind, number, preferMirror = false)",
        "function setRepoTabCount(tab, count)",
    )
    assert 'kind === "issues"\n          ? renderRepoIssueDetailSkeleton(repo, number)' in loader
    # Unless the node sent the issue drawn already (repo_ssr_issue.py).
    assert loader.index("takeRepoFirstViewIssue(repo, number)") < loader.index(
        "renderRepoIssueDetailSkeleton(repo, number)")
    skeleton = _slice(
        REPO_UI,
        "function renderRepoIssueDetailSkeleton(repo, number)",
        "function setRepoIssueRecordOpen(open)",
    )
    # Drawn from the clicked row where there is one, shimmering where not.
    assert 'find((issue) => String(issue.number) === String(number))' in skeleton
    assert "escapeHtml(item.title)" in skeleton
    assert "renderIssueLabelChip(label)" in skeleton
    assert "motion-reduce:animate-none" in skeleton
    # It is not the detail: the converge poll must not mistake it for one.
    assert "data-repo-record-detail" not in skeleton
    assert 'aria-busy="true"' in skeleton


def test_issue_title_editor_is_full_width_and_legible_on_phones():
    # Issue #718: on a phone the title input shared its row with Save, Cancel
    # and the header buttons, squeezing it to a sliver of 16px text.
    form = _slice(
        REPO_UI,
        "<form data-repo-issue-title-form",
        "</form>",
    )
    assert "basis-full flex-wrap" in form
    assert "sm:flex-1 sm:flex-nowrap" in form
    field = _slice(form, "<input data-repo-issue-title-input", "/>")
    assert "h-11" in field
    assert "text-[20px]" in field
    assert "basis-full" in field
    edit = _slice(
        WIRING,
        'const issueTitleEditButton = event.target.closest(',
        "return;",
    )
    assert 'issueTitleEditButton.classList.add("hidden")' in edit
