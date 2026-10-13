package com.forkmesh.nativeapp.admin

import com.forkmesh.nativeapp.insights.InsightsException
import com.forkmesh.nativeapp.insights.InsightsRequesting
import com.forkmesh.nativeapp.insights.StatsTone
import com.forkmesh.nativeapp.model.OrgApiException
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.assertFails
import com.forkmesh.nativeapp.testing.expect

private class RecordingTransport : InsightsRequesting {
    val calls = ArrayList<Triple<String, String, Map<String, Any?>?>>()
    val replies = HashMap<String, Map<String, Any?>>()
    val failing = HashSet<String>()

    override fun call(method: String, path: String, body: Map<String, Any?>?): Map<String, Any?> {
        calls.add(Triple(method, path, body))
        val target = body?.get("target") as? String
        if (target != null && target in failing) throw OrgApiException("target_is_admin", "x")
        return replies[path.substringBefore('?')] ?: emptyMap()
    }
}

class AdminTest {
    fun testClientPathsAndParsing() {
        val transport = RecordingTransport()
        transport.replies["/api/admin/phone/overview"] = mapOf("actor" to "fred", "errors24h" to 4L, "users" to 90L)
        transport.replies["/api/admin/phone/errors"] = mapOf(
            "total" to 7L, "groupCount" to 2L, "hours" to listOf(1L, 0L, 6L),
            "groups" to listOf(
                mapOf("status" to "500", "method" to "GET", "path" to "/api/x", "message" to "boom", "source" to "server", "count" to 5L, "userCount" to 2L, "anonymous" to 1L),
                mapOf("status" to "0", "source" to "desktop", "count" to 2L),
            ),
        )
        val client = AdminClient(transport)
        val overview = client.overview()
        assertEq("fred", overview.actor)
        assertEq(4, overview.errors24h)
        assertEq(null, overview.pendingVerifications)
        val errors = client.errors()
        assertEq(listOf(1, 0, 6), errors.hours)
        assertEq(1, errors.filtered("desktop").size)
        assertEq(2, errors.filtered(null).size)
        val group = errors.groups[0]
        assertEq("GET /api/x", group.requestLine)
        assertEq("2 users · 1 anonymous", group.affected)
        assertEq("Server", group.sourceLabel)
        expect(group.agentPrompt("app.forkmesh.com").startsWith("Investigate this server error group from app.forkmesh.com"))
        assertEq("HTTP 0", errors.groups[1].title)
        assertEq("Desktop", errors.groups[1].sourceLabel)
        client.rows("users", limit = 500)
        assertEq("/api/admin/phone/database?op=rows&table=users&limit=50", transport.calls.last().second)
        client.inventory()
        assertEq("/api/admin/phone/database?op=inventory", transport.calls.last().second)
        client.inventory(refresh = true)
        assertEq("/api/admin/phone/database?op=inventory&refresh=1", transport.calls.last().second)
    }

    fun testUserActionsValidateAndBatch() {
        val transport = RecordingTransport()
        val client = AdminClient(transport)
        client.perform(AdminUserAction.Quarantine, " Fred ")
        assertEq(Triple("POST", "/api/accounts/admin-quarantine", mapOf<String, Any?>("target" to "fred")), transport.calls.last())
        assertEq("invalid_user", assertFails<InsightsException> { client.perform(AdminUserAction.Delete, "a b") }.code)
        assertEq("invalid_user", assertFails<InsightsException> { client.perform(AdminUserAction.Delete, "") }.code)
        transport.failing.add("boss")
        val failures = client.perform(AdminUserAction.Delete, listOf("ann", "boss") + (1..40).map { "u$it" })
        assertEq(listOf("boss"), failures.keys.toList())
        assertEq("target_is_admin", failures.getValue("boss").code)
        assertEq(1 + AdminClient.MAX_BATCH, transport.calls.size)
        expect(AdminUserAction.Delete.needsDeviceAuth)
        expect(!AdminUserAction.Unquarantine.needsDeviceAuth)
    }

    fun testPurgeRepository() {
        val transport = RecordingTransport()
        val client = AdminClient(transport)
        client.purgeRepository(" forkmesh ", "web.site")
        assertEq("DELETE", transport.calls.last().first)
        assertEq("/api/repositories?owner=forkmesh&name=web.site", transport.calls.last().second)
        assertEq("invalid_repository", assertFails<InsightsException> { client.purgeRepository("..", "x") }.code)
        assertEq("invalid_repository", assertFails<InsightsException> { client.purgeRepository("a/b", "x") }.code)
        assertEq("forkmesh/web", AdminClient.purgeConfirmation("forkmesh", "web"))
    }

