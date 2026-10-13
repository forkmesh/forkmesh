package com.forkmesh.nativeapp.model

import com.forkmesh.nativeapp.json.jsonMaps

data class NetworkStats(
    val nodesOnline: Int = 0,
    val hostsOnline: Int = 0,
    val repos: Int = 0,
    val payoutNodes: List<PayoutNode> = emptyList(),
) {
    companion object {
        fun fromJson(j: Map<String, Any?>): NetworkStats {
            val onlineNodes = j["onlineNodes"]
            val nodesOnline = if (onlineNodes is List<*>) {
                onlineNodes.size
            } else {
                modelInt(j["nodesOnline"] ?: j["clients"] ?: 0L).toInt()
            }
            val payoutRaw = when {
                j["payoutNodes"] is List<*> -> j["payoutNodes"]
                j["payout_nodes"] is List<*> -> j["payout_nodes"]
                else -> emptyList<Any?>()
            }
            return NetworkStats(
                nodesOnline = nodesOnline,
                hostsOnline = modelInt(j["hostsOnline"] ?: j["hosts"] ?: 0L).toInt(),
                repos = modelInt(j["repos"] ?: j["repositories"] ?: 0L).toInt(),
                payoutNodes = jsonMaps(payoutRaw).map { PayoutNode.fromJson(LinkedHashMap(it)) },
            )
        }
    }
}

data class PayoutNode(
    val name: String = "",
    val wallet: String = "",
    val balanceLamports: Long = 0,
    val balanceSol: Double = 0.0,
    val online: Boolean = false,
    val payoutEligible: Boolean = false,
    val eligibilityReason: String = "",
    val relay: String = "",
) {
    val shortWallet: String
        get() {
            val clean = wallet.trim()
            if (clean.length <= 16) return clean
            return clean.substring(0, 6) + "..." + clean.substring(clean.length - 6)
        }

    val eligibilityLabel: String
        get() {
            val reason = eligibilityReason.trim()
            if (reason.isNotEmpty()) return reason.replace('_', ' ').replace('-', ' ')
            return if (payoutEligible) "eligible" else "not eligible"
        }

    val balanceLabel: String
        get() {
            if (balanceSol > 0) return "${modelTrimDouble(balanceSol)} SOL ($balanceLamports lamports)"
            return "$balanceLamports lamports"
        }

    companion object {
        fun fromJson(json: Map<String, Any?>): PayoutNode = PayoutNode(
            name = firstModelString(json, listOf("name", "node", "id")),
            wallet = firstModelString(json, listOf("wallet", "walletAddress", "payoutWallet", "address")),
            balanceLamports = modelInt(json["balanceLamports"]),
            balanceSol = modelDouble(json["balanceSol"]),
            online = modelBool(json["online"]),
            payoutEligible = modelBool(json["payoutEligible"]),
            eligibilityReason = firstModelString(json, listOf("eligibilityReason", "reason")),
            relay = firstModelString(json, listOf("relay")),
        )
    }
}

data class NetworkLeaderboards(
    val fundsMainnodes: List<FundsReceivedEntry> = emptyList(),
    val fundsContributors: List<FundsReceivedEntry> = emptyList(),
    val fundsProjects: List<FundsReceivedEntry> = emptyList(),
) {
    val isEmpty: Boolean
        get() = fundsMainnodes.isEmpty() && fundsContributors.isEmpty() && fundsProjects.isEmpty()

    companion object {
        fun fromJson(json: Map<String, Any?>): NetworkLeaderboards = NetworkLeaderboards(
            fundsMainnodes = fundsReceivedList(json["fundsMainnodes"]),
            fundsContributors = fundsReceivedList(json["fundsContributors"]),
            fundsProjects = fundsReceivedList(json["fundsProjects"]),
        )

        private fun fundsReceivedList(raw: Any?): List<FundsReceivedEntry> =
            jsonMaps(raw).map { FundsReceivedEntry.fromJson(LinkedHashMap(it)) }
    }
}

data class FundsReceivedEntry(
    val name: String = "",
    val lamports: Long = 0,
    val sol: Double = 0.0,
) {
    val amountLabel: String
        get() {
            if (sol > 0) return "${modelTrimDouble(sol)} SOL ($lamports lamports)"
            return "$lamports lamports"
        }

    companion object {
        fun fromJson(json: Map<String, Any?>): FundsReceivedEntry = FundsReceivedEntry(
            name = firstModelString(json, listOf("name", "node", "account", "project", "repo")),
            lamports = modelInt(json["lamports"]),
            sol = modelDouble(json["sol"]),
        )
    }
}
