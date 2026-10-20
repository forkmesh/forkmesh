package com.forkmesh.nativeapp.shell

import com.forkmesh.nativeapp.api.MemoryPrefStore
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class ShellBarLayoutTest {
    private val items = listOf("home", "Agents", "Inbox", "Repos", "Chat", "notes", "timers", "settings")
    private val id: (String) -> String = { it }
    private val keepHome = listOf("home")

    private fun keys(layout: ShellBarLayout, ranking: List<String> = emptyList()): List<String> =
        layout.slots(items, ranking, id).map { slot ->
            when (slot) {
                is ShellBarSlot.Single -> slot.item
                is ShellBarSlot.Folder -> "${slot.folder.name}[${slot.members.joinToString(",")}]"
            }
        }

    private fun bar(layout: ShellBarLayout, ranking: List<String> = emptyList(), keep: List<String> = keepHome, limit: Int = ShellBarLayout.BAR_LIMIT): List<String> =
        layout.barSlots(items, ranking, keep, limit, id).map { ShellBarLayout.slotId(it, id) }

    fun testAnEmptyLayoutKeepsTheBarAsItIs() {
        assertEq(items, keys(ShellBarLayout()))
        expect(ShellBarLayout().isEmpty)
    }

    fun testHiddenItemsLeaveTheBarAndComeBack() {
        val layout = ShellBarLayout()
        layout.hide("Inbox")
        layout.hide("timers")
        layout.hide("Inbox")
        assertEq(listOf("Inbox", "timers"), layout.hidden)
        assertEq(listOf("home", "Agents", "Repos", "Chat", "notes", "settings"), keys(layout))
        layout.show("Inbox")
        assertEq(listOf("home", "Agents", "Inbox", "Repos", "Chat", "notes", "settings"), keys(layout))
        layout.showAll()
        assertEq(items, keys(layout))
    }

    fun testAFolderSitsWhereItsFirstItemWasAndKeepsItsOwnOrder() {
        val layout = ShellBarLayout()
        val folderId = layout.createFolder("  Tools  ", "timers")
        layout.move("notes", folderId)
        assertEq("Tools", layout.folder(folderId)?.name)
        assertEq(listOf("home", "Agents", "Inbox", "Repos", "Chat", "Tools[timers,notes]", "settings"), keys(layout))
        layout.move("Agents", folderId)
        assertEq(listOf("home", "Tools[timers,notes,Agents]", "Inbox", "Repos", "Chat", "settings"), keys(layout))
        assertEq(folderId, layout.folderOf("notes")?.id)
        layout.move("notes", null)
        assertEq(null, layout.folderOf("notes"))
        assertEq(listOf("home", "Tools[timers,Agents]", "Inbox", "Repos", "Chat", "notes", "settings"), keys(layout))
    }

    fun testHiddenItemsDoNotShowInFoldersAndAnEmptyFolderGoes() {
        val layout = ShellBarLayout()
        val folderId = layout.createFolder("Tools", "timers")
        layout.hide("timers")
        assertEq(listOf("home", "Agents", "Inbox", "Repos", "Chat", "notes", "settings"), keys(layout))
        layout.hide("notes")
        layout.move("notes", folderId)
        expect(!layout.isHidden("notes"))
        assertEq(listOf("home", "Agents", "Inbox", "Repos", "Chat", "Tools[notes]", "settings"), keys(layout))
    }

    fun testFoldersGetFreshIdsRenameAndRemoveKeepingTheirItems() {
        val layout = ShellBarLayout()
        val first = layout.createFolder("A", "notes")
        val second = layout.createFolder("", "timers")
        expect(first != second)
        assertEq("Folder", layout.folder(second)?.name)
        layout.renameFolder(first, "x".repeat(80))
        assertEq(ShellBarLayout.NAME_LIMIT, layout.folder(first)?.name?.length)
        layout.removeFolder(first)
        val third = layout.createFolder("C")
        expect(second != third)
        assertEq(listOf("home", "Agents", "Inbox", "Repos", "Chat", "notes", "Folder[timers]", "settings"), keys(layout))
    }

    fun testSteppingAnItemRewritesTheOrder() {
        val layout = ShellBarLayout()
        layout.step("Chat", ShellBarStep.left, items, emptyList(), id)
        assertEq(listOf("home", "Agents", "Inbox", "Chat", "Repos", "notes", "timers", "settings"), keys(layout))
        layout.step("settings", ShellBarStep.first, items, emptyList(), id)
        assertEq("settings", keys(layout).first())
        layout.step("settings", ShellBarStep.last, items, emptyList(), id)
        assertEq("settings", keys(layout).last())
        expect(!layout.canStep("settings", ShellBarStep.right, items, emptyList(), id))
        expect(layout.canStep("settings", ShellBarStep.left, items, emptyList(), id))
        expect(!layout.canStep("home", ShellBarStep.first, items, emptyList(), id))
    }

    fun testSteppingAFolderMovesAllItsItemsAndHiddenItemsKeepTheirPlace() {
        val layout = ShellBarLayout()
        val folderId = layout.createFolder("Tools", "notes")
        layout.move("timers", folderId)
        layout.hide("Inbox")
        layout.step("folder:$folderId", ShellBarStep.first, items, emptyList(), id)
        assertEq(listOf("Tools[notes,timers]", "home", "Agents", "Repos", "Chat", "settings"), keys(layout))
        layout.show("Inbox")
        assertEq("Inbox", keys(layout).last())
        layout.step("timers", ShellBarStep.left, items, emptyList(), id)
        assertEq(listOf("timers", "notes"), layout.folder(folderId)?.items)
        expect(layout.canStep("notes", ShellBarStep.left, items, emptyList(), id))
        expect(!layout.canStep("timers", ShellBarStep.left, items, emptyList(), id))
    }

    fun testSteppingUnderARankingKeepsWhatWasShown() {
        val layout = ShellBarLayout()
        layout.step("Chat", ShellBarStep.left, items, listOf("settings", "notes"), id)
        assertEq(listOf("settings", "notes", "home", "Agents", "Inbox", "Chat", "Repos", "timers"), keys(layout))
    }

    fun testTheLayoutIsSavedAndAnEmptyOneClearsTheKey() {
        val store = MemoryPrefStore()
        val layout = ShellBarLayout()
        layout.hide("Chat")
        layout.createFolder("Tools", "notes")
        layout.save(store)
        assertEq(layout, ShellBarLayout.load(store))
        ShellBarLayout().save(store)
        expect(!store.contains(ShellBarLayout.KEY))
        store.putString(ShellBarLayout.KEY, "not json")
        assertEq(ShellBarLayout(), ShellBarLayout.load(store))
    }

    fun testTheSavedJsonUsesTheSharedFieldNames() {
        val store = MemoryPrefStore()
        val layout = ShellBarLayout()
        layout.createFolder("Tools", "notes")
        layout.hide("Chat")
        layout.fit(items, emptyList(), keepHome, ShellBarLayout.BAR_LIMIT, id)
        layout.save(store)
        val json = com.forkmesh.nativeapp.json.Json.parseObject(store.getString("shell/bar-layout")!!)
        assertEq(listOf("Chat"), json["hidden"])
        assertEq(listOf(mapOf("id" to "f1", "name" to "Tools", "items" to listOf("notes"))), json["folders"])
        assertEq(emptyList<Any?>(), json["order"])
        assertEq(listOf("home", "Agents", "Inbox", "Repos", "notes", "timers"), json["bar"])
        val plain = ShellBarLayout(hidden = listOf("Chat"))
        plain.save(store)
        expect(!com.forkmesh.nativeapp.json.Json.parseObject(store.getString("shell/bar-layout")!!).containsKey("bar"))
    }

    fun testASavedLayoutMissingAFieldOrWithTheWrongTypeLoadsEmpty() {
        val store = MemoryPrefStore()
        store.putString(ShellBarLayout.KEY, "{\"hidden\":[\"Chat\"],\"order\":[\"Inbox\"]}")
        assertEq(ShellBarLayout(), ShellBarLayout.load(store))
        store.putString(ShellBarLayout.KEY, "{\"hidden\":[\"Chat\"],\"folders\":[{\"id\":\"f1\"}],\"order\":[]}")
        assertEq(ShellBarLayout(), ShellBarLayout.load(store))
        store.putString(ShellBarLayout.KEY, "{\"hidden\":[],\"folders\":[],\"order\":[],\"bar\":\"home\"}")
        assertEq(ShellBarLayout(), ShellBarLayout.load(store))
        store.putString(ShellBarLayout.KEY, "{\"hidden\":[],\"folders\":[],\"order\":[],\"bar\":null}")
        assertEq(ShellBarLayout(), ShellBarLayout.load(store))
    }

    fun testTheBarHoldsSixSlotsAndKeepsHome() {
        val layout = ShellBarLayout()
        assertEq(listOf("home", "Agents", "Inbox", "Repos", "Chat", "notes"), bar(layout))
        expect(layout.barIsFull(items, emptyList(), keepHome, ShellBarLayout.BAR_LIMIT, id))
        expect(!layout.inBar("settings", items, emptyList(), keepHome, ShellBarLayout.BAR_LIMIT, id))
        assertEq(
            listOf("settings", "timers", "notes", "Chat", "Repos", "home"),
            bar(layout, listOf("settings", "timers", "notes", "Chat", "Repos", "Inbox")),
        )
        val hidden = ShellBarLayout()
        hidden.hide("home")
        assertEq("home", bar(hidden).first())
    }

    fun testRemovingFromTheBarDoesNotPullTheNextItemIn() {
        val layout = ShellBarLayout()
        layout.removeFromBar(listOf("Inbox", "home"), items, emptyList(), keepHome, ShellBarLayout.BAR_LIMIT, id)
        assertEq(listOf("home", "Agents", "Repos", "Chat", "notes"), bar(layout))
        expect(layout.isHidden("settings"))
        expect(!layout.isEmpty)
        expect(layout.putInBar("settings", null, items, emptyList(), keepHome, ShellBarLayout.BAR_LIMIT, id))
        assertEq(listOf("home", "Agents", "Repos", "Chat", "notes", "settings"), bar(layout))
        expect(!layout.putInBar("timers", null, items, emptyList(), keepHome, ShellBarLayout.BAR_LIMIT, id))
        expect(!layout.inBar("timers", items, emptyList(), keepHome, ShellBarLayout.BAR_LIMIT, id))
    }

    fun testAFullBarTakesAnItemInPlaceOfAnother() {
        val layout = ShellBarLayout()
        expect(layout.putInBar("settings", "Repos", items, emptyList(), keepHome, ShellBarLayout.BAR_LIMIT, id))
        assertEq(listOf("home", "Agents", "Inbox", "settings", "Chat", "notes"), bar(layout))
        expect(!layout.putInBar("timers", "home", items, emptyList(), keepHome, ShellBarLayout.BAR_LIMIT, id))
        expect(layout.putInBar("notes", "Agents", items, emptyList(), keepHome, ShellBarLayout.BAR_LIMIT, id))
        assertEq(listOf("home", "notes", "Agents", "Inbox", "settings", "Chat"), bar(layout))
        expect(layout.putInBar("home", "Chat", items, emptyList(), keepHome, ShellBarLayout.BAR_LIMIT, id))
        assertEq(listOf("notes", "Agents", "Inbox", "settings", "Chat", "home"), bar(layout))
    }

    fun testAFolderTakesOneSlotOfTheBar() {
        val layout = ShellBarLayout()
        val folderId = layout.createFolder("Tools", "timers")
        layout.move("settings", folderId)
        layout.fit(items, emptyList(), keepHome, ShellBarLayout.BAR_LIMIT, id)
        assertEq(listOf("home", "Agents", "Inbox", "Repos", "Chat", "notes"), bar(layout))
        expect(layout.putInBar("timers", "notes", items, emptyList(), keepHome, ShellBarLayout.BAR_LIMIT, id))
        assertEq(listOf("home", "Agents", "Inbox", "Repos", "Chat", "folder:$folderId"), bar(layout))
        expect(layout.inBar("timers", items, emptyList(), keepHome, ShellBarLayout.BAR_LIMIT, id))
        expect(!layout.inBar("settings", items, emptyList(), keepHome, ShellBarLayout.BAR_LIMIT, id))
    }

    fun testTheBarLimitIsAChoice() {
        val layout = ShellBarLayout()
        assertEq(listOf("home", "Agents", "Inbox", "Repos"), bar(layout, limit = 4))
        expect(layout.barIsFull(items, emptyList(), keepHome, 4, id))
        assertEq(items.size, layout.barSlots(items, emptyList(), keepHome, 8, id).size)
        expect(!layout.barIsFull(items.take(7), emptyList(), keepHome, 8, id))
        expect(layout.inBar("settings", items, emptyList(), keepHome, 8, id))
        expect(!layout.putInBar("Chat", null, items, emptyList(), keepHome, 4, id))
        expect(layout.putInBar("Chat", "Inbox", items, emptyList(), keepHome, 4, id))
        assertEq(listOf("home", "Agents", "Chat", "Repos"), bar(layout, limit = 4))
        assertEq(1, layout.barSlots(items, emptyList(), keepHome, 0, id).size)
    }

    fun testAnOldSavedLayoutWithoutABarStillLoads() {
        val store = MemoryPrefStore()
        store.putString(ShellBarLayout.KEY, "{\"hidden\":[\"Chat\"],\"folders\":[],\"order\":[\"Inbox\"]}")
        assertEq(ShellBarLayout(hidden = listOf("Chat"), order = listOf("Inbox")), ShellBarLayout.load(store))
        val layout = ShellBarLayout()
        layout.fit(items, emptyList(), keepHome, ShellBarLayout.BAR_LIMIT, id)
        layout.save(store)
        assertEq(listOf("home", "Agents", "Inbox", "Repos", "Chat", "notes"), ShellBarLayout.load(store).bar)
        layout.showAll()
        assertEq(null, layout.bar)
    }

    fun testCopiesDoNotShareTheirFolders() {
        val layout = ShellBarLayout()
        layout.createFolder("Tools", "notes")
        val copy = layout.copy()
        copy.move("timers", "f1")
        assertEq(listOf("notes"), layout.folder("f1")?.items)
        assertEq(listOf("notes", "timers"), copy.folder("f1")?.items)
    }
}
