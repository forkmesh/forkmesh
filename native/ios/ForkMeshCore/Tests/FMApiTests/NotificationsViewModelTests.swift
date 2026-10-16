// The notifications screen's logic (native/android NotificationsLogicTest.kt), including the
// ping-to-agent prompt and repository choice.

import FMBase
import XCTest
@testable import FMApi

private let items = [
    ForkNotification(
        id: "n1", kind: "mention", title: "You were mentioned in mona/forkmesh",
        body: "Please review @mona before the desktop owner applies it.", repo: "mona/forkmesh",
        href: "/mona/forkmesh/issues/12", actor: "kai", source: "issue", ts: 1_770_000_000_000),
    ForkNotification(
        id: "n2", kind: "host_offline", title: "Host offline for mona/forkmesh",
        body: "No live desktop host has checked in recently.", repo: "mona/forkmesh", source: "host",
        ts: 1_769_990_000_000, readAt: 1_769_995_000_000),
]

private let utc = TimeZone(identifier: "UTC")!

final class NotificationsViewModelTests: XCTestCase {
    func testEveryNotificationKindHasARelatedMobileIcon() {
        let expected: [String: String] = [
            "mention": "alternate_email_rounded",
            "subscribed": "notifications_active_outlined",
            "issue_opened": "adjust",
            "pull_submitted": "call_merge_rounded",
            "issue_assigned": "assignment_ind_outlined",
            "repo_shared": "folder_special_outlined",
            "bounty_funded": "payments_outlined",
            "bounty_paid": "payments_outlined",
            "release_published": "new_releases_outlined",
            "host_online": "cloud_done_outlined",
            "host_offline": "cloud_off_outlined",
            "credits_refilled": "account_balance_wallet_outlined",
            "pending_inbox": "inbox_outlined",
            "mirror_request": "sync_alt",
            "pending_reward": "account_balance_wallet_outlined",
            "org_succession": "account_tree_outlined",
            "repository_hosted": "cloud_done_outlined",
            "organization_task_started": "check_circle_outline",
            "organization_task_activity": "check_circle_outline",
            "organization_task_update": "check_circle_outline",
            "calendar_invitation": "calendar_today_outlined",
            "calendar_reminder": "calendar_today_outlined",
            "calendar_rsvp": "calendar_today_outlined",
            "chat_webhook": "chat_bubble_outline",
            "account_email_sent": "mark_email_read_outlined",
            "operational_alert": "warning_amber_outlined",
            "tip_sent": "payments_outlined",
            "error_group": "error_outline",
            "plugin_activity": "extension_outlined",
            "timer_finished": "timer_outlined",
            "desktop_ping": "desktop_mac_outlined",
            "repo_activity": "commit_outlined",
            "code_push": "commit_outlined",
            "security_alert": "gpp_maybe_outlined",
            "device_added": "smartphone_outlined",
            "hackathon_squad": "groups_outlined",
            "hackathon_reward": "card_giftcard_outlined",
            "badge": "emoji_events_outlined",
            "package_review": "rate_review_outlined",
        ]
        for (kind, icon) in expected {
            XCTAssertEqual(notificationIconForKind(kind), icon, kind)
        }
        XCTAssertEqual(notificationIconForKind(" Mention "), "alternate_email_rounded")
        XCTAssertEqual(notificationIconForKind("a_future_kind"), notificationLineIcon)
    }

    func testTheListShowsEveryUpdateWithItsLabels() {
        XCTAssertEqual(notificationFilters, ["All", "Unread", "Mentions", "Repo", "System"])
        let body = notificationsBody(loading: false, error: "", all: items, filter: "All")
        XCTAssertEqual(body.stateKey, "list-All")
        guard case .items(_, let shown) = body else { return XCTFail("expected items") }
        XCTAssertEqual(shown.count, 2)
        XCTAssertEqual(notificationsUnreadLabel(items.filter(\.isUnread).count), "1 unread")
        XCTAssertEqual(notificationsUpdatesLabel(2), "2 updates")
        XCTAssertEqual(notificationsUpdatesLabel(1), "1 update")
        XCTAssertEqual(notificationPills(items[0]), ["Mention", "mona/forkmesh", "@kai"])
        XCTAssertEqual(notificationPills(items[1]), ["Host", "mona/forkmesh"])
        XCTAssertEqual(notificationContextLabel(items[0]), "Open context: /mona/forkmesh/issues/12")
        XCTAssertNil(notificationContextLabel(items[1]))
        var bare = items[1]
        bare.body = ""
        XCTAssertEqual(notificationPreview(bare), "Open context in ForkMesh to review this update.")
    }

    func testFiltersUnreadAndGroups() {
        XCTAssertEqual(filterNotifications(items, "Unread").map(\.id), ["n1"])
        XCTAssertEqual(filterNotifications(items, "Mentions").map(\.id), ["n1"])
        XCTAssertEqual(filterNotifications(items, "System").map(\.id), ["n2"])
        XCTAssertEqual(filterNotifications(items, "Repo").map(\.id), [])
    }

