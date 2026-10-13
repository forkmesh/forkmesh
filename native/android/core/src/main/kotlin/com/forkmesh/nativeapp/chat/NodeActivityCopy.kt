package com.forkmesh.nativeapp.chat

import com.forkmesh.nativeapp.format.relativeTimeFrom

enum class NodeActivityTone { TERTIARY, SUCCESS, WARNING }

data class NodeActivityCard(
    val badge: String,
    val badgeTone: NodeActivityTone,
    val headline: String,
    val body: String,
    val tokens: List<String>,
    val lastError: String,
    val needsDesktopDetail: String?,
    val applierTokens: List<String>,
    val appliedNote: String?,
    val leftReasons: List<String>,
    val syncing: Boolean,
) {
    val syncLabel: String get() = if (syncing) "Syncing…" else "Sync now"

    companion object {
        const val TITLE = "Node activity"
        const val SYNC_TOOLTIP = "Run one signed sync now"
    }
}

fun nodeActivityCard(
    node: NodeSyncSnapshot?,
    noDesktop: Boolean,
    applier: NodeSyncApplierSummary?,
    nowMillis: Long = System.currentTimeMillis(),
): NodeActivityCard? {
    if (node == null || node.mode == NodeSyncMode.DISABLED) return null
    val active = node.mode == NodeSyncMode.ACTIVE
    val live = node.connection == NodeSyncConnection.LIVE
    val counts = node.counts
    val hostBound = listOf(NodeSyncKind.AGENT_PROMPTS, NodeSyncKind.ABOUT, NodeSyncKind.BRANCH_OPS)
        .mapNotNull { kind -> (counts[kind] ?: 0).takeIf { it > 0 }?.let { kind to it } }

    val tokens = ArrayList<String>()
    val lastSyncAt = node.lastSyncAt
    tokens.add(if (lastSyncAt == null) "Never synced" else "Synced ${relativeTimeFrom(lastSyncAt, nowMillis)}")
    tokens.add(if (node.pendingTotal == 0) "No pending items" else "${node.pendingTotal} pending")
    for ((kind, count) in counts) if (count > 0) tokens.add("$count ${kind.label(count)}")
    if (node.ackedTotal > 0) tokens.add("${node.ackedTotal} applied")

    val needsDesktop = if (hostBound.isNotEmpty() && noDesktop) {
        "${hostBound.joinToString(" and ") { (kind, count) -> "$count ${kind.label(count)}" }} " +
            "reached this phone but need a host to apply. They are " +
            "held here and never acknowledged, so a desktop node can " +
            "still pick the work up."
    } else {
        null
    }

    val showApplier = applier != null && (applier.appliedTotal > 0 || applier.leftTotal > 0)
    val applierTokens = ArrayList<String>()
    var appliedNote: String? = null
    val reasons = ArrayList<String>()
    if (showApplier) {
        if (applier.appliedTotal > 0) applierTokens.add("${applier.appliedTotal} committed here")
        if (applier.leftTotal > 0) applierTokens.add("${applier.leftTotal} left for a desktop")
        if (applier.appliedTotal > 0) {
            appliedNote = "Applied items are committed to this device only. Push the repository to publish them."
        }
        for ((reason, count) in applier.leftReasons.take(3)) {
            reasons.add("· $reason${if (count > 1) " (×$count)" else ""}")
        }
    }

    return NodeActivityCard(
        badge = when {
            !active -> "standby"
            live -> "live"
            node.connection == NodeSyncConnection.CONNECTING -> "connecting"
            else -> "offline"
        },
        badgeTone = when {
            !active -> NodeActivityTone.TERTIARY
            live -> NodeActivityTone.SUCCESS
            else -> NodeActivityTone.WARNING
        },
        headline = if (active) "This device is acting as a node" else "A desktop node is handling this account",
        body = if (active) {
            "It signs as ${node.owner} and reads the account inbox " +
                "while the app is open. Nothing runs in the background."
        } else {
            "The desktop node drains this account's inbox. This " +
                "phone stays out of the way so the two never claim " +
                "the same items."
        },
        tokens = tokens,
        lastError = node.lastError,
        needsDesktopDetail = needsDesktop,
        applierTokens = applierTokens,
        appliedNote = appliedNote,
        leftReasons = reasons,
        syncing = node.syncing,
    )
}
