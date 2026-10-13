package com.forkmesh.nativeapp.market

import com.forkmesh.nativeapp.control.ControlFamilyChannel
import com.forkmesh.nativeapp.control.DesktopControlFamily
import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.json.JsonMap
import com.forkmesh.nativeapp.json.bool
import com.forkmesh.nativeapp.json.int
import com.forkmesh.nativeapp.json.long
import com.forkmesh.nativeapp.json.maps
import com.forkmesh.nativeapp.json.obj
import com.forkmesh.nativeapp.json.optBool
import com.forkmesh.nativeapp.json.str
import com.forkmesh.nativeapp.security.SecretStore
import java.util.UUID

object Lamports {
    const val PER_SOL: Long = 1_000_000_000

    fun sol(lamports: Long): String {
        val negative = lamports < 0
        val value = if (lamports == Long.MIN_VALUE) Long.MAX_VALUE else kotlin.math.abs(lamports)
        val whole = value / PER_SOL
        val fraction = (value % PER_SOL).toString().padStart(9, '0').trimEnd('0')
        val text = if (fraction.isEmpty()) "$whole" else "$whole.$fraction"
        return (if (negative) "-" else "") + text + " SOL"
    }

    fun parse(text: String): Long? {
        val trimmed = text.trim()
        if (trimmed.isEmpty() || trimmed.startsWith("-") || trimmed.startsWith("+")) return null
        val parts = trimmed.split('.')
        if (parts.size > 2) return null
        val wholePart = parts[0]
        val whole = wholePart.ifEmpty { "0" }
        val fraction = if (parts.size == 2) parts[1] else ""
        if (!whole.all { it in '0'..'9' } || !fraction.all { it in '0'..'9' }) return null
        if (fraction.length > 9 || whole.length > 9) return null
        if (parts.size == 2 && fraction.isEmpty() && wholePart.isEmpty()) return null
        return whole.toLong() * PER_SOL + fraction.padEnd(9, '0').toLong()
    }
}

data class LedgerEntry(
    val id: String,
    val category: String = "record",
    val title: String = "Financial record",
    val status: String = "recorded",
    val direction: String = "record",
    val currency: String = "SOL",
    val amountLamports: Long = 0,
    val amountSol: String = "",
    val amountUsd: String = "",
    val amountKnown: Boolean = true,
    val occurredAt: Long = 0,
    val transactionId: String = "",
    val explorerUrl: String = "",
    val note: String = "",
) {
    val amountText: String
        get() = when {
            !amountKnown -> "Amount not recorded"
            currency == "SOL" -> Lamports.sol(amountLamports)
            amountUsd.isEmpty() -> currency
            else -> "$$amountUsd"
        }

    val verified: Boolean get() = transactionId.isNotEmpty()

    companion object {
        fun fromJson(json: JsonMap) = LedgerEntry(
            json.str("id"), json.str("category", "record"), json.str("title", "Financial record"), json.str("status", "recorded"),
            json.str("direction", "record"), json.str("currency", "SOL"), maxOf(0, json.long("amountLamports")), json.str("amountSol"),
            json.str("amountUsd"), json.optBool("amountKnown") ?: true, json.long("occurredAt"), json.str("transactionId"),
            json.str("explorerUrl"), json.str("note"),
        )
    }
}

data class SystemWallet(
    val key: String,
    val label: String,
    val purpose: String,
    val address: String,
    val explorerUrl: String,
    val balanceLamports: Long,
    val balanceKnown: Boolean,
    val sharedAddress: Boolean,
) {
    val isTreasury: Boolean get() = key == "treasury" || label.lowercase().contains("treasury")
    val donationUri: String get() = "solana:$address"

    companion object {
        fun fromJson(json: JsonMap) = SystemWallet(
            json.str("key"), json.str("label"), json.str("purpose"), json.str("address"), json.str("explorerUrl"),
            maxOf(0, json.long("balanceLamports")), json.bool("balanceKnown"), json.bool("sharedAddress"),
        )
    }
}

data class LedgerCursor(val beforeAt: Long, val beforeId: String) {
    val query: List<Pair<String, String>> get() = listOf("beforeAt" to beforeAt.toString(), "beforeId" to beforeId)
}

