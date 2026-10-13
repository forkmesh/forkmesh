package com.forkmesh.nativeapp.model

import com.forkmesh.nativeapp.api.DartUri
import com.forkmesh.nativeapp.json.asJsonMap

const val SOLANA_EXPLORER_BASE_URL = "https://explorer.solana.com"

fun solanaExplorerAddressUrl(address: String): String {
    val clean = address.trim()
    if (clean.isEmpty()) return ""
    return "$SOLANA_EXPLORER_BASE_URL/address/${DartUri.encodeComponent(clean)}"
}

fun solanaExplorerSignatureUrl(signature: String): String {
    val clean = signature.trim()
    if (clean.isEmpty()) return ""
    return "$SOLANA_EXPLORER_BASE_URL/tx/${DartUri.encodeComponent(clean)}"
}

data class BountyWallet(
    val address: String = "",
    val balanceLamports: Long = 0,
    val balanceSol: Double = 0.0,
    val payUri: String = "",
) {
    val hasWallet: Boolean get() = address.trim().isNotEmpty()

    val balanceLabel: String
        get() {
            if (balanceSol > 0) return "${modelTrimDouble(balanceSol)} SOL ($balanceLamports lamports)"
            return "$balanceLamports lamports"
        }

    val shortAddress: String
        get() {
            val clean = address.trim()
            if (clean.length <= 16) return clean
            return clean.substring(0, 6) + "..." + clean.substring(clean.length - 6)
        }

    val explorerAddressUrl: String get() = solanaExplorerAddressUrl(address)

    companion object {
        fun fromJson(data: Any?): BountyWallet {
            val source = data.asJsonMap()
            return BountyWallet(
                address = firstModelString(source, listOf("address", "walletAddress")),
                balanceLamports = modelInt(source["balanceLamports"]),
                balanceSol = modelDouble(source["balanceSol"]),
                payUri = firstModelString(source, listOf("uri", "payUri")),
            )
        }
    }
}

data class IssueBounty(
    val address: String = "",
    val status: String = "open",
    val amountUsd: Double = 0.0,
    val requiredLamports: Long = 0,
    val receivedLamports: Long = 0,
    val amountSol: Double = 0.0,
    val payUri: String = "",
    val payee: String = "",
    val payoutSig: String = "",
    val confirmed: Boolean = false,
) {
    val hasFunding: Boolean
        get() = address.isNotEmpty() ||
            amountUsd > 0 ||
            requiredLamports > 0 ||
            receivedLamports > 0 ||
            payoutSig.isNotEmpty()

    val statusLabel: String
        get() {
            val clean = status.trim()
            if (clean.isNotEmpty()) return clean
            if (payoutSig.isNotEmpty()) return "paid"
            if (requiredLamports > 0 && receivedLamports >= requiredLamports) return "funded"
            return "open"
        }

    val progressLabel: String
        get() {
            if (requiredLamports > 0) return "$receivedLamports / $requiredLamports lamports"
            if (receivedLamports > 0) return "$receivedLamports lamports received"
            return ""
        }

    companion object {
        fun fromJson(data: Any?): IssueBounty {
            val source = data.asJsonMap()
            val nested = when {
                source["bounty"] is Map<*, *> -> source["bounty"].asJsonMap()
                source["funding"] is Map<*, *> -> source["funding"].asJsonMap()
                else -> source
            }
            return IssueBounty(
                address = firstModelString(nested, listOf("address", "bountyAddress")),
                status = firstModelString(nested, listOf("status", "bountyStatus")).ifEmpty { "open" },
                amountUsd = modelDouble(nested["amountUsd"] ?: nested["bountyUsd"]),
                requiredLamports = modelInt(nested["requiredLamports"] ?: nested["bountyRequiredLamports"]),
                receivedLamports = modelInt(nested["receivedLamports"] ?: nested["bountyReceivedLamports"]),
                amountSol = modelDouble(nested["amountSol"] ?: nested["bountyAmountSol"]),
                payUri = firstModelString(nested, listOf("uri", "payUri", "bountyPayUri")),
                payee = firstModelString(nested, listOf("payee", "bountyPayee")),
                payoutSig = firstModelString(nested, listOf("payoutSig", "bountyPayoutSig")),
                confirmed = nested["confirmed"] == true,
            )
        }
    }
}
