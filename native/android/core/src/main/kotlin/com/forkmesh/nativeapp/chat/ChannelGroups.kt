package com.forkmesh.nativeapp.chat

import com.forkmesh.nativeapp.api.PrefStore
import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.json.jsonMaps
import com.forkmesh.nativeapp.json.jsonText

object ChannelGroups {
    const val MAX_GROUP_NAME_LENGTH = 48
    const val MAX_GROUPS = 64

    data class Group(
        val name: String,
        val channels: List<String> = emptyList(),
        val collapsed: Boolean = false,
        val automatic: Boolean = false,
    )

    data class State(val groups: List<Group> = emptyList(), val ungrouped: List<String> = emptyList()) {
        val isEmpty: Boolean get() = groups.isEmpty() && ungrouped.isEmpty()
    }

    data class Row(
        val group: String = "",
        val channel: String = "",
        val header: Boolean = false,
        val collapsed: Boolean = false,
        val automatic: Boolean = false,
    ) {
        val id: String get() = if (header) "group:$group" else "channel:$channel"
    }

    fun normalizeName(name: String): String =
        name.split(Regex("\\s+")).filter { it.isNotEmpty() }.joinToString(" ").take(MAX_GROUP_NAME_LENGTH)

    fun indexOfGroup(state: State, name: String): Int = state.groups.indexOfFirst { it.name == name }

    fun explicitGroupOf(state: State, channel: String): String = state.groups.firstOrNull { channel in it.channels }?.name ?: ""

    private fun detach(state: State, channel: String): State = State(
        state.groups.map { it.copy(channels = it.channels.filter { c -> c != channel }) },
        state.ungrouped.filter { it != channel },
    )

    fun createGroup(state: State, rawName: String): State? {
        val name = normalizeName(rawName)
        if (name.isEmpty() || indexOfGroup(state, name) >= 0 || state.groups.size >= MAX_GROUPS) return null
        return state.copy(groups = state.groups + Group(name))
    }

    private fun materialize(state: State, name: String, members: List<String>, automatic: Boolean): State {
        var groups = state.groups
        var index = groups.indexOfFirst { it.name == name }
        if (index < 0) {
            if (name.isEmpty() || groups.size >= MAX_GROUPS) return state
            groups = groups + Group(name, automatic = automatic)
            index = groups.size - 1
        }
        val memberSet = members.toSet()
        val at = index
        groups = groups.mapIndexed { i, g -> if (i == at) g.copy(channels = members) else g.copy(channels = g.channels.filter { it !in memberSet }) }
        return State(groups, state.ungrouped.filter { it !in memberSet })
    }

    fun renameGroup(state: State, from: String, to: String, members: List<String>): State? {
        val target = normalizeName(to)
        if (target.isEmpty() || target == from || indexOfGroup(state, target) >= 0) return null
        val next = materialize(state, from, members, automatic = false)
        val index = indexOfGroup(next, from)
        if (index < 0) return null
        return next.copy(groups = next.groups.mapIndexed { i, g -> if (i == index) g.copy(name = target, automatic = false) else g })
    }

    fun removeGroup(state: State, name: String, members: List<String>): State {
        var next = state.copy(groups = state.groups.filter { it.name != name })
        for (channel in members) {
            next = detach(next, channel)
            next = next.copy(ungrouped = next.ungrouped + channel)
        }
        return next
    }

    fun setCollapsed(state: State, name: String, collapsed: Boolean, automatic: Boolean): State {
        var groups = state.groups
        var index = groups.indexOfFirst { it.name == name }
        if (index < 0) {
            if (name.isEmpty() || groups.size >= MAX_GROUPS) return state
            groups = groups + Group(name, automatic = automatic)
            index = groups.size - 1
        }
        val at = index
        return state.copy(groups = groups.mapIndexed { i, g -> if (i == at) g.copy(collapsed = collapsed) else g })
    }

    fun assign(state: State, channel: String, group: String, index: Int, members: List<String>, automaticGroup: Boolean): State? {
        if (channel.isEmpty()) return null
        if (group.isEmpty()) {
            val next = detach(state, channel)
            return next.copy(ungrouped = next.ungrouped + channel)
        }
        var at = index
        val old = members.indexOf(channel)
        if (old >= 0 && at > old) at--
        var next = materialize(state, group, members.filter { it != channel }, automaticGroup)
        val groupIndex = indexOfGroup(next, group)
        if (groupIndex < 0) return null
        next = detach(next, channel)
        val channels = next.groups[groupIndex].channels.toMutableList()
        if (at < 0 || at > channels.size) at = channels.size
        channels.add(at, channel)
        return next.copy(groups = next.groups.mapIndexed { i, g -> if (i == groupIndex) g.copy(channels = channels) else g })
    }

    fun purge(state: State, channelMatches: (String) -> Boolean, groupMatches: (String) -> Boolean): State {
        val groups = ArrayList<Group>()
        for (g in state.groups) {
            val kept = g.channels.filterNot(channelMatches)
            if (g.automatic && kept.isEmpty() && groupMatches(g.name)) continue
            groups.add(g.copy(channels = kept))
        }
        return State(groups, state.ungrouped.filterNot(channelMatches))
    }