data class LedgerPage(val entries: List<LedgerEntry>, val next: LedgerCursor?, val wallets: List<SystemWallet>, val solUsd: String) {
    val treasury: SystemWallet? get() = wallets.firstOrNull { it.isTreasury } ?: wallets.firstOrNull()

    companion object {
        fun fromJson(json: JsonMap): LedgerPage {
            val page = json.obj("page")
            val cursor = page.obj("nextCursor")
            val quote = json.obj("solUsd")
            return LedgerPage(
                json.maps("entries").map(LedgerEntry::fromJson).filter { it.id.isNotEmpty() },
                if (page.bool("hasMore") && cursor.str("beforeId").isNotEmpty()) LedgerCursor(cursor.long("beforeAt"), cursor.str("beforeId")) else null,
                json.maps("wallets").map(SystemWallet::fromJson).filter { it.address.isNotEmpty() },
                if (quote.bool("available")) quote.str("price") else "",
            )
        }
    }
}

data class RewardEstimate(
    val rewardId: String,
    val recipient: String = "",
    val recipientAddress: String = "",
    val repository: String = "",
    val sourceKind: String = "",
    val subjectNumber: Int = 0,
    val subjectTitle: String = "",
    val amountLamports: Long = 0,
    val status: String = "",
    val transactionSignature: String = "",
    val createdAt: Long = 0,
) {
    val paid: Boolean get() = status == "paid" || transactionSignature.isNotEmpty()
    val subjectLabel: String get() = "$repository ${if (sourceKind == "pull") "PR" else "Issue"} #$subjectNumber"

    companion object {
        fun fromJson(json: JsonMap) = RewardEstimate(
            json.str("rewardId"), json.str("recipient"), json.str("recipientAddress"), json.str("repository"), json.str("sourceKind"),
            json.int("subjectNumber"), json.str("subjectTitle"), maxOf(0, json.long("amountLamports")), json.str("status"),
            json.str("transactionSignature"), json.long("createdAt"),
        )
    }
}

data class WalletBalance(val address: String, val lamports: Long?, val checkedAt: Long) {
    companion object {
        fun fromJson(address: String, json: JsonMap) = WalletBalance(
            address, if (json.bool("ok")) (json["lamports"] as? Number)?.toLong() else null, json.long("checkedAt"),
        )
    }
}

object SolanaAddress {
    private const val ALPHABET = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz"

    fun isValid(value: String): Boolean = value.length in 32..44 && value.all { it in ALPHABET }
}

enum class PersonalRecordKind(val wire: String) {
    EXPENSE("expense"), INCOME("income");

    companion object {
        fun of(wire: String): PersonalRecordKind? = entries.firstOrNull { it.wire == wire }
    }
}

data class PersonalRecord(
    val id: String = UUID.randomUUID().toString().lowercase(),
    val kind: PersonalRecordKind,
    val title: String,
    val amountMinor: Long,
    val currency: String = "USD",
    val occurredAt: Long,
    val note: String = "",
    val settled: Boolean = false,
) {
    val amountText: String get() = MoneyText.cents(amountMinor, currency)

    fun toJson(): Map<String, Any?> = linkedMapOf(
        "id" to id, "kind" to kind.wire, "title" to title, "amountMinor" to amountMinor, "currency" to currency,
        "occurredAt" to occurredAt, "note" to note, "settled" to settled,
    )

    companion object {
        fun make(
            kind: PersonalRecordKind,
            title: String,
            amountMinor: Long,
            currency: String = "USD",
            occurredAt: Long,
            note: String = "",
            settled: Boolean = false,
            id: String = UUID.randomUUID().toString().lowercase(),
        ) = PersonalRecord(id, kind, title.take(200), maxOf(0, amountMinor), currency, occurredAt, note.take(2000), settled)

        fun fromJson(json: JsonMap): PersonalRecord? {
            val kind = PersonalRecordKind.of(json.str("kind")) ?: return null
            val id = json.str("id").ifEmpty { return null }
            return make(kind, json.str("title"), json.long("amountMinor"), json.str("currency", "USD"), json.long("occurredAt"), json.str("note"), json.bool("settled"), id)
        }
    }
}

data class PersonalTotal(val currency: String, val income: Long, val expense: Long)

