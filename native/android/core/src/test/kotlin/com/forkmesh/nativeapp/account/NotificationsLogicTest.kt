package com.forkmesh.nativeapp.account

import com.forkmesh.nativeapp.model.ForkNotification
import com.forkmesh.nativeapp.model.NotificationDeepLink
import com.forkmesh.nativeapp.model.RepoDetailTab
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import java.time.ZoneOffset

class NotificationsLogicTest {
    private val items = listOf(
        ForkNotification(
            id = "n1",
            kind = "mention",
            title = "You were mentioned in mona/forkmesh",
            body = "Please review @mona before the desktop owner applies it.",
            repo = "mona/forkmesh",
            href = "/mona/forkmesh/issues/12",
            actor = "kai",
            source = "issue",
            ts = 1770000000000,
        ),
        ForkNotification(
            id = "n2",
            kind = "host_offline",
            title = "Host offline for mona/forkmesh",
            body = "No live desktop host has checked in recently.",
            repo = "mona/forkmesh",
            source = "host",
            ts = 1769990000000,
            readAt = 1769995000000,
        ),
    )

    fun testEveryNotificationKindHasARelatedMobileIcon() {
        val expected = mapOf(
            "mention" to "alternate_email_rounded",
            "subscribed" to "notifications_active_outlined",
            "issue_opened" to "adjust",
            "pull_submitted" to "call_merge_rounded",
            "issue_assigned" to "assignment_ind_outlined",
            "repo_shared" to "folder_special_outlined",
            "bounty_funded" to "payments_outlined",
            "bounty_paid" to "payments_outlined",
            "release_published" to "new_releases_outlined",
            "host_online" to "cloud_done_outlined",
            "host_offline" to "cloud_off_outlined",
            "credits_refilled" to "account_balance_wallet_outlined",
            "pending_inbox" to "inbox_outlined",
            "mirror_request" to "sync_alt",
            "pending_reward" to "account_balance_wallet_outlined",
            "org_succession" to "account_tree_outlined",
            "repository_hosted" to "cloud_done_outlined",
            "organization_task_started" to "check_circle_outline",
            "organization_task_activity" to "check_circle_outline",
            "organization_task_update" to "check_circle_outline",
            "calendar_invitation" to "calendar_today_outlined",
            "calendar_reminder" to "calendar_today_outlined",
            "calendar_rsvp" to "calendar_today_outlined",
            "chat_webhook" to "chat_bubble_outline",
            "account_email_sent" to "mark_email_read_outlined",
            "operational_alert" to "warning_amber_outlined",
            "tip_sent" to "payments_outlined",
            "error_group" to "error_outline",
            "plugin_activity" to "extension_outlined",
            "timer_finished" to "timer_outlined",
            "desktop_ping" to "desktop_windows_outlined",
            "repo_activity" to "commit_outlined",
            "code_push" to "commit_outlined",
            "security_alert" to "gpp_maybe_outlined",
            "device_added" to "smartphone_outlined",
            "hackathon_squad" to "groups_outlined",
            "hackathon_reward" to "card_giftcard_outlined",
            "package_review" to "rate_review_outlined",
        )
        for ((kind, icon) in expected) assertEq(icon, notificationIconForKind(kind), kind)
        assertEq("alternate_email_rounded", notificationIconForKind(" Mention "))
        assertEq(NOTIFICATION_LINE_ICON, notificationIconForKind("a_future_kind"))
    }

    fun testTheListShowsEveryUpdateWithItsLabels() {
        assertEq(listOf("All", "Unread", "Mentions", "Repo", "System"), NOTIFICATION_FILTERS)
        val body = notificationsBody(loading = false, error = "", all = items, filter = "All")
        expect(body is NotificationsBody.Items)
        assertEq("list-All", body.stateKey)
        assertEq(2, (body as NotificationsBody.Items).items.size)
        assertEq("1 unread", notificationsUnreadLabel(items.count { it.isUnread }))
        assertEq("2 updates", notificationsUpdatesLabel(2))
        assertEq("1 update", notificationsUpdatesLabel(1))
        assertEq(listOf("Mention", "mona/forkmesh", "@kai"), notificationPills(items[0]))
        assertEq(listOf("Host", "mona/forkmesh"), notificationPills(items[1]))
        assertEq("Open context: /mona/forkmesh/issues/12", notificationContextLabel(items[0]))
        assertEq(null, notificationContextLabel(items[1]))
        assertEq("Open context in ForkMesh to review this update.", notificationPreview(items[1].copy(body = "")))
    }

    fun testFiltersUnreadAndGroups() {
        assertEq(listOf("n1"), filterNotifications(items, "Unread").map { it.id })
        assertEq(listOf("n1"), filterNotifications(items, "Mentions").map { it.id })
        assertEq(listOf("n2"), filterNotifications(items, "System").map { it.id })
        assertEq(emptyList<String>(), filterNotifications(items, "Repo").map { it.id })
    }

    fun testMarkingAllReadLeavesTheUnreadFilterCaughtUp() {
        val read = items.map { it.copyWith(readAt = 1770001000000) }
        expect(!canMarkAllRead(read.count { it.isUnread }))
        assertEq("0 unread", notificationsUnreadLabel(0))
        val body = notificationsBody(loading = false, error = "", all = read, filter = "Unread")
        assertEq("empty-Unread", body.stateKey)
        val empty = body as NotificationsBody.Empty
        assertEq("All caught up", empty.title)
        assertEq("mark_email_read_outlined", empty.icon)
        val repo = notificationsBody(loading = false, error = "", all = read, filter = "Repo") as NotificationsBody.Empty
        assertEq("No repo notifications", repo.title)
        assertEq(NOTIFICATION_LINE_ICON, repo.icon)
        expect(repo.message.startsWith("Mainnode-backed ForkMesh updates"))
    }

