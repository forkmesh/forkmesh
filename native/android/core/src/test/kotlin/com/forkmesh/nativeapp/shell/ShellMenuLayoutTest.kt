package com.forkmesh.nativeapp.shell

import com.forkmesh.nativeapp.api.MemoryPrefStore
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class ShellMenuLayoutTest {
    private val defaults = listOf(
        "main" to listOf("home", "Agents", "Inbox", "Repos"),
        "repo" to listOf("repo-pulls", "repo-issues"),
        "workspace" to listOf("notes", "tasks"),
        "business" to emptyList(),
    )

    private fun arranged(layout: ShellMenuLayout, groups: List<Pair<String, List<String>>> = defaults): List<List<String>> =
        layout.arranged(groups, { it.first }, { it.second }, { it })

    private fun current(layout: ShellMenuLayout): List<Pair<String, List<String>>> = defaults.map { it.first }.zip(arranged(layout))

    fun testAnEmptyLayoutKeepsTheDefaultGroups() {
        assertEq(defaults.map { it.second }, arranged(ShellMenuLayout()))
        expect(ShellMenuLayout().isEmpty)
    }

    fun testMovingForwardInAGroupLandsAfterTheTarget() {
        val layout = ShellMenuLayout().let { it.moved("home", "main", "Inbox", current(it)) }
        assertEq(listOf("Agents", "Inbox", "home", "Repos"), arranged(layout)[0])
    }

    fun testMovingBackwardInAGroupLandsBeforeTheTarget() {
        val layout = ShellMenuLayout().let { it.moved("Repos", "main", "Agents", current(it)) }
        assertEq(listOf("home", "Repos", "Agents", "Inbox"), arranged(layout)[0])
    }

    fun testAnItemMovesIntoAnotherGroup() {
        var layout = ShellMenuLayout().let { it.moved("repo-pulls", "main", "Repos", current(it)) }
        assertEq(listOf("home", "Agents", "Inbox", "repo-pulls", "Repos"), arranged(layout)[0])
        assertEq(listOf("repo-issues"), arranged(layout)[1])
        layout = layout.moved("tasks", "business", null, current(layout))
        assertEq(listOf("notes"), arranged(layout)[2])
        assertEq(listOf("tasks"), arranged(layout)[3])
    }

    fun testAnAbsentItemKeepsItsPlaceForWhenItComesBack() {
        var layout = ShellMenuLayout().let { it.moved("repo-pulls", "main", "home", current(it)) }
        val noRepo = defaults.filter { it.first != "repo" }
        val withoutRepo = noRepo.map { it.first }.zip(arranged(layout, noRepo))
        assertEq(listOf("home", "Agents", "Inbox", "Repos"), withoutRepo[0].second)
        layout = layout.moved("notes", "main", null, withoutRepo)
        assertEq(listOf("repo-pulls", "home", "Agents", "Inbox", "Repos", "notes"), arranged(layout)[0])
    }

    fun testNewItemsJoinTheEndOfTheirGroupAndAMissingGroupFallsBack() {
        val layout = ShellMenuLayout().let { it.moved("tasks", "business", null, current(it)) }
        val grown = defaults.mapIndexed { index, entry -> if (index == 0) entry.first to entry.second + "Search" else entry }
        assertEq(listOf("home", "Agents", "Inbox", "Repos", "Search"), arranged(layout, grown)[0])
        val noBusiness = defaults.filter { it.first != "business" }
        assertEq(listOf("notes", "tasks"), arranged(layout, noBusiness)[2])
    }

    fun testTheLayoutRoundTripsThroughTheStore() {
        val store = MemoryPrefStore()
        val layout = ShellMenuLayout().let { it.moved("notes", "main", "home", current(it)) }
        layout.save(store)
        assertEq(layout, ShellMenuLayout.load(store))
        ShellMenuLayout().save(store)
        assertEq(null, store.getString(ShellMenuLayout.KEY))
    }
}

class ShellBarPrefsMenuTest {
    fun testAMoveIsKeptAndReadBackAndResetClearsIt() {
        val store = MemoryPrefStore()
        val prefs = ShellBarPrefs(store)
        val groups = listOf("main" to listOf("home", "Agents", "Inbox"), "workspace" to listOf("notes"))
        expect(prefs.moveInMenu("notes", "main", "home", groups))
        val arranged = ShellBarPrefs(store).arranged(groups, { it.first }, { it.second }, { it })
        assertEq(listOf("notes", "home", "Agents", "Inbox"), arranged[0].second)
        assertEq(1, arranged.size)
        prefs.resetMenu()
        assertEq(null, store.getString(ShellMenuLayout.KEY))
    }
}