class PersonalLedgerStore(private val store: SecretStore) {
    fun load(): List<PersonalRecord> {
        val text = store.read(KEY) ?: return emptyList()
        val list = runCatching { Json.parse(text) as? List<*> }.getOrNull() ?: return emptyList()
        return list.mapNotNull { PersonalRecord.fromJson(it.asMap()) }.sortedByDescending { it.occurredAt }
    }

    fun save(records: List<PersonalRecord>): List<PersonalRecord> {
        val bounded = records.sortedByDescending { it.occurredAt }.take(MAX_RECORDS)
        store.write(KEY, Json.stringify(bounded.map { it.toJson() }))
        return bounded
    }

    fun clear() = store.delete(KEY)

    companion object {
        const val MAX_RECORDS = 500
        const val KEY = "packages/ledger/personal-records"

        fun totals(records: List<PersonalRecord>): List<PersonalTotal> {
            val byCurrency = java.util.TreeMap<String, LongArray>()
            for (record in records) {
                val entry = byCurrency.getOrPut(record.currency) { LongArray(2) }
                if (record.kind == PersonalRecordKind.INCOME) entry[0] += record.amountMinor else entry[1] += record.amountMinor
            }
            return byCurrency.map { PersonalTotal(it.key, it.value[0], it.value[1]) }
        }
    }
}

class LedgerModel(private val client: PackagesClient, private val store: PersonalLedgerStore) {
    var entries: List<LedgerEntry> = emptyList()
        private set
    var wallets: List<SystemWallet> = emptyList()
        private set
    var solUsd: String = ""
        private set
    var next: LedgerCursor? = null
        private set
    var rewards: List<RewardEstimate> = emptyList()
        private set
    var records: List<PersonalRecord> = store.load()
        private set
    var ownBalance: WalletBalance? = null
        private set
    var errorText: String = ""
        private set

    val treasury: SystemWallet? get() = wallets.firstOrNull { it.isTreasury } ?: wallets.firstOrNull()
    val pendingRewards: List<RewardEstimate> get() = rewards.filter { !it.paid }
    val paidRewards: List<RewardEstimate> get() = rewards.filter { it.paid }

    fun refresh(ownAddress: String = "") {
        try {
            val page = client.ledger()
            entries = page.entries
            wallets = page.wallets
            solUsd = page.solUsd
            next = page.next
            errorText = ""
        } catch (thrown: Exception) {
            errorText = PackagesException.describe(thrown)
        }
        rewards = runCatching { client.rewardEstimates() }.getOrNull() ?: rewards
        if (SolanaAddress.isValid(ownAddress)) ownBalance = runCatching { client.walletBalance(ownAddress) }.getOrNull()
    }

    fun loadMore() {
        val cursor = next ?: return
        try {
            val page = client.ledger(cursor)
            val known = entries.map { it.id }.toSet()
            entries = entries + page.entries.filter { it.id !in known }
            next = page.next
        } catch (thrown: Exception) {
            errorText = PackagesException.describe(thrown)
        }
    }

    fun add(record: PersonalRecord) {
        records = store.save(records.filter { it.id != record.id } + record)
    }

    fun remove(id: String) {
        records = store.save(records.filter { it.id != id })
    }

    fun toggleSettled(id: String) {
        records = store.save(records.map { if (it.id == id) it.copy(settled = !it.settled) else it })
    }

    val totals: List<PersonalTotal> get() = PersonalLedgerStore.totals(records)
}

data class WalletRequestState(val id: String, val action: String, val summary: String, val status: String, val requestedAt: Long) {
    companion object {
        fun fromJson(json: JsonMap) = WalletRequestState(
            json.str("id"), json.str("action"), json.str("summary"), json.str("status", "awaiting_owner"), json.long("requestedAt"),
        )
    }
}

data class WalletStatus(
    val linked: Boolean = false,
    val address: String = "",
    val busy: Boolean = false,
    val queued: Int = 0,
    val minLamports: Long = 0,
    val maxLamports: Long = 0,
    val awaitingOwner: WalletRequestState? = null,
) {
    fun amountProblem(lamports: Long?): String? {
        if (lamports == null || lamports <= 0) return "Enter an amount in SOL."
        if (minLamports > 0 && lamports < minLamports) return "The smallest amount is ${Lamports.sol(minLamports)}."
        if (maxLamports > 0 && lamports > maxLamports) return "The largest amount is ${Lamports.sol(maxLamports)}."
        return null
    }

    companion object {
        fun fromJson(json: JsonMap): WalletStatus {
            val pending = json.obj("request")
            return WalletStatus(
                json.bool("linked"), json.str("address"), json.bool("busy"), maxOf(0, json.int("queued")),
                maxOf(0, json.long("minLamports")), maxOf(0, json.long("maxLamports")),
                if (pending.str("id").isEmpty()) null else WalletRequestState.fromJson(pending),
            )
        }
    }
}

