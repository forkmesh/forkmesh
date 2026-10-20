package com.forkmesh.nativeapp.shell

import com.forkmesh.nativeapp.api.PrefStore

data class ShellMenuLayout(val groups: Map<String, List<String>> = emptyMap()) {
    val isEmpty: Boolean get() = groups.isEmpty()

    fun save(store: PrefStore) {
        ShellDecode.write(store, KEY, if (isEmpty) null else mapOf("groups" to groups))
    }

    fun <Group, Item> arranged(defaults: List<Group>, group: (Group) -> String, items: (Group) -> List<Item>, id: (Item) -> String): List<List<Item>> {
        val present = defaults.map(group).toSet()
        val home = HashMap<String, String>()
        for ((key, ids) in groups) {
            if (key !in present) continue
            for (itemId in ids) home.putIfAbsent(itemId, key)
        }
        val byId = HashMap<String, Item>()
        val loose = HashMap<String, MutableList<Item>>()
        for (entry in defaults) {
            val key = group(entry)
            for (item in items(entry)) {
                val itemId = id(item)
                if (byId.containsKey(itemId)) continue
                byId[itemId] = item
                if (!home.containsKey(itemId)) loose.getOrPut(key) { mutableListOf() }.add(item)
            }
        }
        return defaults.map { entry ->
            val key = group(entry)
            val emitted = HashSet<String>()
            val placed = mutableListOf<Item>()
            for (itemId in groups[key].orEmpty()) {
                if (home[itemId] != key || itemId in emitted) continue
                val item = byId[itemId] ?: continue
                emitted += itemId
                placed += item
            }
            placed + loose[key].orEmpty()
        }
    }

    fun moved(itemId: String, to: String, near: String?, current: List<Pair<String, List<String>>>): ShellMenuLayout {
        val lists = current.map { it.first to it.second.toMutableList() }.toMutableList()
        var fromGroup = -1
        var fromIndex = -1
        lists.forEachIndexed { groupIndex, entry ->
            val index = entry.second.indexOf(itemId)
            if (index >= 0) {
                fromGroup = groupIndex
                fromIndex = index
                entry.second.removeAt(index)
            }
        }
        val into = lists.indexOfFirst { it.first == to }
        if (into < 0) return this
        val target = lists[into].second
        var insert = target.size
        if (near != null && near != itemId) {
            val at = target.indexOf(near)
            if (at >= 0) insert = if (fromGroup == into && fromIndex <= at) at + 1 else at
        }
        target.add(insert, itemId)
        val shown = lists.flatMap { it.second }.toSet()
        val next = LinkedHashMap<String, MutableList<String>>()
        for ((key, ids) in lists) next[key] = ids
        for ((key, ids) in groups) {
            val list = next[key] ?: mutableListOf()
            ids.forEachIndexed { index, kept ->
                if (kept in shown || kept in list) return@forEachIndexed
                val after = ids.subList(0, index).asReversed().map { list.indexOf(it) }.firstOrNull { it >= 0 }
                list.add(if (after != null) after + 1 else 0, kept)
            }
            if (list.isNotEmpty()) next[key] = list
        }
        return ShellMenuLayout(next.filterValues { it.isNotEmpty() }.mapValues { it.value.toList() })
    }

    companion object {
        const val KEY = "shell/menu-layout"

        fun load(store: PrefStore): ShellMenuLayout {
            val json = ShellDecode.parseObject(store.getString(KEY)) ?: return ShellMenuLayout()
            val raw = json["groups"] as? Map<*, *> ?: return ShellMenuLayout()
            val groups = LinkedHashMap<String, List<String>>()
            for ((key, value) in raw) {
                val ids = (value as? List<*>)?.mapNotNull { it as? String } ?: continue
                if (key is String) groups[key] = ids
            }
            return ShellMenuLayout(groups)
        }
    }
}
