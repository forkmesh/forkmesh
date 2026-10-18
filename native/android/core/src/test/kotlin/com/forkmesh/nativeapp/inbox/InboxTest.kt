package com.forkmesh.nativeapp.inbox

import com.forkmesh.nativeapp.api.MemoryPrefStore
import com.forkmesh.nativeapp.model.ForkNotification
import com.forkmesh.nativeapp.model.OrgApiException
import com.forkmesh.nativeapp.navigation.ComposeTarget
import com.forkmesh.nativeapp.navigation.InboxFilter
import com.forkmesh.nativeapp.navigation.PullTab
import com.forkmesh.nativeapp.navigation.RepoRef
import com.forkmesh.nativeapp.navigation.RepoSection
import com.forkmesh.nativeapp.navigation.Route
import com.forkmesh.nativeapp.push.PushTarget
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import java.time.ZoneId

class InboxTest {
    private val repo = RepoRef("fm", "app")

    private class FakeClient : InboxClient {
        val calls = ArrayList<String>()
        var failWrites = false
        var failPings = false
        var tasksError: Exception? = null
        var pings: List<Map<String, Any?>> = emptyList()
        var tasks: List<Map<String, Any?>> = emptyList()
        var pauseUntil = 0L

        override fun call(method: String, path: String, body: Map<String, Any?>?): Map<String, Any?> {
            calls += "$method $path ${body ?: ""}".trim()
            if (method == "GET" && path.startsWith("/api/notifications")) {
                if (failPings) error("offline")
                return mapOf("notifications" to pings, "unread" to pings.size.toLong(), "pauseUntil" to pauseUntil)
            }
            if (method == "GET" && path == "/api/tasks") {
                tasksError?.let { throw it }
                return mapOf("actor" to "Me", "tasks" to tasks)
            }
            if (failWrites) error("offline")
            if (method == "DELETE") pings = if (body?.get("all") == true) emptyList() else pings.filter { it["id"] != body?.get("id") }
            if (method == "POST" && body?.get("ids") is List<*>) {
                val ids = body["ids"] as List<*>
                pings = pings.map { if (it["id"] in ids) it + ("readAt" to 1L) else it }
            }
            return if (body?.containsKey("pauseUntil") == true) mapOf("pauseUntil" to body["pauseUntil"]) else emptyMap()
        }
    }

    private fun ping(id: String, kind: String, ts: Long, read: Boolean = false, href: String = "", meta: Map<String, Any?> = emptyMap()) =
        mapOf("id" to id, "kind" to kind, "title" to "t$id", "repo" to "fm/app", "href" to href, "ts" to ts, "readAt" to if (read) 1L else 0L, "meta" to meta)

    fun testCategoriesAndFilters() {
        assertEq(InboxCategory.Approval, InboxCategory.of(" agent-approval "))
        assertEq(InboxCategory.Review, InboxCategory.of("pull_submitted"))
        assertEq(InboxCategory.Assigned, InboxCategory.of("organization_task_started"))
        assertEq(InboxCategory.Ping, InboxCategory.of("code_push"))
        expect(InboxCategory.Review.matches(InboxFilter.Reviews))
        expect(!InboxCategory.Review.matches(InboxFilter.Mentions))
    }

    fun testWebLinksFromPings() {
        val origin = "https://app.forkmesh.com"
        assertEq("https://app.forkmesh.com/dashboard/calendar?event=7", InboxRoutes.webLink("/dashboard/calendar?event=7", origin))
        assertEq("https://example.org/x", InboxRoutes.webLink(" https://example.org/x ", origin))
        assertEq(null, InboxRoutes.webLink("/api/notifications", origin))
        assertEq(null, InboxRoutes.webLink("/api", origin))
        assertEq(null, InboxRoutes.webLink("javascript:alert(1)", origin))
        assertEq(null, InboxRoutes.webLink("https://user@evil.example/", origin))
        assertEq(null, InboxRoutes.webLink("", origin))
    }

