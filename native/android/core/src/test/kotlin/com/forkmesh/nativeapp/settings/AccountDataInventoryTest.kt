package com.forkmesh.nativeapp.settings

import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class AccountDataInventoryTest {
    private val payload: Map<String, Any?> = mapOf(
        "ok" to true,
        "generatedAt" to 5L,
        "subjects" to listOf(
            mapOf("id" to "account", "kind" to "account", "label" to "Your account", "tables" to 2L, "rows" to 3L, "nodes" to listOf("alice-laptop")),
            mapOf("id" to "org:acme", "kind" to "organization", "label" to "acme", "role" to "member", "readable" to false, "tables" to 1L, "rows" to 1L),
        ),
        "groups" to listOf(
            mapOf("subject" to "account", "table" to "notifications", "label" to "Notifications", "category" to "Notifications", "rows" to 2L, "deletable" to true),
            mapOf("subject" to "account", "table" to "users", "label" to "Your account record", "category" to "Account", "rows" to 1L),
            mapOf("subject" to "account", "table" to "steps_keys", "label" to "Steps keys", "category" to "Other", "rows" to 1L, "withheld" to true),
            mapOf("subject" to "org:acme", "table" to "orgs", "label" to "Organization record", "category" to "Organizations", "rows" to 1L, "readable" to false),
        ),
        "store" to listOf(mapOf("key" to "android.home", "kind" to "layout", "label" to "Home screen and bar layout", "bytes" to 120L, "updatedAt" to 9L)),
    )

    fun testParsesSubjectsGroupsAndStore() {
        val inventory = AccountDataInventory.parse(payload)
        assertEq(listOf("account", "org:acme"), inventory.subjects.map { it.id })
        assertEq("Organization acme", inventory.subjects[1].title)
        expect(inventory.subjects[0].subtitle.contains("alice-laptop"))
        expect(inventory.subjects[1].subtitle.contains("owners and admins"))
        assertEq("Home screen and bar layout", inventory.store.first().title)
        assertEq(6, inventory.totalRows)
        val sections = inventory.groups("account")
        assertEq(listOf("Account", "Notifications", "Other"), sections.map { it.category })
        expect(sections[2].groups[0].note.contains("never shown"))
    }

    fun testRemovingRowsDropsAnEmptiedGroup() {
        val inventory = AccountDataInventory.parse(payload)
        val once = inventory.removing("account/notifications", 1)
        assertEq(1, once.groups.first { it.table == "notifications" }.rows)
        val twice = once.removing("account/notifications", 1)
        expect(twice.groups.none { it.table == "notifications" })
        expect(twice.removingStore("android.home").store.isEmpty())
    }

    fun testRowTitlesFieldsAndTimes() {
        val row = AccountDataRow.parse(
            mapOf(
                "rowid" to 4L, "deletable" to true,
                "fields" to linkedMapOf("recipient_bi" to "(hashed identifier)", "ts" to 1_760_000_000_000L, "data" to mapOf("title" to "Mention", "body" to "hi")),
            ),
            0,
        )
        assertEq("4", row.id)
        expect(row.deletable)
        assertEq("Mention", row.title)
        assertEq("2025-10-09 08:53 UTC", row.fields.first { it.name == "ts" }.text)
        expect(row.fields.first { it.name == "data" }.isStructured)
        val bare = AccountDataRow.parse(mapOf("fields" to mapOf("kind" to "(withheld: security credential)")), 2)
        assertEq("i2", bare.id)
        expect(!bare.deletable)
        assertEq("Item", bare.title)
    }

    fun testPathsEncodeQueryValues() {
        assertEq("/api/accounts/data/rows?subject=org%3Aacme&table=orgs", AccountDataPaths.rows("org:acme", "orgs", 0))
        assertEq("/api/accounts/data/rows?subject=account&table=notifications&after=50", AccountDataPaths.rows("account", "notifications", 50))
        assertEq("/api/accounts/data/rows/notifications/7", AccountDataPaths.row("notifications", 7))
        assertEq("/api/accounts/data/store/android.home", AccountDataPaths.storeItem("android.home"))
    }

    fun testBrowserLoadsPagesAndDeletesARow() {
        val fake = FakeAccountEndpoints()
        fake.answers["GET /api/accounts/data"] = Result.success(payload)
        fake.answers["GET ${AccountDataPaths.rows("account", "notifications", 0)}"] = Result.success(
            mapOf("ok" to true, "readable" to true, "next" to 9L, "rows" to listOf(mapOf("rowid" to 3L, "deletable" to true, "fields" to mapOf("data" to mapOf("title" to "One"))))),
        )
        fake.answers["GET ${AccountDataPaths.rows("account", "notifications", 9)}"] = Result.success(
            mapOf("ok" to true, "readable" to true, "next" to 0L, "rows" to listOf(mapOf("rowid" to 9L, "deletable" to true, "fields" to mapOf("data" to mapOf("title" to "Two"))))),
        )
        val browser = AccountDataBrowser(fake)
        browser.load()
        val group = browser.inventory!!.groups.first { it.table == "notifications" }
        browser.loadRows(group)
        browser.loadRows(group, more = true)
        assertEq(listOf("One", "Two"), browser.page(group)?.rows?.map { it.title })
        assertEq(0L, browser.page(group)?.next)
        expect(browser.delete(browser.page(group)!!.rows[0], group))
        assertEq("DELETE", fake.calls.last().method)
        assertEq("/api/accounts/data/rows/notifications/3", fake.calls.last().path)
        assertEq(listOf("Two"), browser.page(group)?.rows?.map { it.title })
        assertEq(1, browser.inventory?.groups?.first { it.table == "notifications" }?.rows)
    }

    fun testBrowserSaysWhenTheServerIsOld() {
        val fake = FakeAccountEndpoints()
        fake.answers["GET /api/accounts/data"] = Result.failure(AccountEndpointException("http_404", 404))
        val browser = AccountDataBrowser(fake)
        browser.load()
        assertEq("This server cannot list your data yet.", browser.error)
    }
}

