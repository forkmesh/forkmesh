package com.forkmesh.nativeapp.shell

import com.forkmesh.nativeapp.api.MemoryPrefStore
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class ShellHomeStyleTest {
    private val sections = listOf(
        ShellHomeSection(
            "main", "Go to",
            listOf(
                ShellHomeEntry("Agents", "Agents", badge = 31, active = true),
                ShellHomeEntry("Inbox", "Inbox", count = 0),
                ShellHomeEntry("Repos", "Repos", badge = 2),
            ),
        ),
        ShellHomeSection(
            "workspace", "Workspace",
            listOf(
                ShellHomeEntry("notes", "Notes", count = 9),
                ShellHomeEntry("tasks", "Tasks", count = 16),
            ),
        ),
        ShellHomeSection(
            "business", "Business",
            listOf(
                ShellHomeEntry("crm", "CRM", count = 1840),
                ShellHomeEntry("market", "Market", count = 3),
                ShellHomeEntry("adReview", "Ad review"),
                ShellHomeEntry("analytics", "Analytics", active = true),
            ),
        ),
    )

    private fun ids(style: ShellHomeStyle, uses: Map<String, Int> = emptyMap(), recent: List<String> = emptyList()): List<List<String>> =
        style.arranged(sections, uses, recent).map { it.ids }

    fun testTheDefaultKeepsEverySectionInItsOwnOrder() {
        val style = ShellHomeStyle()
        assertEq(sections.map { section -> section.entries.map { it.id } }, ids(style))
        assertEq(listOf("Go to", "Workspace", "Business"), style.arranged(sections, emptyMap(), emptyList()).map { it.title })
        expect(style.headed)
        assertEq(5, style.across)
        expect(style.highlightFrequent)
    }

    fun testNameSortsWithinEachSection() {
        val style = ShellHomeStyle()
        style.sort = ShellHomeSort.byName
        assertEq(listOf("adReview", "analytics", "crm", "market"), ids(style)[2])
        style.sort = ShellHomeSort.nameDescending
        assertEq(listOf("market", "crm", "analytics", "adReview"), ids(style)[2])
    }

    fun testNamesCompareNaturallyAndIgnoreCase() {
        expect(shellNaturalCompare("Item 2", "Item 10") < 0)
        expect(shellNaturalCompare("Item 10", "Item 2") > 0)
        assertEq(0, shellNaturalCompare("Notes", "notes"))
        expect(shellNaturalCompare("apple", "Banana") < 0)
        expect(shellNaturalCompare("Item", "Item 1") < 0)
        assertEq(0, shellNaturalCompare("Item 01", "Item 1"))
        expect(shellNaturalCompare("éclair", "Eclair2") < 0)
        val entries = listOf("Item 10", "item 2", "Item 1").map { ShellHomeEntry(it, it) }
        val style = ShellHomeStyle()
        style.sort = ShellHomeSort.byName
        val sorted = ShellHomeStyle.sorted(entries, style.sort, emptyMap(), emptyList()).map { it.id }
        assertEq(listOf("Item 1", "item 2", "Item 10"), sorted)
        val tied = listOf(ShellHomeEntry("b", "Same"), ShellHomeEntry("a", "same"))
        assertEq(listOf("b", "a"), ShellHomeStyle.sorted(tied, ShellHomeSort.byName, emptyMap(), emptyList()).map { it.id })
        assertEq(listOf("b", "a"), ShellHomeStyle.sorted(tied, ShellHomeSort.nameDescending, emptyMap(), emptyList()).map { it.id })
    }

    fun testUseSortsKeepTheCustomOrderForTies() {
        val style = ShellHomeStyle()
        style.sort = ShellHomeSort.mostUsed
        val uses = mapOf("Repos" to 4, "Inbox" to 9)
        assertEq(listOf("Inbox", "Repos", "Agents"), ids(style, uses)[0])
        style.sort = ShellHomeSort.leastUsed
        assertEq(listOf("Agents", "Repos", "Inbox"), ids(style, uses)[0])
    }

    fun testRecentPutsTheNewestFirstAndTheNeverOpenedLast() {
        val style = ShellHomeStyle()
        style.sort = ShellHomeSort.recent
        assertEq(listOf("market", "crm", "adReview", "analytics"), ids(style, recent = listOf("market", "Repos", "crm"))[2])
    }

    fun testAttentionPutsUnreadThenActiveFirst() {
        val style = ShellHomeStyle()
        style.sort = ShellHomeSort.attention
        assertEq(listOf("Agents", "Repos", "Inbox"), ids(style)[0])
        assertEq(listOf("analytics", "crm", "market", "adReview"), ids(style)[2])
    }

    fun testBiggestCountPutsUnknownCountsLast() {
        val style = ShellHomeStyle()
        style.sort = ShellHomeSort.count
        assertEq(listOf("crm", "market", "adReview", "analytics"), ids(style)[2])
        assertEq(listOf("Agents", "Repos", "Inbox"), ids(style)[0])
    }

    fun testHiddenSectionsLeaveHomeButNotASearch() {
        val style = ShellHomeStyle()
        style.setShown("workspace", false)
        expect(!style.shows("workspace"))
        assertEq(listOf("main", "business"), style.arranged(sections, emptyMap(), emptyList()).map { it.id })
        assertEq(3, style.arranged(sections, emptyMap(), emptyList(), searching = true).size)
        style.setShown("workspace", true)
        expect(style.hidden.isEmpty())
    }

    fun testBarIconsLeaveHomeByDefaultButNotASearch() {
        var style = ShellHomeStyle()
        expect(style.hideBarIcons)
        val docked = setOf("Agents", "Inbox", "Repos", "notes")
        val arranged = style.arranged(sections, emptyMap(), emptyList(), docked)
        assertEq(listOf("workspace", "business"), arranged.map { it.id })
        assertEq(listOf("tasks"), arranged[0].ids)
        val everything = sections.map { section -> section.entries.map { it.id } }
        assertEq(everything, style.arranged(sections, emptyMap(), emptyList(), docked, searching = true).map { it.ids })
        style.grouped = false
        expect(!style.arranged(sections, emptyMap(), emptyList(), docked)[0].ids.contains("Agents"))
        style = ShellHomeStyle()
        style.hideBarIcons = false
        assertEq(everything, style.arranged(sections, emptyMap(), emptyList(), docked).map { it.ids })
        expect(!style.isDefault)
    }

    fun testUngroupedIsOneSectionSortedAcrossEverything() {
        val style = ShellHomeStyle()
        style.grouped = false
        style.sort = ShellHomeSort.byName
        val arranged = style.arranged(sections, emptyMap(), emptyList())
        assertEq(1, arranged.size)
        assertEq(ShellHomeStyle.ALL_ID, arranged[0].id)
        assertEq(listOf("adReview", "Agents", "analytics", "crm"), arranged[0].ids.take(4))
        expect(!style.headed)
        expect(!style.reorders)
    }

    fun testSectionOrders() {
        val style = ShellHomeStyle()
        style.groupOrder = ShellHomeGroupOrder.byName
        assertEq(listOf("business", "main", "workspace"), style.arranged(sections, emptyMap(), emptyList()).map { it.id })
        style.groupOrder = ShellHomeGroupOrder.size
        assertEq(listOf("business", "main", "workspace"), style.arranged(sections, emptyMap(), emptyList()).map { it.id })
        style.groupOrder = ShellHomeGroupOrder.mostUsed
        val uses = mapOf("notes" to 5, "crm" to 2)
        assertEq(listOf("workspace", "business", "main"), style.arranged(sections, uses, emptyList()).map { it.id })
    }

    fun testSavingRoundTripsAndTheDefaultLeavesNoKey() {
        val store = MemoryPrefStore()
        ShellHomeStyle().save(store)
        assertEq(null, store.getString(ShellHomeStyle.KEY))
        var style = ShellHomeStyle()
        style.sort = ShellHomeSort.attention
        style.icons = ShellHomeIconStyle.tiles
        style.across = 6
        style.highlightFrequent = false
        style.hideBarIcons = false
        style.setShown("personal", false)
        style.save(store)
        assertEq(style, ShellHomeStyle.load(store))
        style = ShellHomeStyle()
        style.save(store)
        assertEq(null, store.getString(ShellHomeStyle.KEY))
    }

    fun testTheSavedJsonUsesTheSharedFieldNamesAndValues() {
        val store = MemoryPrefStore()
        val style = ShellHomeStyle()
        style.sort = ShellHomeSort.nameDescending
        style.groupOrder = ShellHomeGroupOrder.mostUsed
        style.icons = ShellHomeIconStyle.outline
        style.setShown("business", false)
        style.save(store)
        val json = com.forkmesh.nativeapp.json.Json.parseObject(store.getString("shell/home-style")!!)
        assertEq("nameDescending", json["sort"])
        assertEq("mostUsed", json["groupOrder"])
        assertEq("outline", json["icons"])
        assertEq(listOf("business"), json["hidden"])
        assertEq(5L, json["across"])
        assertEq(true, json["hideBarIcons"])
        assertEq(10, json.size)
    }

    fun testAnOldOrBrokenSaveFallsBackFieldByField() {
        val store = MemoryPrefStore()
        store.putString(ShellHomeStyle.KEY, "{\"sort\":\"name\",\"across\":12,\"icons\":\"sparkly\"}")
        val style = ShellHomeStyle.load(store)
        assertEq(ShellHomeSort.byName, style.sort)
        assertEq(6, style.across)
        assertEq(ShellHomeIconStyle.outline, style.icons)
        expect(style.grouped)
        expect(style.hideBarIcons)
        store.putString(ShellHomeStyle.KEY, "{\"across\":1,\"grouped\":\"yes\",\"hidden\":[\"a\",3]}")
        val low = ShellHomeStyle.load(store)
        assertEq(4, low.across)
        expect(low.grouped)
        expect(low.hidden.isEmpty())
        store.putString(ShellHomeStyle.KEY, "not json")
        assertEq(ShellHomeStyle(), ShellHomeStyle.load(store))
        store.putString(ShellHomeStyle.KEY, "[1,2]")
        assertEq(ShellHomeStyle(), ShellHomeStyle.load(store))
    }

    fun testRowsCountEachSectionOnItsOwnRows() {
        val arranged = ShellHomeStyle().arranged(sections, emptyMap(), emptyList())
        assertEq(3, ShellHomeStyle.rows(arranged, 5))
        assertEq(4, ShellHomeStyle.rows(arranged, 3))
    }
}
