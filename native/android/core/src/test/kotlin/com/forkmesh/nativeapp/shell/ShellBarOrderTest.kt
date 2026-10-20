package com.forkmesh.nativeapp.shell

import com.forkmesh.nativeapp.api.MemoryPrefStore
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import kotlin.math.abs

class ShellBarOrderTest {
    private val id: (String) -> String = { it }
    private val items = listOf("Agents", "Inbox", "Repos", "Search", "settings", "world")

    private fun near(expected: Double, actual: Double) = expect(abs(expected - actual) < 0.0001, "expected <$expected> but was <$actual>")

    fun testBarKeepsItsOrderUntilRecentFirstIsOn() {
        val order = ShellBarOrder(MemoryPrefStore())
        order.use("world")
        assertEq(ShellBarSort.usual, order.sort)
        assertEq(items, order.ordered(items, id))
        order.sort = ShellBarSort.recent
        assertEq(listOf("world", "Agents", "Inbox", "Repos", "Search", "settings"), order.ordered(items, id))
        order.use("settings")
        order.use("Inbox")
        order.use("world")
        assertEq(listOf("world", "Inbox", "settings"), order.recent)
        assertEq(listOf("world", "Inbox", "settings", "Agents", "Repos", "Search"), order.ordered(items, id))
        order.clear()
        assertEq(items, order.ordered(items, id))
    }

    fun testRecentListIsCappedAndIgnoresGoneItems() {
        val many = (0 until ShellBarOrder.LIMIT + 5).map { "page$it" }
        val recent = many.fold(emptyList<String>()) { list, page -> ShellBarOrder.used(page, list) }
        assertEq(ShellBarOrder.LIMIT, recent.size)
        assertEq(many.last(), recent.first())
        assertEq(listOf("b", "a"), ShellBarOrder.ordered(listOf("a", "b"), listOf("gone", "b"), id))
    }

    fun testMostUsedSortsByUseCountThenRecency() {
        val order = ShellBarOrder(MemoryPrefStore())
        order.sort = ShellBarSort.frequent
        assertEq(items, order.ordered(items, id))
        for (page in listOf("settings", "world", "settings", "Inbox", "settings", "world", "Search")) order.use(page)
        assertEq(mapOf("settings" to 3, "world" to 2, "Inbox" to 1, "Search" to 1), order.uses)
        assertEq(listOf("settings", "world", "Search", "Inbox", "Agents", "Repos"), order.ordered(items, id))
        order.sort = ShellBarSort.recent
        assertEq(listOf("Search", "world", "settings", "Inbox", "Agents", "Repos"), order.ordered(items, id))
        order.clear()
        order.sort = ShellBarSort.frequent
        assertEq(emptyMap<String, Int>(), order.uses)
        assertEq(items, order.ordered(items, id))
    }

    fun testHeatGrowsWithUsesAndTopsOutAtTheMostUsed() {
        val ids = listOf("settings", "world", "Inbox", "Repos")
        val uses = mapOf("settings" to 40, "world" to 5, "Inbox" to 1, "gone" to 400)
        near(1.0, ShellBarOrder.heat("settings", uses, ids))
        assertEq(0.0, ShellBarOrder.heat("Repos", uses, ids))
        val world = ShellBarOrder.heat("world", uses, ids)
        val inbox = ShellBarOrder.heat("Inbox", uses, ids)
        expect(world > inbox)
        expect(inbox > 0)
        expect(world < 1)
        assertEq(0.0, ShellBarOrder.heat("settings", emptyMap(), ids))
        assertEq(40, ShellBarOrder.mostUses(uses, ids))
        assertEq(world, ShellBarOrder.heat("world", uses, ShellBarOrder.mostUses(uses, ids)))
    }

    fun testFrequentMarkNeedsThreeOpensAndHalfTheTopHeat() {
        val ids = listOf("settings", "world", "Inbox", "Repos", "Search")
        val uses = mapOf("settings" to 40, "world" to 12, "Inbox" to 2, "Search" to 1, "gone" to 400)
        near(1.0, ShellBarOrder.frequentMark("settings", uses, ids) ?: 0.0)
        val world = ShellBarOrder.frequentMark("world", uses, ids)
        expect(world != null)
        expect((world ?: 1.0) < 1)
        assertEq(null, ShellBarOrder.frequentMark("Inbox", uses, ids))
        assertEq(null, ShellBarOrder.frequentMark("Search", uses, ids))
        assertEq(null, ShellBarOrder.frequentMark("Repos", uses, ids))
        assertEq(null, ShellBarOrder.frequentMark("Search", mapOf("Search" to 2), ids))
        expect(ShellBarOrder.frequentMark("Search", mapOf("Search" to 3), ids) != null)
        assertEq(world, ShellBarOrder.frequentMark("world", uses, 40))
    }

