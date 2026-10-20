package com.forkmesh.nativeapp.shell

import com.forkmesh.nativeapp.api.MemoryPrefStore
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class ShellBarPrefsTest {
    private val items = listOf("home", "Agents", "Inbox", "Repos", "Chat", "notes", "timers", "settings")
    private val id: (String) -> String = { it }

    private fun slotIds(prefs: ShellBarPrefs): List<String> = prefs.slots(items, id).map { ShellBarLayout.slotId(it, id) }

    fun testEverythingIsReadBackFromTheStore() {
        val store = MemoryPrefStore()
        val first = ShellBarPrefs(store)
        first.setSort(ShellBarSort.recent)
        first.use("notes")
        first.use("Chat")
        first.styleBar { shape = ShellBarShape.squares }
        first.styleHome { across = 4 }
        first.arrange { hide("timers") }
        val second = ShellBarPrefs(store)
        assertEq(ShellBarSort.recent, second.sort)
        assertEq(listOf("Chat", "notes"), second.recent)
        assertEq(mapOf("Chat" to 1, "notes" to 1), second.uses)
        assertEq(ShellBarShape.squares, second.barStyle.shape)
        assertEq(4, second.home.across)
        assertEq(listOf("timers"), second.layout.hidden)
    }

    fun testTheBarKeepsHomeAndFollowsTheRanking() {
        val prefs = ShellBarPrefs(MemoryPrefStore())
        assertEq(listOf("home", "Agents", "Inbox", "Repos", "Chat", "notes"), slotIds(prefs))
        prefs.setSort(ShellBarSort.recent)
        prefs.use("settings")
        prefs.use("timers")
        assertEq(listOf("timers", "settings", "home", "Agents", "Inbox", "Repos"), slotIds(prefs))
        assertEq(setOf("timers", "settings", "home", "Agents", "Inbox", "Repos"), prefs.barIds(items, id))
        expect(prefs.inBar("timers", items, id))
        expect(prefs.barIsFull(items, id))
    }

    fun testTheBarStyleLimitAndRowsSetTheCapacity() {
        val prefs = ShellBarPrefs(MemoryPrefStore())
        prefs.styleBar { limit = 4 }
        assertEq(4, prefs.slots(items, id).size)
        prefs.styleBar { rows = ShellBarRows.double }
        assertEq(8, prefs.slots(items, id).size)
        prefs.styleBar { limit = 99 }
        assertEq(8, prefs.barStyle.limit)
    }

    fun testStyleEditsClampAndOnlyWriteWhenTheyChange() {
        val store = MemoryPrefStore()
        val prefs = ShellBarPrefs(store)
        prefs.styleHome { across = 12 }
        assertEq(6, prefs.home.across)
        prefs.styleHome { across = 5 }
        assertEq(null, store.getString(ShellHomeStyle.KEY))
        prefs.styleBar { corner = 7.0 }
        assertEq(1.0, prefs.barStyle.corner)
        prefs.styleBar { corner = 0.5 }
        assertEq(null, store.getString(ShellBarStyle.KEY))
    }

    fun testSteppingAnItemSwitchesToTheUsualOrder() {
        val prefs = ShellBarPrefs(MemoryPrefStore())
        prefs.setSort(ShellBarSort.recent)
        prefs.use("notes")
        expect(prefs.canStep("notes", ShellBarStep.right, items, id))
        prefs.step("notes", ShellBarStep.right, items, id)
        assertEq(ShellBarSort.usual, prefs.sort)
        expect(prefs.layout.order.isNotEmpty())
    }

    fun testPuttingAnItemInAPlaceSwitchesToTheUsualOrder() {
        val prefs = ShellBarPrefs(MemoryPrefStore())
        prefs.setSort(ShellBarSort.frequent)
        expect(!prefs.putInBar("settings", null, items, id))
        expect(prefs.putInBar("settings", "Repos", items, id))
        assertEq(ShellBarSort.usual, prefs.sort)
        assertEq(listOf("home", "Agents", "Inbox", "settings", "Chat", "notes"), slotIds(prefs))
        prefs.removeFromBar(listOf("Inbox", "home"), items, id)
        assertEq(listOf("home", "Agents", "settings", "Chat", "notes"), slotIds(prefs))
    }

    fun testVisitAndClearRefreshTheUsage() {
        val prefs = ShellBarPrefs(MemoryPrefStore())
        prefs.visit(listOf("Inbox"))
        prefs.visit(listOf("Inbox"))
        assertEq(listOf("Inbox"), prefs.recent)
        assertEq(mapOf("Inbox" to 1), prefs.uses)
        prefs.clear()
        expect(prefs.recent.isEmpty() && prefs.uses.isEmpty())
    }

    fun testARemovedTabStaysOutOfTheBarAfterReload() {
        val store = com.forkmesh.nativeapp.api.MemoryPrefStore()
        val items = listOf("home", "agents", "inbox", "repos", "code", "chat", "search", "palette", "market", "crm")
        val first = ShellBarPrefs(store)
        assertEq(listOf("home", "agents", "inbox", "repos", "code", "chat"), first.barIds(items) { it }.toList())
        first.removeFromBar(listOf("inbox"), items) { it }
        expect("inbox" !in first.barIds(items) { it }, "removed now")
        val again = ShellBarPrefs(store)
        expect("inbox" !in again.barIds(items) { it }, "removed after reload")
        expect("market" !in again.barIds(items) { it }, "the freed slot is not refilled")
    }
}

