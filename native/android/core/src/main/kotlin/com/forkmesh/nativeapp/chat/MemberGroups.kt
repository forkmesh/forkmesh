package com.forkmesh.nativeapp.chat

import com.forkmesh.nativeapp.model.Member
import com.forkmesh.nativeapp.model.MemberGroup

fun memberGroupsOf(members: List<Member>): List<MemberGroup> {
    val byKey = LinkedHashMap<String, MutableList<Member>>()
    for (m in members) byKey.getOrPut(identityKey(m)) { ArrayList() }.add(m)
    val nameCounts = HashMap<String, Int>()
    for (nodes in byKey.values) {
        val key = displayName(nodes.first()).lowercase()
        nameCounts[key] = (nameCounts[key] ?: 0) + 1
    }
    val groups = ArrayList<MemberGroup>()
    for (nodes in byKey.values) {
        nodes.sortWith(::compareNodes)
        val primary = nodes.first()
        val groupName = displayName(primary)
        val collides = (nameCounts[groupName.lowercase()] ?: 0) > 1
        groups.add(
            MemberGroup(
                name = groupName,
                members = nodes.toList(),
                self = nodes.any { it.self },
                disambiguator = if (collides) shortIdentity(primary) else "",
            ),
        )
    }
    return groups
}

private fun displayName(m: Member): String {
    if (dartTrimText(m.owner).isNotEmpty()) return dartTrimText(m.owner)
    if (dartTrimText(m.name).isNotEmpty()) return dartTrimText(m.name)
    if (dartTrimText(m.nodeName).isNotEmpty()) return dartTrimText(m.nodeName)
    return m.id
}

private fun identityKey(m: Member): String {
    val owner = dartTrimText(m.owner).lowercase()
    if (owner.isNotEmpty()) return "owner:$owner"
    if (m.self) return "self"
    val wallet = dartTrimText(m.solanaAddress)
    if (wallet.isNotEmpty()) return "wallet:$wallet"
    val display = displayName(m).lowercase()
    if (display.isNotEmpty()) return "name:$display"
    return "node:${m.id}"
}

private fun shortIdentity(m: Member): String {
    val raw = if (dartTrimText(m.solanaAddress).isNotEmpty()) dartTrimText(m.solanaAddress) else m.id
    if (raw.length <= 10) return raw
    return "${raw.substring(0, 4)}…${raw.substring(raw.length - 4)}"
}

private fun compareNodes(a: Member, b: Member): Int {
    if (a.self != b.self) return if (a.self) -1 else 1
    if (a.online != b.online) return if (a.online) -1 else 1
    return a.id.compareTo(b.id)
}