    fun testRoutesFromPings() {
        assertEq(Route.Approval("s1/a1"), InboxRoutes.route("agent_approval", "", mapOf("sessionId" to "s1", "approvalId" to "a1"), ""))
        assertEq(Route.Agent("s1"), InboxRoutes.route("agent_approval", "", mapOf("sessionId" to "s1"), ""))
        assertEq(Route.Agent("12"), InboxRoutes.route("desktop_ping", "/fm/app/agents/12", mapOf("sessionId" to "12"), "fm/app"))
        assertEq(Route.Issue(repo, 4), InboxRoutes.route("mention", "/fm/app/issues/4", emptyMap(), ""))
        assertEq(Route.Commit(repo, "abcdef1"), InboxRoutes.route("code_push", "/fm/app/commit/abcdef1", emptyMap(), ""))
        assertEq(Route.Commits(repo, "dev"), InboxRoutes.route("code_push", "/fm/app/commits/dev", emptyMap(), ""))
        assertEq(Route.LaunchedAgent(repo, "7"), InboxRoutes.route("agent_finished", "/fm/app/agents/7", emptyMap(), ""))
        assertEq(Route.Run(repo, "41"), InboxRoutes.route("desktop_ping", "/fm/app/actions/runs/41", mapOf("runId" to "41"), "fm/app"))
        assertEq(Route.Repo(repo, RepoSection.Actions), InboxRoutes.route("repo_activity", "/fm/app/actions", emptyMap(), ""))
        assertEq(Route.Pull(repo, 3, PullTab.Conversation), InboxRoutes.route("pull_submitted", "", mapOf("number" to 3L), "fm/app"))
        assertEq(Route.Repo(repo, RepoSection.Issues), InboxRoutes.route("issue_opened", "", emptyMap(), "fm/app"))
        assertEq(Route.Repo(repo, RepoSection.Releases), InboxRoutes.route("release_published", "", emptyMap(), "fm/app"))
        assertEq(null, InboxRoutes.route("x", "", emptyMap(), "bad"))
        assertEq("s" to "a", InboxRoutes.approvalParts("s/a"))
        assertEq(null, InboxRoutes.approvalParts("s/"))
    }

    fun testPushTargetCarriesTheDesktopAgent() {
        val done = ForkNotification(id = "p1", kind = "desktop_ping", title = "Agent finished", repo = "fm/app", href = "/fm/app/agents/12", meta = mapOf("sessionId" to "12", "category" to "agent-finished"))
        assertEq(PushTarget("proj", "p1", "/fm/app/agents/12", "12", done), PushTarget.forPing("proj", done))
        val bare = ForkNotification(id = "p2", kind = "desktop_ping", title = "Agent finished", meta = mapOf("sessionId" to "7"))
        assertEq("7", PushTarget.forPing("proj", bare).agent)
        val mention = ForkNotification(id = "p3", kind = "mention", title = "Look", href = "/fm/app/issues/1")
        assertEq("", PushTarget.forPing("proj", mention).agent)
        val relay = ForkNotification(id = "p4", kind = "agent_finished", title = "Done", href = "/fm/app/agents/7")
        assertEq("", PushTarget.forPing("proj", relay).agent)
        assertEq("7", PushTarget.from("proj", "", "", " 7 ")?.agent)
    }

    fun testFeedMergesPinsApprovalsAndDedupes() {
        val a = InboxItem("ping:1", InboxSource.Ping, InboxCategory.Ping, "x", "a", ts = 10)
        val b = InboxItem("ping:2", InboxSource.Ping, InboxCategory.Approval, "agent_approval", "b", ts = 1)
        val c = InboxItem("task:3", InboxSource.Task, InboxCategory.Assigned, "t", "c", ts = 20)
        val merged = InboxFeed.merge(listOf(a, b), listOf(c, c), emptyList())
        assertEq(listOf("ping:2", "task:3", "ping:1"), merged.map { it.id })
        assertEq(2, InboxFeed.filter(merged, InboxFilter.Pings).size)
        assertEq(1, InboxFeed.filter(merged, InboxFilter.Approvals).size)
        assertEq(3, InboxFeed.unreadCounts(merged)[InboxFilter.All])
        val task = InboxFeed.taskItem(mapOf("id" to "t1", "assigneeKind" to "user", "assignee" to "ME", "status" to "open", "updatedAt" to 5L, "createdAt" to 2L, "details" to "x".repeat(400)), "me")!!
        assertEq("task:t1@5", task.id)
        assertEq(280, task.body.length)
        assertEq(Route.Task("t1"), task.route)
        assertEq(null, InboxFeed.taskItem(mapOf("id" to "t1", "assigneeKind" to "user", "assignee" to "me", "status" to "done"), "me"))
        assertEq(null, InboxFeed.taskItem(mapOf("id" to "t1", "assigneeKind" to "team", "assignee" to "me"), "me"))
    }

