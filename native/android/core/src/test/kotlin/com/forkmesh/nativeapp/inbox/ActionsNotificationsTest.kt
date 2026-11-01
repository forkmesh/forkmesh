package com.forkmesh.nativeapp.inbox

import com.forkmesh.nativeapp.model.ForkNotification
import com.forkmesh.nativeapp.navigation.PullTab
import com.forkmesh.nativeapp.navigation.RepoRef
import com.forkmesh.nativeapp.navigation.Route
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class ActionsNotificationsTest {
    private val repo = RepoRef("fm", "app")
    private val head = "0123456789abcdef0123456789abcdef01234567"

    fun testApprovalCategoryAndActions() {
        assertEq(NotificationCategoryId.ActionsApproval, NotificationCategoryId.forPing("action_approval"))
        assertEq(NotificationCategoryId.ActionsApproval, NotificationCategoryId.of("actions-approval"))
        assertEq(NotificationCategoryId.CiFailed, NotificationCategoryId.forPing("action_failed"))
        assertEq(NotificationCategoryId.CiFailed, NotificationCategoryId.of("ci-failed"))
        assertEq(listOf(NotificationActionId.Approve, NotificationActionId.Open), NotificationCategoryId.ActionsApproval.actions)
        expect(NotificationCategoryId.ActionsApproval.isTimeSensitive)
        expect(NotificationActionId.Approve.requiresAuthentication)
        expect(NotificationActionId.Approve.opensApp)
    }

    fun testApprovalPayloadRoutesAndCommands() {
        val fields = mapOf("repo" to "fm/app", "number" to "7", "head" to head, "runId" to "7000001", "href" to "/fm/app/pull/7/checks", "alertId" to "p1")
        val payload = NotificationPayload.from("actions-approval", fields)
        assertEq(7, payload.number)
        assertEq(head, payload.head)
        assertEq("fm/app", payload.repo)
        assertEq("7000001", payload.runId)
        assertEq(Route.Pull(repo, 7, PullTab.Checks), NotificationActionRouter.route(payload))
        assertEq(NotificationCommand.ApproveChecks(repo, 7, head), NotificationActionRouter.command("fm.approve", payload, null))
        assertEq(NotificationCommand.Open(Route.Pull(repo, 7, PullTab.Checks), "/fm/app/pull/7/checks"), NotificationActionRouter.command("fm.open", payload, null))
        assertEq(NotificationCommand.Open(Route.Pull(repo, 7, PullTab.Checks), "/fm/app/pull/7/checks"), NotificationActionRouter.command(null, payload, null))
        val noHead = payload.copy(head = "")
        assertEq(NotificationCommand.Open(Route.Pull(repo, 7, PullTab.Checks), "/fm/app/pull/7/checks"), NotificationActionRouter.command("fm.approve", noHead, null))
        assertEq(NotificationCommand.Ignore("no pull request"), NotificationActionRouter.command("fm.approve", payload.copy(repo = ""), null))
        assertEq(payload, NotificationPayload.from(null, payload.fields("action_approval")))
        assertEq(head, payload.fields("action_approval")["head"])
        val byKind = NotificationPayload.from(null, mapOf("kind" to "action_approval", "repo" to "fm/app", "number" to "7", "head" to head))
        assertEq(NotificationCategoryId.ActionsApproval, byKind.category)
    }

    fun testFailedRunOpensTheRun() {
        val payload = NotificationPayload.from("ci-failed", mapOf("repo" to "fm/app", "runId" to "1000001", "number" to "7", "href" to "/fm/app/actions/runs/1000001"))
        assertEq(Route.Run(repo, "1000001"), NotificationActionRouter.route(payload))
        assertEq(NotificationCommand.Open(Route.Run(repo, "1000001"), "/fm/app/actions/runs/1000001"), NotificationActionRouter.command("fm.openLog", payload, null))
    }

    fun testInboxRowsRouteToThePullAndTheRun() {
        val meta = mapOf("category" to "actions-approval", "number" to 7L, "head" to head, "runId" to 7000001L, "reason" to "author", "author" to "bob")
        assertEq(Route.Pull(repo, 7, PullTab.Checks), InboxRoutes.route("action_approval", "/fm/app/pull/7", meta, "fm/app"))
        assertEq(Route.Pull(repo, 7, PullTab.Conversation), InboxRoutes.route("action_approval", "/fm/app/pull/7", meta, ""))
        assertEq(Route.Pull(repo, 7, PullTab.Checks), InboxRoutes.route("action_approval", "/fm/app/pull/7/checks", meta, ""))
        assertEq(Route.Pull(repo, 7, PullTab.Checks), InboxRoutes.route("action_approval", "/fm/app/pull/7/checks", meta, "fm/app"))
        assertEq(Route.Pull(repo, 7, PullTab.Checks), InboxRoutes.route("pull_submitted", "/fm/app/pull/7/checks", emptyMap(), ""))
        assertEq(Route.Pull(repo, 7, PullTab.Conversation), InboxRoutes.route("pull_submitted", "/fm/app/pull/7/unknown", emptyMap(), ""))
        val link = com.forkmesh.nativeapp.model.NotificationDeepLink.parse("/fm/app/pull/7/checks")!!
        assertEq(com.forkmesh.nativeapp.model.RepoDetailTab.Pulls, link.initialTab)
        assertEq(7, link.number)
        assertEq("checks", link.section)
        val failed = mapOf("category" to "ci-failed", "runId" to 1000001L, "workflow" to "CI")
        assertEq(Route.Run(repo, "1000001"), InboxRoutes.route("action_failed", "/fm/app/actions/runs/1000001", failed, "fm/app"))
        assertEq(Route.Run(repo, "1000001"), InboxRoutes.route("action_failed", "", mapOf("runId" to 1000001.0), "fm/app"))
        val ping = ForkNotification(id = "p9", kind = "action_approval", title = "Checks need your approval", repo = "fm/app", href = "/fm/app/pull/7/checks", meta = meta)
        assertEq(Route.Pull(repo, 7, PullTab.Checks), InboxItem.fromPing(ping).route)
    }
}