class AccountSyncTest {
    private class FakeSource(var value: Map<String, Any?>?) : AccountSyncSource {
        override val key = "android.rss"
        override val kind = "subscriptions"
        override val label = "News"
        val restored = ArrayList<Map<String, Any?>>()

        override fun snapshot(): Map<String, Any?>? = value

        override fun restore(value: Map<String, Any?>): Int {
            restored.add(value)
            return 1
        }
    }

    fun testPreferenceSourcesSplitHomeFromSettingsAndSkipSecrets() {
        val store = MemoryAccountSyncPrefs(
            mapOf(
                "shell/home-style" to "{\"columns\":4}",
                "shell/bar-recent" to "a,b",
                "appearance/themeMode" to "dark",
                "device/haptics" to true,
                "auth/session" to "secret",
                "p/one/server/room" to "room",
                "chat/channelGroups" to "x".repeat(AccountSyncKeys.MAX_VALUE_CHARS + 1),
            ),
        )
        val home = PreferenceSyncSource.home(store).snapshot()?.get("values") as? Map<*, *>
        assertEq(setOf("shell/home-style"), home?.keys)
        val settings = PreferenceSyncSource.settings(store).snapshot()?.get("values") as? Map<*, *>
        assertEq(setOf("appearance/themeMode", "device/haptics"), settings?.keys)
        val fresh = MemoryAccountSyncPrefs()
        val restored = PreferenceSyncSource.settings(fresh).restore(
            mapOf(
                "type" to DataFolder.SETTINGS_TYPE, "schemaVersion" to 1L,
                "values" to mapOf(
                    "appearance/themeMode" to mapOf("t" to "string", "v" to "light"),
                    "auth/session" to mapOf("t" to "string", "v" to "stolen"),
                    "shell/home-style" to mapOf("t" to "string", "v" to "{}"),
                ),
            ),
        )
        assertEq(1, restored)
        assertEq("light", fresh.raw("appearance/themeMode"))
        assertEq(null, fresh.raw("auth/session"))
        assertEq(null, fresh.raw("shell/home-style"))
    }

    fun testSavesOnlyChangedSourcesWhileEnabled() {
        val fake = FakeAccountEndpoints()
        val prefs = MemoryAccountSyncPrefs()
        val source = FakeSource(mapOf("feeds" to listOf("https://example.com/rss")))
        val sync = AccountSyncService(fake, prefs, listOf(source), device = "Pixel", clock = { 1_000L }) { "alice" }
        assertEq(0, sync.saveIfChanged())
        expect(fake.calls.isEmpty())
        sync.enabled = true
        assertEq(1, sync.saveIfChanged())
        assertEq("PUT", fake.calls.last().method)
        assertEq("/api/accounts/data/store/android.rss", fake.calls.last().path)
        assertEq("subscriptions", fake.calls.last().body?.get("kind"))
        assertEq("Pixel", fake.calls.last().body?.get("device"))
        assertEq(1_000L, sync.lastSavedAt)
        assertEq(0, sync.saveIfChanged())
        source.value = mapOf("feeds" to emptyList<String>())
        assertEq(1, sync.saveIfChanged())
        assertEq(2, fake.calls.size)
        assertEq(1, sync.saveNow())
        sync.enabled = false
        expect(prefs.keys().none { it.startsWith("accountSync/hash/") })
    }

    fun testRestoreAppliesTheSavedCopyAndReportsAnEmptyAccount() {
        val fake = FakeAccountEndpoints()
        val source = FakeSource(null)
        val sync = AccountSyncService(fake, MemoryAccountSyncPrefs(), listOf(source)) { "alice" }
        fake.answers["GET /api/accounts/data/store/android.rss"] = Result.failure(AccountEndpointException("not_found", 404))
        assertEq(0, sync.restore())
        assertEq("Nothing is saved on ForkMesh yet.", sync.error)
        fake.answers["GET /api/accounts/data/store/android.rss"] = Result.success(mapOf("ok" to true, "item" to mapOf("key" to "android.rss", "value" to mapOf("feeds" to listOf("x")))))
        assertEq(1, sync.restore())
        assertEq("", sync.error)
        assertEq(listOf(mapOf("feeds" to listOf("x"))), source.restored.toList())
    }

    fun testOldServerStopsSaving() {
        val fake = FakeAccountEndpoints()
        fake.answers["PUT /api/accounts/data/store/android.rss"] = Result.failure(AccountEndpointException("http_404", 404))
        val sync = AccountSyncService(fake, MemoryAccountSyncPrefs(), listOf(FakeSource(mapOf("n" to 1L)))) { "alice" }
        assertEq(0, sync.saveNow())
        assertEq("This server cannot save your data yet.", sync.error)
    }
}