    fun toJson(state: State): String {
        if (state.isEmpty) return ""
        val groups = state.groups.map { group ->
            val entry = linkedMapOf<String, Any?>("name" to group.name)
            if (group.channels.isNotEmpty()) entry["channels"] = group.channels
            if (group.collapsed) entry["collapsed"] = true
            if (group.automatic) entry["automatic"] = true
            entry
        }
        val root = linkedMapOf<String, Any?>("groups" to groups)
        if (state.ungrouped.isNotEmpty()) root["ungrouped"] = state.ungrouped
        return Json.stringify(root)
    }

    fun fromJson(json: String): State {
        val root = try {
            Json.parseObject(json)
        } catch (_: Exception) {
            return State()
        }
        val groups = ArrayList<Group>()
        val seen = HashSet<String>()
        for (entry in jsonMaps(root["groups"])) {
            if (groups.size >= MAX_GROUPS) break
            val name = normalizeName(jsonText(entry["name"]))
            if (name.isEmpty() || groups.any { it.name == name }) continue
            val channels = ArrayList<String>()
            for (channel in (entry["channels"] as? List<*>).orEmpty().map(::jsonText)) {
                if (channel.isNotEmpty() && seen.add(channel)) channels.add(channel)
            }
            groups.add(Group(name, channels, entry["collapsed"] == true, entry["automatic"] == true))
        }
        val ungrouped = ArrayList<String>()
        for (channel in (root["ungrouped"] as? List<*>).orEmpty().map(::jsonText)) {
            if (channel.isNotEmpty() && seen.add(channel)) ungrouped.add(channel)
        }
        return State(groups, ungrouped)
    }

    private class Bucket(val name: String, val members: MutableList<String>, val collapsed: Boolean, val automatic: Boolean)

    fun layout(state: State, channels: List<String>, autoGroupFor: ((String) -> String)? = null): List<Row> {
        val present = channels.toSet()
        val buckets = state.groups.map { g -> Bucket(g.name, g.channels.filter { it in present }.toMutableList(), g.collapsed, g.automatic) }.toMutableList()
        val ungrouped = ArrayList<String>()
        for (channel in channels) {
            if (explicitGroupOf(state, channel).isNotEmpty()) continue
            val auto = if (autoGroupFor != null && channel !in state.ungrouped) normalizeName(autoGroupFor(channel)) else ""
            if (auto.isEmpty()) {
                ungrouped.add(channel)
                continue
            }
            val existing = buckets.firstOrNull { it.name == auto }
            if (existing != null) existing.members.add(channel) else buckets.add(Bucket(auto, mutableListOf(channel), false, true))
        }
        val rows = ArrayList<Row>(ungrouped.map { Row(channel = it) })
        for (bucket in buckets) {
            if (bucket.automatic && bucket.members.isEmpty()) continue
            rows.add(Row(group = bucket.name, header = true, collapsed = bucket.collapsed, automatic = bucket.automatic))
            for (channel in bucket.members) rows.add(Row(group = bucket.name, channel = channel, automatic = bucket.automatic))
        }
        return rows
    }

    fun renderedMembers(rows: List<Row>, group: String): List<String> =
        if (group.isEmpty()) emptyList() else rows.filter { !it.header && it.group == group }.map { it.channel }

    fun renderedGroupNames(rows: List<Row>): List<String> = rows.filter { it.header }.map { it.group }

    fun renderedGroupIsAutomatic(rows: List<Row>, group: String): Boolean = rows.firstOrNull { it.header && it.group == group }?.automatic ?: false

    fun visibleRows(rows: List<Row>): List<Row> {
        val collapsed = rows.filter { it.header && it.collapsed }.map { it.group }.toSet()
        return rows.filter { it.header || it.group.isEmpty() || it.group !in collapsed }
    }
}

class ChannelGroupsStore(private val prefs: PrefStore, scope: String = "", formerScopes: List<String> = emptyList()) {
    companion object {
        const val KEY = "chat/channelGroups"

        fun keyFor(scope: String): String = if (scope.isEmpty()) KEY else "$KEY/$scope"

        fun projectScope(projectId: String, room: String): String = "project-" + ChatHistoryCodec.scopeName(projectId, room)

        fun forProject(prefs: PrefStore, projectId: String, serverUrl: String, room: String): ChannelGroupsStore =
            ChannelGroupsStore(prefs, projectScope(projectId.ifEmpty { serverUrl }, room), listOf(ChatHistoryCodec.scopeName(serverUrl, room)))
    }

    private val scopedKey = keyFor(scope)

    init {
        if (!prefs.contains(scopedKey)) {
            val former = formerScopes.map(::keyFor).firstOrNull { it != scopedKey && prefs.getString(it) != null }
            if (former != null) {
                prefs.putString(scopedKey, prefs.getString(former)!!)
                prefs.remove(former)
            }
        }
    }

    @Volatile
    var state: ChannelGroups.State = ChannelGroups.fromJson(prefs.getString(scopedKey) ?: "")
        private set

    @Synchronized
    fun update(change: (ChannelGroups.State) -> ChannelGroups.State?): Boolean {
        val next = change(state) ?: return false
        if (next == state) return false
        state = next
        val json = ChannelGroups.toJson(next)
        if (json.isEmpty()) prefs.remove(scopedKey) else prefs.putString(scopedKey, json)
        return true
    }
}