    fun testModelLoadsMarksAndQueuesOfflineWrites() {
        val client = FakeClient()
        client.pings = listOf(ping("1", "mention", 5, href = "/fm/app/issues/1"), ping("2", "agent_approval", 1, meta = mapOf("sessionId" to "s", "approvalId" to "a")))
        client.tasks = listOf(mapOf("id" to "t", "assigneeKind" to "user", "assignee" to "me", "status" to "open", "updatedAt" to 3L))
        val prefs = MemoryPrefStore()
        val model = InboxModel(client, { "Me" }, prefs, nowMs = { 1000L })
        model.load()
        assertEq(listOf("ping:2", "ping:1", "task:t@3"), model.items.map { it.id })
        assertEq(3, model.unreadTotal)
        expect(client.calls.any { it.startsWith("GET /api/notifications?node=me&limit=100") })
        client.failWrites = true
        model.markRead(model.items.first { it.id == "ping:1" })
        assertEq(InboxSyncState.Pending, model.items.first { it.id == "ping:1" }.sync)
        assertEq(1, model.pendingSyncCount)
        model.delete(model.items.first { it.id == "ping:2" })
        assertEq(2, model.pendingSyncCount)
        model.markRead(model.items.first { it.source == InboxSource.Task })
        client.failWrites = false
        val reloaded = InboxModel(client, { "Me" }, prefs, nowMs = { 1000L })
        reloaded.load()
        assertEq(0, reloaded.pendingSyncCount)
        assertEq(listOf("ping:1", "task:t@3"), reloaded.items.map { it.id })
        expect(reloaded.items.none { it.unread })
    }

    fun testModelReportsErrorsAndTaskAccess() {
        val client = FakeClient()
        client.failPings = true
        client.tasksError = OrgApiException("org_member_required", "no")
        val model = InboxModel(client, { "me" }, MemoryPrefStore())
        model.load()
        assertEq(listOf("Pings could not be loaded."), model.errors)
        client.failPings = false
        client.tasksError = IllegalStateException("x")
        model.load()
        assertEq(listOf("Assigned tasks could not be loaded."), model.errors)
        val guest = InboxModel(client, { "" }, MemoryPrefStore())
        guest.load()
        assertEq(listOf("Sign in to see your inbox."), guest.errors)
    }

    fun testMarkAllReadAndPause() {
        val client = FakeClient()
        client.pings = listOf(ping("1", "mention", 5))
        var now = 0L
        val model = InboxModel(client, { "me" }, MemoryPrefStore(), nowMs = { now })
        model.load()
        model.markAllRead()
        assertEq(0, model.unreadTotal)
        expect(client.calls.last().contains("all=true"))
        assertEq(null, model.pause(PingPauseChoice.OneHour))
        assertEq(3_600_000L, model.pauseUntilMs)
        expect(model.isPaused)
        assertEq("Pings paused for 1 more hour", model.pauseLabel)
        now = 3_000_000L
        assertEq("Pings paused for 10 more minutes", model.pauseLabel)
        model.pause(PingPauseChoice.OneWeek)
        now = 3_000_000L
        assertEq("Pings paused for 7 more days", model.pauseLabel)
        client.failWrites = true
        assertEq("Pings could not be resumed.", model.pause(null))
    }