    fun testPullingHomeDownPastTheThresholdRevealsSearchOnce() {
        expect(!ShellBarOrder.revealsSearch(0.0, false))
        expect(!ShellBarOrder.revealsSearch(-200.0, false))
        expect(!ShellBarOrder.revealsSearch(ShellBarOrder.SEARCH_PULL - 1, false))
        expect(ShellBarOrder.revealsSearch(ShellBarOrder.SEARCH_PULL, false))
        expect(!ShellBarOrder.revealsSearch(200.0, true))
    }

    fun testFittedTileShrinksRowsToTheScreenWithinBounds() {
        assertEq(42.0, ShellBarOrder.fittedTile(700.0, 100.0, 10, 42.0, 28.0))
        near(400.0 / 12, ShellBarOrder.fittedTile(500.0, 100.0, 12, 42.0, 28.0))
        assertEq(28.0, ShellBarOrder.fittedTile(300.0, 100.0, 12, 42.0, 28.0))
        assertEq(42.0, ShellBarOrder.fittedTile(0.0, 100.0, 12, 42.0, 28.0))
        assertEq(42.0, ShellBarOrder.fittedTile(500.0, 100.0, 0, 42.0, 28.0))
        assertEq(20.0, ShellBarOrder.fittedTile(50.0, 100.0, 4, 20.0, 28.0))
    }

    fun testVisitCountsAPageOpenedAnotherWayOnce() {
        val order = ShellBarOrder(MemoryPrefStore())
        order.use("Inbox")
        expect(!order.visit(listOf("Inbox")))
        expect(order.visit(listOf("Agents")))
        expect(!order.visit(listOf("Agents")))
        expect(!order.visit(listOf("")))
        expect(!order.visit(emptyList()))
        expect(order.visit(listOf("Inbox")))
        order.use("repo-pulls")
        expect(!order.visit(listOf("repo-pulls", "currentRepo")))
        expect(order.visit(listOf("repo-issues", "currentRepo")))
        assertEq(mapOf("Inbox" to 2, "Agents" to 1, "repo-pulls" to 1, "repo-issues" to 1), order.uses)
        assertEq(listOf("repo-issues", "repo-pulls", "Inbox", "Agents"), order.recent)
    }

    fun testSortReadsTheOldRecentFirstSwitchAndSeedsCountsFromRecent() {
        val store = MemoryPrefStore()
        store.putBool(ShellBarOrder.RECENT_FIRST_KEY, true)
        store.putString(ShellBarOrder.RECENT_KEY, "[\"world\",\"Inbox\"]")
        val order = ShellBarOrder(store)
        assertEq(ShellBarSort.recent, order.sort)
        assertEq(mapOf("world" to 1, "Inbox" to 1), order.uses)
        order.sort = ShellBarSort.frequent
        expect(!store.contains(ShellBarOrder.RECENT_FIRST_KEY))
        assertEq(ShellBarSort.frequent, ShellBarOrder(store).sort)
        assertEq(listOf("world", "Inbox"), order.ranking)
        order.use("Inbox")
        assertEq(listOf("Inbox", "world"), order.ranking)
    }

    fun testUseCountsAreCappedAndKeepTheItemJustUsed() {
        var uses = emptyMap<String, Int>()
        var recent = emptyList<String>()
        for (index in 0 until ShellBarOrder.LIMIT) {
            val page = "page$index"
            recent = ShellBarOrder.used(page, recent)
            uses = ShellBarOrder.counted(page, uses, recent)
            recent = ShellBarOrder.used(page, recent)
            uses = ShellBarOrder.counted(page, uses, recent)
        }
        recent = ShellBarOrder.used("new", recent)
        uses = ShellBarOrder.counted("new", uses, recent)
        assertEq(ShellBarOrder.LIMIT, uses.size)
        assertEq(1, uses["new"])
        assertEq(null, uses["page0"])
        assertEq(mapOf("a:b" to 2, "c" to 5), ShellBarOrder.decodeUses(ShellBarOrder.encodeUses(mapOf("a:b" to 2, "c" to 5))))
        assertEq(mapOf("ok" to 2), ShellBarOrder.decodeUses(listOf("x", "0:zero", "-1:neg", "3:", "2:ok")))
    }

    fun testListsAreStoredAsJsonArraysUnderTheSharedKeys() {
        val store = MemoryPrefStore()
        val order = ShellBarOrder(store)
        order.sort = ShellBarSort.frequent
        order.use("b")
        order.use("a")
        order.use("b")
        assertEq("frequent", store.getString("shell/bar-sort"))
        assertEq("[\"b\",\"a\"]", store.getString("shell/bar-recent"))
        assertEq("[\"1:a\",\"2:b\"]", store.getString("shell/bar-uses"))
        store.putString("shell/bar-recent", "not json")
        assertEq(emptyList<String>(), order.recent)
        order.clear()
        assertEq("[]", store.getString("shell/bar-recent"))
        assertEq("[]", store.getString("shell/bar-uses"))
    }
}
