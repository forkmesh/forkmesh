package com.forkmesh.nativeapp.shell

import com.forkmesh.nativeapp.api.PrefStore

class ShellBarPrefs(private val store: PrefStore, val keep: List<String> = listOf(HOME_ID)) {
    private val order = ShellBarOrder(store)

    var sort: ShellBarSort = order.sort
        private set

    var recent: List<String> = order.recent
        private set

    var uses: Map<String, Int> = order.uses
        private set

    var layout: ShellBarLayout = ShellBarLayout.load(store)
        private set

    var home: ShellHomeStyle = ShellHomeStyle.load(store)
        private set

    var barStyle: ShellBarStyle = ShellBarStyle.load(store)
        private set

    var menu: ShellMenuLayout = ShellMenuLayout.load(store)
        private set

    fun <Group, Item> arranged(groups: List<Group>, group: (Group) -> String, items: (Group) -> List<Item>, id: (Item) -> String): List<Pair<Group, List<Item>>> =
        groups.zip(menu.arranged(groups, group, items, id)).filter { it.second.isNotEmpty() }

    fun moveInMenu(itemId: String, to: String, near: String?, current: List<Pair<String, List<String>>>): Boolean {
        val lists = if (current.any { it.first == to }) current else current + (to to emptyList())
        val next = menu.moved(itemId, to, near, lists)
        if (next == menu) return false
        menu = next
        next.save(store)
        return true
    }

    fun resetMenu() {
        if (menu.isEmpty) return
        menu = ShellMenuLayout()
        menu.save(store)
    }

    val ranking: List<String> get() = ShellBarOrder.ranking(sort, recent, uses)

    private val limit: Int get() = barStyle.capacity

    fun styleBar(edit: ShellBarStyle.() -> Unit) {
        val draft = barStyle.copy()
        draft.edit()
        val next = draft.clamped
        if (next == barStyle) return
        barStyle = next
        next.save(store)
    }

    fun styleHome(edit: ShellHomeStyle.() -> Unit) {
        val next = home.copy()
        next.edit()
        next.across = ShellHomeStyle.clampedAcross(next.across)
        if (next == home) return
        home = next
        next.save(store)
    }

    fun setSort(next: ShellBarSort) {
        order.sort = next
        sort = next
    }

    fun use(id: String) {
        order.use(id)
        refresh()
    }

    fun visit(ids: List<String>) {
        if (!order.visit(ids)) return
        refresh()
    }

    fun clear() {
        order.clear()
        refresh()
    }

    private fun refresh() {
        recent = order.recent
        uses = order.uses
    }

    fun <Item> slots(items: List<Item>, id: (Item) -> String): List<ShellBarSlot<Item>> =
        layout.barSlots(items, ranking, keep, limit, id)

    fun <Item> inBar(itemId: String, items: List<Item>, id: (Item) -> String): Boolean =
        layout.inBar(itemId, items, ranking, keep, limit, id)

    fun <Item> barIds(items: List<Item>, id: (Item) -> String): Set<String> {
        val ids = LinkedHashSet<String>()
        for (slot in layout.barSlots(items, ranking, keep, limit, id)) {
            when (slot) {
                is ShellBarSlot.Single -> ids.add(id(slot.item))
                is ShellBarSlot.Folder -> ids.addAll(slot.members.map(id))
            }
        }
        return ids
    }

    fun <Item> barIsFull(items: List<Item>, id: (Item) -> String): Boolean =
        layout.barIsFull(items, ranking, keep, limit, id)

    fun <Item> removeFromBar(ids: List<String>, items: List<Item>, id: (Item) -> String) {
        val current = ranking
        val room = limit
        arrange { removeFromBar(ids, items, current, keep, room, id) }
    }

    fun <Item> putInBar(itemId: String, at: String? = null, items: List<Item>, id: (Item) -> String): Boolean {
        val current = ranking
        val next = layout.copy()
        if (!next.putInBar(itemId, at, items, current, keep, limit, id)) return false
        commit(next)
        if (at != null && sort != ShellBarSort.usual) setSort(ShellBarSort.usual)
        return true
    }

    fun arrange(edit: ShellBarLayout.() -> Unit) {
        val next = layout.copy()
        next.edit()
        commit(next)
    }

    private fun commit(next: ShellBarLayout) {
        if (next == layout) return
        layout = next
        next.save(store)
    }

    private fun <Item> fitted(items: List<Item>, id: (Item) -> String): ShellBarLayout {
        val next = layout.copy()
        next.fit(items, ranking, keep, limit, id)
        return next
    }

    fun <Item> canStep(key: String, step: ShellBarStep, items: List<Item>, id: (Item) -> String): Boolean =
        fitted(items, id).canStep(key, step, items, ranking, id)

    fun <Item> step(key: String, step: ShellBarStep, items: List<Item>, id: (Item) -> String) {
        val current = ranking
        val next = fitted(items, id)
        next.step(key, step, items, current, id)
        commit(next)
        if (sort != ShellBarSort.usual) setSort(ShellBarSort.usual)
    }

    companion object {
        const val HOME_ID = "home"
    }
}