    fun testDeleteAllPingsClearsTasksToo() {
        val client = FakeClient()
        client.pings = listOf(ping("1", "mention", 5), ping("2", "code_push", 4))
        client.tasks = listOf(mapOf("id" to "t", "assigneeKind" to "user", "assignee" to "me", "status" to "open", "updatedAt" to 3L))
        val prefs = MemoryPrefStore()
        val local = LocalPingLog(prefs, { 0L })
        local.record("git", "Push failed", "no route")
        val model = InboxModel(client, { "me" }, prefs, localPings = local)
        model.load()
        expect(model.canDeleteAllPings)
        client.failWrites = true
        model.delete(model.items.first { it.id == "ping:2" })
        assertEq(1, model.pendingSyncCount)
        assertEq("Pings could not be deleted.", model.deleteAllPings())
        assertEq(3, model.items.size)
        client.failWrites = false
        var changed = 0
        model.onChanged = { changed++ }
        assertEq(null, model.deleteAllPings())
        expect(client.calls.last().startsWith("DELETE /api/notifications"))
        expect(client.calls.last().contains("all=true"))
        assertEq(emptyList<String>(), model.items.map { it.id })
        assertEq(0, model.pendingSyncCount)
        assertEq(0, PingSyncQueue(prefs).operations.size)
        assertEq(0, LocalPingLog(prefs).pings.size)
        assertEq(1, changed)
        expect(!model.canDeleteAllPings)
        model.load()
        assertEq(emptyList<String>(), model.items.map { it.id })
        assertEq(emptyList<String>(), InboxModel(client, { "me" }, prefs).also { it.load() }.items.map { it.id })
        client.tasks = listOf(mapOf("id" to "t", "assigneeKind" to "user", "assignee" to "me", "status" to "open", "updatedAt" to 9L))
        model.load()
        assertEq(listOf("task:t@9"), model.items.map { it.id })
        expect(model.canDeleteAllPings)
        val guestLocal = LocalPingLog(MemoryPrefStore())
        guestLocal.record("git", "Push failed", "no route")
        val guest = InboxModel(client, { "" }, MemoryPrefStore(), localPings = guestLocal)
        val before = client.calls.size
        assertEq(null, guest.deleteAllPings())
        assertEq(before, client.calls.size)
        assertEq(0, guestLocal.pings.size)
    }

    fun testPromptFromPingAndTask() {
        val ping = ForkNotification(id = "1", kind = "mention", title = "Look", body = "at this", repo = "fm/app", actor = "bob", href = "/fm/app/issues/1", ts = 0)
        val text = pingAgentPrompt(ping, ZoneId.of("UTC"))
        assertEq("ForkMesh ping (Mention)\nLook\n\nat this\n\nRepository: fm/app\nFrom: bob\nLink: /fm/app/issues/1", text)
        val model = InboxModel(FakeClient(), { "me" }, MemoryPrefStore())
        val task = InboxItem("task:1", InboxSource.Task, InboxCategory.Assigned, "t", "Do it", body = "now", repo = "fm/app")
        assertEq(ComposeTarget(repo = repo, text = "Do it\n\nnow"), model.prompt(task))
    }

    fun testReadStateCapsAndPersists() {
        val prefs = MemoryPrefStore()
        val state = InboxReadState(prefs)
        state.markRead((1..450).map { "id$it" })
        expect(!state.isRead("id1"))
        expect(state.isRead("id450"))
        expect(InboxReadState(prefs).isRead("id51"))
    }

    fun testSyncQueueAttemptsAndDeleteReplacesMarkRead() {
        val queue = PingSyncQueue(MemoryPrefStore())
        queue.enqueue(PingSyncOperation.Kind.MarkRead, "a", 1)
        queue.enqueue(PingSyncOperation.Kind.MarkRead, "a", 2)
        queue.enqueue(PingSyncOperation.Kind.Delete, "a", 3)
        queue.enqueue(PingSyncOperation.Kind.MarkRead, "", 3)
        assertEq(1, queue.operations.size)
        expect(queue.isDeleted("a"))
        repeat(5) { queue.drain { false } }
        assertEq(InboxSyncState.Failed, queue.state("a"))
        assertEq(0, queue.drain { true })
        queue.discardFailed()
        assertEq(null, queue.state("a"))
    }