    func testMarkingAllReadLeavesTheUnreadFilterCaughtUp() {
        let read = items.map { $0.copyWith(readAt: 1_770_001_000_000) }
        XCTAssertFalse(canMarkAllRead(read.filter(\.isUnread).count))
        XCTAssertEqual(notificationsUnreadLabel(0), "0 unread")
        let body = notificationsBody(loading: false, error: "", all: read, filter: "Unread")
        XCTAssertEqual(body.stateKey, "empty-Unread")
        let unread = NotificationsEmptyCopy(filter: "Unread")
        XCTAssertEqual(unread.title, "All caught up")
        XCTAssertEqual(unread.icon, "mark_email_read_outlined")
        XCTAssertEqual(notificationsBody(loading: false, error: "", all: read, filter: "Repo").stateKey, "empty-Repo")
        let repo = NotificationsEmptyCopy(filter: "Repo")
        XCTAssertEqual(repo.title, "No repo notifications")
        XCTAssertEqual(repo.icon, notificationLineIcon)
        XCTAssertTrue(repo.message.hasPrefix("Mainnode-backed ForkMesh updates"))
    }

    func testSkeletonAndErrorOnlyWithoutCachedItems() {
        XCTAssertEqual(notificationsBody(loading: true, error: "", all: [], filter: "All").stateKey, "loading")
        XCTAssertEqual(notificationsBody(loading: true, error: "", all: items, filter: "All").stateKey, "list-All")
        XCTAssertEqual(
            notificationsBody(loading: false, error: "Exception: offline", all: [], filter: "All"),
            .error("Exception: offline"))
        XCTAssertEqual(
            notificationsBody(loading: false, error: "Exception: offline", all: items, filter: "All").stateKey, "list-All")
    }

    func testTimeLabelIsLocalTwelveHourClock() {
        XCTAssertEqual(notificationTimeLabel(1_770_000_000_000, timeZone: utc), "2:40 AM")
        XCTAssertEqual(notificationTimeLabel(1_770_040_800_000, timeZone: utc), "2:00 PM")
        XCTAssertEqual(notificationTimeLabel(12 * 3_600_000 + 30 * 60_000, timeZone: utc), "12:30 PM")
        XCTAssertEqual(notificationTimeLabel(0, timeZone: utc), "")
    }

    func testPingDetailWhenNamesTheDayAndTwelveHourTime() {
        XCTAssertEqual(pingDetailWhen(1_770_000_000_000, timeZone: utc), "Mon, Feb 2, 2026 · 2:40 AM")
        XCTAssertEqual(pingDetailWhen(1_770_040_800_000, timeZone: utc), "Mon, Feb 2, 2026 · 2:00 PM")
        XCTAssertEqual(pingDetailWhen(0, timeZone: utc), "")
    }

    func testDeleteAllCopyQuotesTheWholeInbox() {
        XCTAssertEqual(notificationsDeleteAllTitle(137), "Delete all 137 notifications?")
        XCTAssertEqual(notificationsDeleteAllTitle(1), "Delete 1 notification?")
        XCTAssertEqual(notificationsDeleteAllTitle(0), "Delete all 0 notifications?")
        XCTAssertTrue(NotificationsCopy.deleteAllBody.hasSuffix("This cannot be undone."))
        XCTAssertTrue(canDeleteAllNotifications(1))
        XCTAssertFalse(canDeleteAllNotifications(0))
        XCTAssertEqual(
            notificationsDeleteErrorText(ApiException("nope")), "Could not delete notifications: Exception: nope")
        XCTAssertEqual(notificationsDeleteErrorText(nil), "Could not delete notifications: unknown error")
    }

    func testTappingAnIssueNotificationOpensTheExactIssue() throws {
        let link = try XCTUnwrap(notificationTapLink(items[0]))
        XCTAssertEqual(link.repo.owner, "mona")
        XCTAssertEqual(link.repo.name, "forkmesh")
        XCTAssertEqual(link.initialTab, .issues)
        XCTAssertEqual(link.number, 12)
        XCTAssertNil(NotificationDeepLink.parse(items[1].href))
        // No link at all: the repository the ping mentions.
        let repoOnly = try XCTUnwrap(notificationTapLink(items[1]))
        XCTAssertEqual(repoOnly.repo.name, "forkmesh")
        // A link this phone cannot open is not second-guessed.
        var web = items[1]
        web.href = "https://example.com/elsewhere"
        XCTAssertNil(notificationTapLink(web))
    }

    func testPingAgentPromptListsOnlyWhatThePingHas() {
        XCTAssertEqual(
            pingAgentPrompt(items[0], timeZone: utc),
            """
            ForkMesh ping (Mention · 2026-02-02 02:40)
            You were mentioned in mona/forkmesh

            Please review @mona before the desktop owner applies it.

            Repository: mona/forkmesh
            From: kai
            Link: /mona/forkmesh/issues/12
            """)
        let bare = ForkNotification(id: "x", kind: "", title: "Hello")
        XCTAssertEqual(pingAgentPrompt(bare, timeZone: utc), "ForkMesh ping (Notification)\nHello")
    }