data class WalletReward(
    val rewardId: String,
    val recipient: String,
    val address: String,
    val lamports: Long,
    val repository: String,
    val kind: String,
    val number: Int,
    val title: String,
) {
    companion object {
        fun fromJson(json: JsonMap) = WalletReward(
            json.str("rewardId"), json.str("recipient"), json.str("address"), maxOf(0, json.long("lamports")), json.str("repository"),
            json.str("kind"), json.int("number"), json.str("title"),
        )
    }
}

enum class WalletAction(val wire: String) {
    TIP("tip"), REWARD("reward");

    companion object {
        fun of(wire: String): WalletAction? = entries.firstOrNull { it.wire == wire }
    }
}

data class WalletConfirmation(val token: String, val action: WalletAction, val summary: String, val expiresInMs: Int, val issuedAtMs: Long) {
    fun expired(nowMs: Long): Boolean = nowMs >= issuedAtMs + expiresInMs

    companion object {
        fun fromJson(json: JsonMap, issuedAtMs: Long): WalletConfirmation? {
            val action = WalletAction.of(json.str("action")) ?: return null
            if (json.str("token").isEmpty()) return null
            return WalletConfirmation(json.str("token"), action, json.str("summary"), json.int("expiresInMs", 60_000), issuedAtMs)
        }
    }
}

class TipRequest(recipient: String, address: String, val lamports: Long) {
    val recipient: String = recipient.trim().lowercase()
    val address: String = address.trim()

    val isValid: Boolean get() = PackagesText.isOrgName(recipient) && SolanaAddress.isValid(address) && lamports > 0

    val params: Map<String, Any?> get() = linkedMapOf("recipient" to recipient, "address" to address, "lamports" to lamports)
}

class WalletClient(private val family: ControlFamilyChannel, private val clock: () -> Long = System::currentTimeMillis) {
    val nowMs: Long get() = clock()

    fun status(): WalletStatus = WalletStatus.fromJson(family.call("status"))

    fun rewards(): List<WalletReward> = family.call("rewards").maps("rewards").map(WalletReward::fromJson).filter { it.rewardId.isNotEmpty() }

    fun prepareTip(tip: TipRequest): WalletConfirmation {
        if (!tip.isValid) throw PackagesException("bad_request")
        return prepare(tip.params + ("action" to WalletAction.TIP.wire))
    }

    fun prepareReward(rewardId: String): WalletConfirmation = prepare(linkedMapOf("action" to WalletAction.REWARD.wire, "rewardId" to rewardId))

    fun requestTip(tip: TipRequest, confirmation: WalletConfirmation): WalletRequestState =
        request(tip.params + ("action" to WalletAction.TIP.wire) + ("token" to confirmation.token), confirmation)

    fun requestReward(rewardId: String, confirmation: WalletConfirmation): WalletRequestState =
        request(linkedMapOf("action" to WalletAction.REWARD.wire, "rewardId" to rewardId, "token" to confirmation.token), confirmation)

    fun onChanged(handler: (Map<String, Any?>) -> Unit): Any = family.addEventTap { suffix, data -> if (suffix == "changed") handler(data) }

    fun removeTap(token: Any) = family.removeEventTap(token)

    private fun prepare(params: Map<String, Any?>): WalletConfirmation {
        val issued = nowMs
        return WalletConfirmation.fromJson(family.call("prepare", params), issued) ?: throw PackagesException("confirmation_required")
    }

    private fun request(params: Map<String, Any?>, confirmation: WalletConfirmation): WalletRequestState {
        if (confirmation.expired(nowMs)) throw PackagesException("confirmation_expired")
        return WalletRequestState.fromJson(family.call("request", params, 30_000))
    }