    fun testLocalPingsRedactDedupeAndRateLimit() {
        var now = 0L
        var next = 0
        val log = LocalPingLog(MemoryPrefStore(), { now }, { "p${next++}" })
        val first = log.record("git", "Push failed", "Bearer abcdefghijkl token=supersecret at /data/user/0/com.x/files/repo")!!
        assertEq("Bearer <redacted> token=<redacted> at <path>", first.message)
        now = 5
        val again = log.record("git", "Push failed", "Bearer abcdefghijkl token=supersecret at /data/user/0/com.x/files/repo")!!
        assertEq(first.id, again.id)
        assertEq(1, log.pings.size)
        repeat(10) { log.record("s", "t$it", "m") }
        assertEq(LocalPingLog.MAX_PER_HOUR, log.pings.size)
        assertEq(null, log.record("s", "  ", "m"))
        log.markAllRead()
        expect(log.items.none { it.unread })
        log.remove(first.id)
        assertEq(LocalPingLog.MAX_PER_HOUR - 1, log.pings.size)
        assertEq(InboxSyncState.LocalOnly, log.items.first().sync)
    }

    fun testNotificationCategoriesAndActions() {
        assertEq(NotificationCategoryId.Dm, NotificationCategoryId.forPing("mention", "dm"))
        assertEq(NotificationCategoryId.AgentFailed, NotificationCategoryId.forPing("agent_failed"))
        assertEq(NotificationCategoryId.CiFailed, NotificationCategoryId.forPing("action_failed"))
        expect(NotificationActionId.Approve.requiresAuthentication)
        expect(NotificationActionId.Approve.opensApp)
        expect(!NotificationActionId.MarkRead.opensApp)
        expect(NotificationActionId.Reply.isTextInput)
        val approval = NotificationPayload.from("agent-approval", mapOf("sessionId" to "s", "approvalId" to "a", "alertId" to "9"))
        assertEq(Route.Approval("s/a"), NotificationActionRouter.route(approval))
        assertEq(NotificationCommand.Approve("s", "a"), NotificationActionRouter.command("fm.approve", approval, null))
        assertEq(NotificationCommand.Deny("s", "a"), NotificationActionRouter.command("fm.deny", approval, null))
        assertEq(NotificationCommand.MarkRead("9"), NotificationActionRouter.command("fm.markRead", approval, null))
        assertEq(NotificationCommand.Open(Route.Approval("s/a"), ""), NotificationActionRouter.command(null, approval, null))
        val dm = NotificationPayload.from(null, mapOf("kind" to "x", "topic" to "dm", "channel" to "bob"))
        assertEq(Route.Dm("bob"), NotificationActionRouter.route(dm))
        assertEq(NotificationCommand.Reply("bob", "hi"), NotificationActionRouter.command("fm.reply", dm, "  hi "))
        assertEq(NotificationCommand.Ignore("empty reply"), NotificationActionRouter.command("fm.reply", dm, " "))
        val ci = NotificationPayload.from("ci-failed", mapOf("repo" to "fm/app", "runId" to "r1"))
        assertEq(Route.Run(repo, "r1"), NotificationActionRouter.route(ci))
        val sent = NotificationActionRouter.command("fm.sendToAgent", ci, null) as NotificationCommand.Compose
        assertEq("A CI run failed in fm/app. Find the cause and fix it.", sent.target.text)
        val pr = NotificationPayload.from("pr-review", mapOf("repo" to "fm/app", "number" to "5"))
        assertEq(NotificationCommand.ApprovePull(repo, 5), NotificationActionRouter.command("fm.approvePull", pr, null))
        assertEq(Route.Pull(repo, 5, PullTab.Conversation), NotificationActionRouter.route(pr))
        assertEq(Route.Inbox(InboxFilter.All), NotificationActionRouter.route(NotificationPayload(NotificationCategoryId.Ping)))
        assertEq(null, NotificationActionRouter.route(NotificationPayload(NotificationCategoryId.AgentFinished)))
        val finished = NotificationPayload(NotificationCategoryId.AgentFinished, sessionId = "s")
        assertEq(NotificationCommand.ContinueRun("s", "go"), NotificationActionRouter.command("fm.continue", finished, "go"))
        assertEq(NotificationCommand.Merge("s"), NotificationActionRouter.command("fm.merge", finished, null))
        assertEq(NotificationCommand.Ignore("no session"), NotificationActionRouter.command("fm.retry", NotificationPayload(NotificationCategoryId.AgentFailed), null))
        val round = NotificationPayload.from(null, finished.fields("agent_finished"))
        assertEq(finished, round)
    }
}