    fun testUsersInventoryCronStorage() {
        val users = AdminUsers.fromJson(
            mapOf(
                "users" to listOf(
                    mapOf("name" to "ann", "email" to "a@x", "createdAt" to 1_700_000_000L, "isAdmin" to true, "emailVerified" to true),
                    mapOf("name" to "bob", "email" to "bob@y", "createdAt" to 1_800_000_000_000L, "accountStatus" to "active", "nodes" to listOf(1L, 2L)),
                    mapOf("email" to "nameless"),
                ),
                "quarantinedUsers" to listOf(mapOf("name" to "eve", "emailVerified" to true)),
            ),
        )
        assertEq(listOf("bob", "ann"), users.filtered("", false).map { it.name })
        assertEq(listOf("bob"), users.filtered("Y", false).map { it.name })
        assertEq(listOf("eve"), users.filtered("", true).map { it.name })
        assertEq(1_700_000_000_000L, users.users[0].createdAt)
        assertEq(listOf("Admin"), users.users[0].badges)
        assertEq(listOf("Unverified"), users.users[1].badges)
        assertEq(2, users.users[1].nodes)
        assertEq(listOf("Quarantined"), users.quarantined[0].badges)
        expect(users.hasAdminFields)
        val inventory = DatabaseInventory.fromJson(
            mapOf("tables" to listOf(mapOf("name" to "b", "rows" to 5L), mapOf("name" to "a", "rows" to 5L), mapOf("name" to "big", "rows" to 50L))),
        )
        assertEq(listOf("big", "a", "b"), inventory.filtered("").map { it.name })
        assertEq(listOf("big", "b"), inventory.filtered(" B ").map { it.name })
        assertEq(60, inventory.totalRows)
        val page = DatabasePage.fromJson(
            mapOf(
                "table" to "t",
                "columns" to listOf(mapOf("name" to "id"), mapOf("name" to "data"), mapOf("name" to "note")),
                "rows" to listOf(
                    mapOf("rowid" to 7L, "values" to mapOf("id" to "x".repeat(500), "note" to mapOf("k" to 1L)), "data" to mapOf("a" to true)),
                    mapOf("digest" to "d1", "values" to mapOf<String, Any?>()),
                ),
                "nextCursor" to "c2",
            ),
        )
        val first = page.rows[0]
        assertEq("7", first.rowid)
        assertEq(DatabaseRow.CELL_LIMIT, first.cells[0].value.length)
        assertEq("(decoded below)", first.cells[1].value)
        assertEq("{\"k\":1}", first.cells[2].value)
        expect(first.data.contains("\"a\""))
        assertEq("1:d1", page.rows[1].id)
        assertEq("NULL", page.rows[1].cells[1].value)
        assertEq("c2", page.nextCursor)
        assertEq(StatsTone.None, CronSummary.fromJson(emptyMap()).health)
        assertEq(StatsTone.Good, CronSummary.fromJson(mapOf("summary" to mapOf("lastTickAt" to 5L, "gapMs" to 1000L))).health)
        assertEq(StatsTone.Critical, CronSummary.fromJson(mapOf("summary" to mapOf("lastTickAt" to 5L, "gapMs" to 11L * 60 * 1000))).health)
        assertEq(StatsTone.Warning, CronSummary.fromJson(mapOf("summary" to mapOf("lastTickAt" to 5L, "failedTicks" to 1L))).health)
        val unavailable = CronSummary.fromJson(mapOf("ok" to false, "error" to "no_cron"))
        assertEq("no_cron", unavailable.unavailable)
        assertEq(StatsTone.Warning, unavailable.health)
        val storage = StorageUsage.fromJson(mapOf("storage" to mapOf("dbBytes" to 2048L, "rows" to 1284L, "region" to "ewr", "none" to null)))
        assertEq(listOf("dbBytes" to "2.0 KB", "region" to "ewr", "rows" to "1,284"), storage.entries.map { it.label to it.value })
        assertEq(AdminSection.Database, AdminSection.of("DATABASE"))
        assertEq(AdminSection.Overview, AdminSection.of("nope"))
    }
}