    companion object {
        fun describe(error: Throwable): String {
            if (DesktopControlFamily.isUnsupported(error)) return "Your desktop needs an update before it can take payment requests from the phone."
            return when (val code = controlErrorCode(error) ?: "") {
                "no_wallet" -> "No tip wallet is linked on your desktop. Link one there first."
                "wallet_busy" -> "Your desktop is already waiting for you to approve a payment."
                "confirmation_expired" -> "That confirmation expired. Try again."
                "confirmation_mismatch", "confirmation_required" -> "The desktop did not accept the confirmation."
                "unknown_reward" -> "That reward is not payable from your desktop right now."
                "bad_address" -> "That recipient has no valid payout address."
                "bad_amount" -> "That amount is outside what the desktop will sign."
                "bad_request" -> "Check the recipient and amount."
                "not_owner" -> "Only the desktop's owner can request payments."
                "" -> PackagesException.describe(error)
                else -> code.replace('_', ' ').replaceFirstChar { it.uppercase() }
            }
        }
    }
}

class WalletRequestModel(private val wallet: WalletClient?, private val packages: PackagesClient) {
    var status: WalletStatus? = null
        private set
    var rewards: List<WalletReward> = emptyList()
        private set
    var confirmation: WalletConfirmation? = null
        private set
    var lastRequest: WalletRequestState? = null
        private set
    var errorText: String = ""
        private set
    var unavailable: Boolean = wallet == null
        private set
    var recipient: String = ""
    var address: String = ""
    var amount: String = ""
    private var tap: Any? = null
    private var pendingTip: TipRequest? = null
    private var pendingReward: String? = null

    val lamports: Long? get() = Lamports.parse(amount)
    val amountProblem: String? get() = (status ?: WalletStatus()).amountProblem(lamports)
    val tip: TipRequest get() = TipRequest(recipient, address, lamports ?: 0)
    val canPrepareTip: Boolean get() = tip.isValid && amountProblem == null && (status?.linked ?: false)

    fun start(onChange: () -> Unit) {
        val wallet = wallet ?: return
        if (tap == null) tap = wallet.onChanged { onChange() }
        refresh()
    }

    fun stop() {
        tap?.let { wallet?.removeTap(it) }
        tap = null
    }

    fun refresh() {
        val wallet = wallet ?: return
        try {
            status = wallet.status()
            rewards = runCatching { wallet.rewards() }.getOrNull() ?: emptyList()
            errorText = ""
            unavailable = false
        } catch (thrown: Exception) {
            errorText = WalletClient.describe(thrown)
            unavailable = true
        }
    }

    fun lookUpAddress() {
        val name = recipient.trim().lowercase()
        if (!PackagesText.isOrgName(name)) return
        try {
            val found = packages.payoutAddress(name)
            address = found
            errorText = if (found.isEmpty()) "@$name has not set a payout address." else ""
        } catch (thrown: Exception) {
            errorText = PackagesException.describe(thrown)
        }
    }

    fun prepareTip() {
        val wallet = wallet ?: return
        if (!canPrepareTip) return
        val request = tip
        try {
            confirmation = wallet.prepareTip(request)
            pendingTip = request
            pendingReward = null
            errorText = ""
        } catch (thrown: Exception) {
            errorText = WalletClient.describe(thrown)
        }
    }

    fun prepareReward(reward: WalletReward) {
        val wallet = wallet ?: return
        try {
            confirmation = wallet.prepareReward(reward.rewardId)
            pendingReward = reward.rewardId
            pendingTip = null
            errorText = ""
        } catch (thrown: Exception) {
            errorText = WalletClient.describe(thrown)
        }
    }

    fun cancel() {
        confirmation = null
        pendingTip = null
        pendingReward = null
    }

    fun send(): Boolean {
        val wallet = wallet ?: return false
        val confirmation = confirmation ?: return false
        return try {
            val tip = pendingTip
            val reward = pendingReward
            lastRequest = when {
                tip != null && confirmation.action == WalletAction.TIP -> wallet.requestTip(tip, confirmation)
                reward != null && confirmation.action == WalletAction.REWARD -> wallet.requestReward(reward, confirmation)
                else -> throw PackagesException("confirmation_mismatch")
            }
            cancel()
            amount = ""
            errorText = ""
            refresh()
            true
        } catch (thrown: Exception) {
            errorText = WalletClient.describe(thrown)
            cancel()
            false
        }
    }
}
