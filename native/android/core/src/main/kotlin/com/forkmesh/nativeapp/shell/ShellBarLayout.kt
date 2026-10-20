package com.forkmesh.nativeapp.shell

import com.forkmesh.nativeapp.api.PrefStore
import kotlin.math.max
import kotlin.math.min

data class ShellBarFolder(val id: String, val name: String, val items: List<String> = emptyList()) {
    val slotId: String get() = ShellBarLayout.FOLDER_PREFIX + id
}

sealed class ShellBarSlot<out Item> {
    data class Single<out Item>(val item: Item) : ShellBarSlot<Item>()

    data class Folder<out Item>(val folder: ShellBarFolder, val members: List<Item>) : ShellBarSlot<Item>()
}

enum class ShellBarStep { left, right, first, last }

data class ShellBarLayout(
    var hidden: List<String> = emptyList(),
    var folders: List<ShellBarFolder> = emptyList(),
    var order: List<String> = emptyList(),
    var bar: List<String>? = null,
) {
    val isEmpty: Boolean get() = hidden.isEmpty() && folders.isEmpty() && order.isEmpty() && bar == null

    fun save(store: PrefStore) {
        ShellDecode.write(store, KEY, if (isEmpty) null else toJson())
    }

    fun toJson(): Map<String, Any?> {
        val json = linkedMapOf<String, Any?>(
            "hidden" to hidden,
            "folders" to folders.map { linkedMapOf("id" to it.id, "name" to it.name, "items" to it.items) },
            "order" to order,
        )
        bar?.let { json["bar"] = it }
        return json
    }

    private fun adopt(other: ShellBarLayout) {
        hidden = other.hidden
        folders = other.folders
        order = other.order
        bar = other.bar
    }

    fun isHidden(id: String): Boolean {
        if (hidden.contains(id)) return true
        val current = bar ?: return false
        return !current.contains(id)
    }

    fun folderOf(id: String): ShellBarFolder? = folders.firstOrNull { it.items.contains(id) }

    fun folder(id: String): ShellBarFolder? = folders.firstOrNull { it.id == id }

    fun hide(id: String) {
        if (!hidden.contains(id)) hidden = hidden + id
        bar = bar?.filter { it != id }
    }

    fun show(id: String) {
        hidden = hidden.filter { it != id }
        val current = bar
        if (current != null && !current.contains(id)) bar = current + id
    }

    fun showAll() {
        hidden = emptyList()
        bar = null
    }

    fun move(id: String, toFolder: String?) {
        folders = folders.map { it.copy(items = it.items.filter { item -> item != id }) }
        val index = folders.indexOfFirst { it.id == toFolder }
        if (toFolder == null || index < 0) return
        folders = folders.mapIndexed { at, folder -> if (at == index) folder.copy(items = folder.items + id) else folder }
        show(id)
    }

    fun createFolder(name: String, with: String? = null): String {
        var number = folders.size + 1
        while (folders.any { it.id == "f$number" }) number++
        val folderId = "f$number"
        folders = folders + ShellBarFolder(folderId, cleanName(name))
        if (with != null) move(with, folderId)
        return folderId
    }

    fun renameFolder(folderId: String, to: String) {
        if (folders.none { it.id == folderId }) return
        folders = folders.map { if (it.id == folderId) it.copy(name = cleanName(to)) else it }
    }

    fun removeFolder(folderId: String) {
        folders = folders.filter { it.id != folderId }
    }

    fun <Item> slots(items: List<Item>, ranking: List<String>, id: (Item) -> String): List<ShellBarSlot<Item>> {
        val placed = ShellBarOrder.ordered(arranged(items, order, id), ranking, id)
        val visible = placed.filter { !isHidden(id(it)) }
        val byId = HashMap<String, Item>()
        for (item in visible) if (!byId.containsKey(id(item))) byId[id(item)] = item
        val emitted = HashSet<String>()
        val result = ArrayList<ShellBarSlot<Item>>()
        for (item in visible) {
            val folder = folderOf(id(item))
            if (folder == null) {
                result.add(ShellBarSlot.Single(item))
                continue
            }
            if (!emitted.add(folder.id)) continue
            result.add(ShellBarSlot.Folder(folder, folder.items.mapNotNull { byId[it] }))
        }
        return result
    }

    fun <Item> step(key: String, step: ShellBarStep, items: List<Item>, ranking: List<String>, id: (Item) -> String) {
        val all = slots(items, ranking, id)
        val folderIndex = folders.indexOfFirst { it.items.contains(key) }
        if (folderIndex >= 0) {
            val inside = members(folders[folderIndex].id, all, id).toMutableList()
            val from = inside.indexOf(key)
            if (from < 0) return
            inside.removeAt(from)
            inside.add(target(from, inside.size + 1, step), key)
            val rest = folders[folderIndex].items.filter { !inside.contains(it) }
            folders = folders.mapIndexed { at, folder -> if (at == folderIndex) folder.copy(items = inside + rest) else folder }
            return
        }
        val keys = all.map { slotId(it, id) }.toMutableList()
        val from = keys.indexOf(key)
        if (from < 0) return
        keys.removeAt(from)
        keys.add(target(from, keys.size + 1, step), key)
        write(keys, items, ranking, id)
    }

    private fun <Item> write(keys: List<String>, items: List<Item>, ranking: List<String>, id: (Item) -> String) {
        val expanded = ArrayList<String>()
        for (key in keys) {
            val folder = if (key.startsWith(FOLDER_PREFIX)) folder(key.substring(FOLDER_PREFIX.length)) else null
            if (folder != null) expanded.addAll(folder.items) else expanded.add(key)
        }
        val rest = ShellBarOrder.ordered(arranged(items, order, id), ranking, id).map(id).filter { !expanded.contains(it) }
        order = expanded + rest
    }

    fun <Item> canStep(key: String, step: ShellBarStep, items: List<Item>, ranking: List<String>, id: (Item) -> String): Boolean {
        val all = slots(items, ranking, id)
        val folder = folderOf(key)
        val keys = if (folder != null) members(folder.id, all, id) else all.map { slotId(it, id) }
        val index = keys.indexOf(key)
        if (index < 0) return false
        return when (step) {
            ShellBarStep.left, ShellBarStep.first -> index > 0
            ShellBarStep.right, ShellBarStep.last -> index < keys.size - 1
        }
    }

    fun <Item> barSlots(
        items: List<Item>,
        ranking: List<String>,
        keep: List<String> = emptyList(),
        limit: Int = BAR_LIMIT,
        id: (Item) -> String,
    ): List<ShellBarSlot<Item>> {
        val shown = copy()
        for (kept in keep) shown.show(kept)
        val all = shown.slots(items, ranking, id)
        val cap = max(1, limit)
        if (all.size <= cap) return all
        val pinned = all.map { slot -> keep.any { holds(slot, it, id) } }
        var room = cap - pinned.count { it }
        val result = ArrayList<ShellBarSlot<Item>>()
        all.forEachIndexed { index, slot ->
            if (pinned[index]) {
                result.add(slot)
            } else if (room > 0) {
                result.add(slot)
                room--
            }
        }
        return result
    }

    fun <Item> inBar(
        itemId: String,
        items: List<Item>,
        ranking: List<String>,
        keep: List<String> = emptyList(),
        limit: Int = BAR_LIMIT,
        id: (Item) -> String,
    ): Boolean = barSlots(items, ranking, keep, limit, id).any { holds(it, itemId, id) }

    fun <Item> barIsFull(
        items: List<Item>,
        ranking: List<String>,
        keep: List<String> = emptyList(),
        limit: Int = BAR_LIMIT,
        id: (Item) -> String,
    ): Boolean = barSlots(items, ranking, keep, limit, id).size >= max(1, limit)

    fun <Item> fit(
        items: List<Item>,
        ranking: List<String>,
        keep: List<String> = emptyList(),
        limit: Int = BAR_LIMIT,
        id: (Item) -> String,
    ) {
        val members = ArrayList<String>()
        for (slot in barSlots(items, ranking, keep, limit, id)) {
            when (slot) {
                is ShellBarSlot.Single -> members.add(id(slot.item))
                is ShellBarSlot.Folder -> members.addAll(slot.members.map(id))
            }
        }
        val present = items.map(id).toSet()
        bar = members + (bar ?: emptyList()).filter { !present.contains(it) && !members.contains(it) }
        for (kept in keep) hidden = hidden.filter { it != kept }
    }

    fun <Item> removeFromBar(
        ids: List<String>,
        items: List<Item>,
        ranking: List<String>,
        keep: List<String> = emptyList(),
        limit: Int = BAR_LIMIT,
        id: (Item) -> String,
    ) {
        fit(items, ranking, keep, limit, id)
        for (itemId in ids) {
            if (!keep.contains(itemId)) bar = bar?.filter { it != itemId }
        }
    }

    fun <Item> putInBar(
        itemId: String,
        at: String? = null,
        items: List<Item>,
        ranking: List<String>,
        keep: List<String> = emptyList(),
        limit: Int = BAR_LIMIT,
        id: (Item) -> String,
    ): Boolean {
        val next = copy()
        next.fit(items, ranking, keep, limit, id)
        val before = next.slots(items, ranking, id)
        val targetIndex = at?.let { key -> before.indexOfFirst { slotId(it, id) == key }.takeIf { it >= 0 } }
        if (before.none { holds(it, itemId, id) }) {
            next.show(itemId)
            if (next.slots(items, ranking, id).size > max(1, limit)) {
                if (targetIndex == null) return false
                val replaced = before[targetIndex]
                if (keep.any { holds(replaced, it, id) }) return false
                val gone = when (replaced) {
                    is ShellBarSlot.Single -> listOf(id(replaced.item))
                    is ShellBarSlot.Folder -> replaced.members.map(id)
                }
                next.bar = next.bar?.filter { !gone.contains(it) }
            }
        }
        if (targetIndex != null) {
            val slotKey = next.folderOf(itemId)?.slotId ?: itemId
            val keys = next.slots(items, ranking, id).map { slotId(it, id) }.toMutableList()
            val from = keys.indexOf(slotKey)
            if (from >= 0) {
                keys.removeAt(from)
                keys.add(min(targetIndex, keys.size), slotKey)
                next.write(keys, items, ranking, id)
            }
        }
        adopt(next)
        return true
    }

    companion object {
        const val KEY = "shell/bar-layout"
        const val FOLDER_PREFIX = "folder:"
        const val NAME_LIMIT = 40
        const val BAR_LIMIT = 6

        @Suppress("UNCHECKED_CAST")
        fun fromJson(values: Map<String, Any?>): ShellBarLayout? {
            val hidden = strings(values["hidden"]) ?: return null
            val order = strings(values["order"]) ?: return null
            val rawFolders = values["folders"] as? List<*> ?: return null
            val folders = rawFolders.map { raw ->
                val folder = raw as? Map<String, Any?> ?: return null
                val id = folder["id"] as? String ?: return null
                val name = folder["name"] as? String ?: return null
                val items = strings(folder["items"]) ?: return null
                ShellBarFolder(id, name, items)
            }
            val bar = if (values["bar"] == null) null else strings(values["bar"]) ?: return null
            return ShellBarLayout(hidden, folders, order, bar)
        }

        private fun strings(value: Any?): List<String>? {
            val list = value as? List<*> ?: return null
            if (list.any { it !is String }) return null
            return list.map { it as String }
        }

        fun load(store: PrefStore): ShellBarLayout =
            ShellDecode.parseObject(store.getString(KEY))?.let(::fromJson) ?: ShellBarLayout()

        fun cleanName(name: String): String {
            val trimmed = name.trim()
            return if (trimmed.isEmpty()) "Folder" else trimmed.take(NAME_LIMIT)
        }

        fun <Item> arranged(items: List<Item>, order: List<String>, id: (Item) -> String): List<Item> =
            ShellBarOrder.ordered(items, order, id)

        fun <Item> slotId(slot: ShellBarSlot<Item>, id: (Item) -> String): String = when (slot) {
            is ShellBarSlot.Single -> id(slot.item)
            is ShellBarSlot.Folder -> slot.folder.slotId
        }

        fun <Item> members(folderId: String, slots: List<ShellBarSlot<Item>>, id: (Item) -> String): List<String> {
            for (slot in slots) {
                if (slot is ShellBarSlot.Folder && slot.folder.id == folderId) return slot.members.map(id)
            }
            return emptyList()
        }

        fun target(index: Int, count: Int, step: ShellBarStep): Int = when (step) {
            ShellBarStep.left -> max(0, index - 1)
            ShellBarStep.right -> min(count - 1, index + 1)
            ShellBarStep.first -> 0
            ShellBarStep.last -> count - 1
        }

        fun <Item> holds(slot: ShellBarSlot<Item>, itemId: String, id: (Item) -> String): Boolean = when (slot) {
            is ShellBarSlot.Single -> id(slot.item) == itemId
            is ShellBarSlot.Folder -> slot.members.any { id(it) == itemId }
        }
    }
}