    func testPingDetailListsEveryFactThePingHas() {
        var ping = items[0]
        ping.meta = ["prNumber": 12.0, "check_state": "failing", "nested": ["x": 1], "blank": " "]
        XCTAssertEqual(
            pingDetailFacts(ping, now: 1_770_000_000_000 + 3 * 3_600_000, timeZone: utc),
            [
                PingDetailFact("Kind", "Mention"),
                PingDetailFact("Received", "Mon, Feb 2, 2026 · 2:40 AM · 3 h ago"),
                PingDetailFact("Status", "Unread"),
                PingDetailFact("Repository", "mona/forkmesh"),
                PingDetailFact("From", "@kai"),
                PingDetailFact("Source", "issue"),
                PingDetailFact("Link", "/mona/forkmesh/issues/12"),
                PingDetailFact("Check state", "failing"),
                PingDetailFact("Pr number", "12"),
                PingDetailFact("ID", "n1"),
            ])
        let read = pingDetailFacts(items[1], now: 1_770_000_000_000, timeZone: utc)
        XCTAssertEqual(read.first { $0.label == "Status" }, PingDetailFact("Status", "Read · Mon, Feb 2, 2026 · 1:16 AM"))
        XCTAssertFalse(read.contains { $0.label == "From" || $0.label == "Link" })
    }

    func testPingAgeLabel() {
        let now = 1_770_000_000_000
        XCTAssertEqual(pingAgeLabel(now - 5_000, now: now), "just now")
        XCTAssertEqual(pingAgeLabel(now - 12 * 60_000, now: now), "12 min ago")
        XCTAssertEqual(pingAgeLabel(now - 2 * 86_400_000, now: now), "2 d ago")
        XCTAssertEqual(pingAgeLabel(now - 40 * 86_400_000, now: now), "")
        XCTAssertEqual(pingAgeLabel(0, now: now), "")
    }

    func testPingOpenLabelNamesWhatTheLinkOpens() throws {
        let issue = try XCTUnwrap(NotificationDeepLink.parse("/mona/forkmesh/issues/12"))
        let pull = try XCTUnwrap(NotificationDeepLink.parse("/mona/forkmesh/pulls/3"))
        let repo = try XCTUnwrap(NotificationDeepLink.parse("/mona/forkmesh"))
        XCTAssertEqual(pingOpenLabel(.repo(issue)), "Open issue #12")
        XCTAssertEqual(pingOpenLabel(.repo(pull)), "Open pull request #3")
        XCTAssertEqual(pingOpenLabel(.repo(repo)), "Open mona/forkmesh")
        XCTAssertEqual(pingOpenLabel(.web(URL(string: "https://forkmesh.com/pricing")!)), "Open in browser")
        XCTAssertNil(pingOpenLabel(.context))
    }

    func testPingNamesLocalRepo() {
        XCTAssertTrue(pingNamesLocalRepo("mona/forkmesh", name: "anything", publishedAs: "Mona/ForkMesh"))
        XCTAssertFalse(pingNamesLocalRepo("mona/forkmesh", name: "forkmesh", publishedAs: "mona/other"))
        XCTAssertTrue(pingNamesLocalRepo("mona/forkmesh", name: "ForkMesh", publishedAs: nil))
        XCTAssertTrue(pingNamesLocalRepo("mona/forkmesh", name: "forkmesh", publishedAs: " "))
        XCTAssertFalse(pingNamesLocalRepo(" ", name: "forkmesh", publishedAs: nil))
    }

    func testPingRepoChoice() {
        let a = PingRepoCandidate(name: "forkmesh", publishedAs: "mona/forkmesh")
        let b = PingRepoCandidate(name: "notes", publishedAs: nil)
        let c = PingRepoCandidate(name: "forkmesh", publishedAs: nil)
        XCTAssertEqual(pingRepoChoice("mona/forkmesh", candidates: [], runningRepo: nil), .copy)
        XCTAssertEqual(pingRepoChoice("mona/forkmesh", candidates: [b, a], runningRepo: nil), .open(1))
        XCTAssertEqual(pingRepoChoice("x/y", candidates: [a, b], runningRepo: "notes"), .open(1))
        XCTAssertEqual(pingRepoChoice("x/y", candidates: [b], runningRepo: nil), .open(0))
        XCTAssertEqual(pingRepoChoice("x/y", candidates: [a, b], runningRepo: nil), .pick([0, 1]))
        XCTAssertEqual(pingRepoChoice("mona/forkmesh", candidates: [b, a, c], runningRepo: nil), .pick([1, 2, 0]))
        XCTAssertEqual(pingRepoSubtitle(b, runningRepo: "notes"), "Agent running — the ping steers it")
        XCTAssertEqual(pingRepoSubtitle(b, runningRepo: nil), "Only on this phone")
        XCTAssertEqual(pingRepoSubtitle(a, runningRepo: nil), "mona/forkmesh")
    }
}
