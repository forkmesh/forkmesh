package com.forkmesh.nativeapp.tools

import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.model.FundsReceivedEntry
import com.forkmesh.nativeapp.model.NetworkLeaderboards
import com.forkmesh.nativeapp.model.NetworkStats
import com.forkmesh.nativeapp.model.PayoutNode

sealed interface LoadState<out T> {
    data object Loading : LoadState<Nothing>

    data class Failed(val message: String) : LoadState<Nothing>

    data class Loaded<T>(val value: T) : LoadState<T>
}

object ActivityRules {
    const val INITIAL_LOG_COUNT = 12
    const val VISIBLE_PAYOUT_NODES = 5
    const val VISIBLE_FUNDS_ENTRIES = 3

    const val NETWORK_ERROR_TITLE = "Could not load network activity"
    const val FUNDS_ERROR_TITLE = "Could not load funds received"
    const val RETRY_TOOLTIP = "Retry network activity"
    const val LOADING_NETWORK = "Loading network activity"
    const val LOADING_FUNDS = "Loading funds received"
    const val PAYOUT_NOTE =
        "Public readiness only. Non-custodial: these addresses and balances are public-chain observations; ForkMesh does not hold node funds, store wallet keys, or execute payouts from mobile. Eligibility never guarantees selection or payment."
    const val FUNDS_NOTE =
        "Historical finalized public accounting only, not a ForkMesh or current wallet balance. User-owned funds, community-pool funds, pending allocations, and completed on-chain transfers are distinct states."

    fun loadStats(api: ApiService): LoadState<NetworkStats> = load { api.networkStats() }

    fun loadLeaderboards(api: ApiService): LoadState<NetworkLeaderboards> = load { api.networkLeaderboards() }

    inline fun <T> load(block: () -> T): LoadState<T> = try {
        LoadState.Loaded(block())
    } catch (error: Exception) {
        LoadState.Failed(error.toString())
    }

    fun visibleLogs(log: List<String>, showAll: Boolean): List<String> {
        val newestFirst = log.asReversed()
        return if (showAll) newestFirst.toList() else newestFirst.take(INITIAL_LOG_COUNT)
    }

    fun hasMoreLogs(log: List<String>): Boolean = log.size > INITIAL_LOG_COUNT

    fun toggleLabel(showAll: Boolean, count: Int): String = if (showAll) "Show less" else "Show all $count logs"

    fun eventsLabel(count: Int): String = "$count events"

    fun nodeName(node: PayoutNode): String = node.name.ifEmpty { "Unnamed node" }

    fun payoutDetails(node: PayoutNode): List<String> {
        val details = mutableListOf(node.eligibilityLabel)
        if (node.shortWallet.isNotEmpty()) details.add(node.shortWallet)
        if (node.balanceLamports > 0 || node.balanceSol > 0) details.add(node.balanceLabel)
        if (node.relay.trim().isNotEmpty()) details.add(node.relay.trim())
        return details
    }

    fun visiblePayoutNodes(nodes: List<PayoutNode>): List<PayoutNode> = nodes.take(VISIBLE_PAYOUT_NODES)

    fun moreNodesLabel(nodes: List<PayoutNode>): String? =
        if (nodes.size > VISIBLE_PAYOUT_NODES) "+${nodes.size - VISIBLE_PAYOUT_NODES} more nodes" else null

    fun visibleFunds(entries: List<FundsReceivedEntry>): List<FundsReceivedEntry> = entries.take(VISIBLE_FUNDS_ENTRIES)

    fun fundsName(entry: FundsReceivedEntry): String = entry.name.ifEmpty { "Unnamed" }
}

data class ActivityLogLine(val timestamp: String, val body: String, val tone: Tone) {
    enum class Tone { Failure, Connected, Left, Event }

    companion object {
        fun parse(line: String): ActivityLogLine {
            val timestamp = if (line.length >= 8) line.substring(0, 8) else ""
            val body = if (line.length > 10) line.substring(10).trim() else line
            val lower = body.lowercase()
            val tone = when {
                lower.contains("failed") || lower.contains("error") -> Tone.Failure
                lower.contains("connected") -> Tone.Connected
                lower.contains("left") -> Tone.Left
                else -> Tone.Event
            }
            return ActivityLogLine(timestamp, body, tone)
        }
    }
}