    fun testSkeletonAndErrorOnlyWithoutCachedItems() {
        assertEq("loading", notificationsBody(loading = true, error = "", all = emptyList(), filter = "All").stateKey)
        assertEq("list-All", notificationsBody(loading = true, error = "", all = items, filter = "All").stateKey)
        val error = notificationsBody(loading = false, error = "Exception: offline", all = emptyList(), filter = "All")
        assertEq(NotificationsBody.Error("Exception: offline"), error)
        assertEq("list-All", notificationsBody(loading = false, error = "Exception: offline", all = items, filter = "All").stateKey)
    }

    fun testTimeLabelIsLocalTwelveHourClock() {
        assertEq("2:40 AM", notificationTimeLabel(1770000000000, ZoneOffset.UTC))
        assertEq("2:00 PM", notificationTimeLabel(1770040800000, ZoneOffset.UTC))
        assertEq("12:30 PM", notificationTimeLabel(12 * 3_600_000L + 30 * 60_000L, ZoneOffset.UTC))
        assertEq("", notificationTimeLabel(0, ZoneOffset.UTC))
    }

    fun testPingDetailWhenNamesTheDayAndTwelveHourTime() {
        assertEq("Mon, Feb 2, 2026 · 2:40 AM", pingDetailWhen(1770000000000, ZoneOffset.UTC))
        assertEq("Mon, Feb 2, 2026 · 2:00 PM", pingDetailWhen(1770040800000, ZoneOffset.UTC))
        assertEq("", pingDetailWhen(0, ZoneOffset.UTC))
    }

    fun testDeleteAllCopyQuotesTheWholeInbox() {
        assertEq("Delete all 137 notifications?", notificationsDeleteAllTitle(137))
        assertEq("Delete 1 notification?", notificationsDeleteAllTitle(1))
        assertEq("Delete all 0 notifications?", notificationsDeleteAllTitle(0))
        expect(NOTIFICATIONS_DELETE_ALL_BODY.endsWith("This cannot be undone."))
        expect(canDeleteAllNotifications(1))
        expect(!canDeleteAllNotifications(0))
        assertEq(
            "Could not delete notifications: java.lang.IllegalStateException: nope",
            notificationsDeleteErrorText(IllegalStateException("nope")),
        )
    }

    fun testTappingAnIssueNotificationOpensTheExactIssue() {
        val link = NotificationDeepLink.parse(items[0].href) ?: throw AssertionError("no link")
        assertEq("mona", link.repo.owner)
        assertEq("forkmesh", link.repo.name)
        assertEq(RepoDetailTab.Issues, link.initialTab)
        assertEq(12, link.number)
        assertEq(null, NotificationDeepLink.parse(items[1].href))
    }

    fun testPingDetailListsEveryFactThePingHas() {
        val ping = items[0].copy(meta = mapOf("prNumber" to 12.0, "check_state" to "failing", "nested" to mapOf("x" to 1), "blank" to " "))
        val facts = pingDetailFacts(ping, now = 1770000000000 + 3 * 3600_000, zone = ZoneOffset.UTC)
        assertEq(
            listOf(
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
            ),
            facts,
        )
        val read = pingDetailFacts(items[1], now = 1770000000000, zone = ZoneOffset.UTC)
        assertEq(PingDetailFact("Status", "Read · Mon, Feb 2, 2026 · 1:16 AM"), read.first { it.label == "Status" })
        expect(read.none { it.label == "From" || it.label == "Link" })
    }

    fun testPingAgeLabel() {
        val now = 1770000000000
        assertEq("just now", pingAgeLabel(now - 5_000, now))
        assertEq("12 min ago", pingAgeLabel(now - 12 * 60_000, now))
        assertEq("2 d ago", pingAgeLabel(now - 2 * 86_400_000L, now))
        assertEq("", pingAgeLabel(now - 40 * 86_400_000L, now))
        assertEq("", pingAgeLabel(0, now))
    }

    fun testPingOpenLabelNamesWhatTheLinkOpens() {
        assertEq("Open issue #12", pingOpenLabel(NotificationDeepLink.parse("/mona/forkmesh/issues/12"), null))
        assertEq("Open pull request #3", pingOpenLabel(NotificationDeepLink.parse("/mona/forkmesh/pulls/3"), null))
        assertEq("Open mona/forkmesh", pingOpenLabel(NotificationDeepLink.parse("/mona/forkmesh"), null))
        assertEq("Open in browser", pingOpenLabel(null, pingWebLink("https://forkmesh.com/pricing")))
        assertEq(null, pingOpenLabel(null, pingWebLink("javascript:alert(1)")))
        assertEq(null, pingWebLink("https://user@evil.example/"))
        assertEq(null, pingWebLink("/settings"))
    }

    fun testPingAgentPromptListsOnlyWhatThePingHas() {
        assertEq(
            "ForkMesh ping (Mention · 2026-02-02 02:40)\nYou were mentioned in mona/forkmesh\n\n" +
                "Please review @mona before the desktop owner applies it.\n\n" +
                "Repository: mona/forkmesh\nFrom: kai\nLink: /mona/forkmesh/issues/12",
            pingAgentPrompt(items[0], ZoneOffset.UTC),
        )
        assertEq("ForkMesh ping (Notification)\nHi", pingAgentPrompt(ForkNotification(id = "", kind = "x", title = "Hi")))
    }
}
