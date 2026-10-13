package com.forkmesh.nativeapp.inbox

import com.forkmesh.nativeapp.api.MemoryPrefStore
import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.json.asJsonMap
import com.forkmesh.nativeapp.model.ForkNotification
import com.forkmesh.nativeapp.model.NotificationPage
import com.forkmesh.nativeapp.push.PushTarget
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class PingSnapshotsTest {
    private fun row(id: String, ts: Long = 0, readAt: Long = 0, href: String = "") =
        ForkNotification(id = id, kind = "code_push", title = "t$id", body = "b$id", repo = "fm/app", href = href, actor = "ann", ts = ts, readAt = readAt)

    private fun page(vararg rows: ForkNotification) = NotificationPage(rows.toList(), rows.count { it.isUnread })

    private class PingsClient : InboxClient {
        var pings: List<ForkNotification> = emptyList()

        override fun call(method: String, path: String, body: Map<String, Any?>?): Map<String, Any?> {
            if (method == "GET" && path.startsWith("/api/notifications")) {
                return mapOf("notifications" to pings.map { it.toJson() }, "unread" to pings.count { it.isUnread }.toLong())
            }
            if (method == "GET" && path == "/api/tasks") return mapOf("tasks" to emptyList<Any?>())
            return emptyMap()
        }
    }

    fun testForkNotificationJsonRoundTrips() {
        val original = ForkNotification(
            id = "p1", kind = "pull_submitted", title = "PR #4", body = "Fix it", repo = "fm/app", href = "/fm/app/pulls/4",
            actor = "ann", source = "world", ts = 1_700_000_000_123, readAt = 0, meta = mapOf("number" to 4L, "topic" to "pulls"),
        )
        assertEq(original, ForkNotification.fromJson(Json.parse(Json.stringify(original.toJson())).asJsonMap()))
        val page = NotificationPage(listOf(original, row("p2", readAt = 9)), unread = 1, total = 7)
        assertEq(page, NotificationPage.fromJson(Json.parse(Json.stringify(page.toJson())).asJsonMap()))
    }

    fun testSnapshotSavesAndLoadsPerAccount() {
        val store = MemoryPrefStore()
        val snapshots = PingSnapshots(store)
        assertEq(null, snapshots.load("proj", "ann"))
        snapshots.save("proj", "Ann", NotificationPage(listOf(row("a", 2), row("b", 1, readAt = 5)), unread = 1, total = 12))
        val reopened = PingSnapshots(store)
        val loaded = reopened.load("proj", " ann ")!!
        assertEq(listOf("a", "b"), loaded.notifications.map { it.id })
        assertEq(1, loaded.unread)
        assertEq(12, loaded.total)
        assertEq(null, reopened.load("proj", "bob"))
        assertEq(null, reopened.load("other", "ann"))
        assertEq(null, reopened.load("proj", ""))
        assertEq("tb", reopened.find("proj", "ann", "b")?.title)
        reopened.clear("proj", "ann")
        assertEq(null, PingSnapshots(store).load("proj", "ann"))
    }

    fun testSnapshotIsCapped() {
        val store = MemoryPrefStore()
        val rows = (0 until 150).map { row("r$it", ts = 1000L - it) }
        PingSnapshots(store).save("proj", "ann", NotificationPage(rows, unread = 150))
        val loaded = PingSnapshots(store).load("proj", "ann")!!
        assertEq(PingSnapshots.MAX_ROWS, loaded.notifications.size)
        assertEq("r0", loaded.notifications.first().id)
        assertEq(150, loaded.unread)
    }

    fun testInsertedRowSurvivesRefreshUntilThePageHasIt() {
        var now = 1_000L
        val store = MemoryPrefStore()
        val snapshots = PingSnapshots(store) { now }
        snapshots.save("proj", "ann", page(row("old", 1)))
        expect(snapshots.insert("proj", "ann", row("new", 5)))
        assertEq(listOf("new", "old"), PingSnapshots(store).load("proj", "ann")!!.notifications.map { it.id })

        val stale = snapshots.save("proj", "ann", page(row("old", 1)))
        assertEq(listOf("new", "old"), stale.notifications.map { it.id })
        assertEq(2, stale.unread)
        assertEq(2, stale.total)

        val caughtUp = snapshots.save("proj", "ann", page(row("new", 5, readAt = 7), row("old", 1)))
        assertEq(listOf("new", "old"), caughtUp.notifications.map { it.id })
        expect(!caughtUp.notifications.first().isUnread)

        val deletedElsewhere = snapshots.save("proj", "ann", page(row("old", 1)))
        assertEq(listOf("old"), deletedElsewhere.notifications.map { it.id })
    }

    fun testInsertedRowExpires() {
        var now = 1_000L
        val snapshots = PingSnapshots(MemoryPrefStore()) { now }
        snapshots.insert("proj", "ann", row("new", 5))
        now += PingSnapshots.INSERTED_TTL_MS + 1
        assertEq(listOf("old"), snapshots.save("proj", "ann", page(row("old", 1))).notifications.map { it.id })
    }

    fun testMarkReadAndRemoveUpdateTheSnapshot() {
        val snapshots = PingSnapshots(MemoryPrefStore())
        snapshots.save("proj", "ann", page(row("a", 2), row("b", 1)))
        snapshots.markRead("proj", "ann", listOf("a"), 42)
        var loaded = snapshots.load("proj", "ann")!!
        assertEq(42L, loaded.notifications.first { it.id == "a" }.readAt)
        assertEq(1, loaded.unread)
        snapshots.remove("proj", "ann", "b")
        loaded = snapshots.load("proj", "ann")!!
        assertEq(listOf("a"), loaded.notifications.map { it.id })
        assertEq(0, loaded.unread)
    }

    fun testInboxModelShowsTheSnapshotBeforeLoadingAndKeepsAnInsertedPing() {
        val store = MemoryPrefStore()
        val snapshots = PingSnapshots(store)
        snapshots.save("proj", "me", page(row("a", 2)))
        val client = PingsClient()
        val model = InboxModel(client, { "Me" }, MemoryPrefStore(), snapshots = snapshots, project = "proj")
        assertEq(listOf("ping:a"), model.items.map { it.id })
        expect(!model.loaded)

        val tapped = model.insert(row("fresh", 9, href = "/fm/app/pulls/3"))!!
        assertEq("ping:fresh", tapped.id)
        assertEq(listOf("ping:fresh", "ping:a"), model.items.map { it.id })

        client.pings = listOf(row("a", 2))
        model.load()
        assertEq(listOf("ping:fresh", "ping:a"), model.items.map { it.id })

        client.pings = listOf(row("fresh", 9, href = "/fm/app/pulls/3"), row("a", 2))
        model.load()
        client.pings = listOf(row("a", 2))
        model.load()
        assertEq(listOf("ping:a"), model.items.map { it.id })
        assertEq(listOf("a"), PingSnapshots(store).load("proj", "me")!!.notifications.map { it.id })
    }

    fun testPushTargetCarriesTheFullRow() {
        val ping = row("p9", ts = 77, href = "https://example.org/x").copy(meta = mapOf("topic" to "ci"))
        val target = PushTarget("proj", ping.id, ping.href, ping = ping)
        val extra = target.rowExtra()!!
        val parsed = PushTarget.from("proj", "p9", ping.href, row = extra)!!
        assertEq(ping, parsed.ping)
        assertEq(target.notificationId(), parsed.notificationId())

        val rowOnly = PushTarget.from("proj", null, null, row = extra)!!
        assertEq("p9", rowOnly.pingId)
        assertEq("https://example.org/x", rowOnly.href)

        assertEq(null, PushTarget.from("proj", "other", "", row = extra)!!.ping)
        assertEq(null, PushTarget.from("proj", "p9", "", row = "{not json")!!.ping)
        assertEq(null, PushTarget.from("proj", "p9", "", row = null)!!.ping)
        assertEq(null, PushTarget.from("proj", null, null, row = "{}"))
        assertEq(null, PushTarget("proj", "big", "", ping = row("big").copy(body = "x".repeat(PushTarget.MAX_ROW_CHARS))).rowExtra())
    }
}
