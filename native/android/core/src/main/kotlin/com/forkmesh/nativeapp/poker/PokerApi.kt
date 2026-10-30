package com.forkmesh.nativeapp.poker

import com.forkmesh.nativeapp.json.JsonMap
import com.forkmesh.nativeapp.json.obj
import com.forkmesh.nativeapp.json.str
import com.forkmesh.nativeapp.json.strings
import com.forkmesh.nativeapp.market.PackagesClient
import com.forkmesh.nativeapp.market.PackagesException

data class PokerTableDraft(
    val name: String,
    val stakes: String,
    val seats: Int,
    val private: Boolean,
    val turnSeconds: Int,
    val bots: Int,
    val buyIn: Long,
) {
    val seatCount: Int get() = seats.coerceIn(PokerApi.MIN_SEATS, PokerApi.MAX_SEATS)

    val body: Map<String, Any?>
        get() = linkedMapOf(
            "name" to name.trim().take(PokerApi.NAME_MAX),
            "stakes" to stakes,
            "seats" to seatCount,
            "private" to private,
            "turnSeconds" to (turnSeconds.takeIf { it in PokerApi.TURN_SECONDS } ?: 30),
            "bots" to bots.coerceIn(0, seatCount - 1),
            "buyIn" to buyIn.coerceAtLeast(0),
        )
}

data class PokerInviteResult(val invited: List<String>, val skipped: List<String>, val table: PokerTable?)

class PokerApi(private val client: PackagesClient) {
    companion object {
        const val PACKAGE_ID = "poker"
        const val API = "/api/poker"
        const val TOPIC = "package:poker"
        const val MIN_SEATS = 2
        const val MAX_SEATS = 9
        const val NAME_MAX = 40
        const val CHAT_MAX = 200
        val TURN_SECONDS = listOf(15, 30, 45, 60)
        private val TABLE_ID = Regex("^[0-9a-f]{32}$")
        private val CODE = Regex("^[A-Z]{6}$")
        private val ACCOUNT = Regex("^[a-z0-9](?:[a-z0-9-]{0,62})$")
        val ACTIONS = setOf("fold", "check", "call", "bet", "raise", "allin")

        fun isTableId(value: String): Boolean = TABLE_ID.matches(value)

        fun normalizeCode(raw: String): String = raw.trim().uppercase().filter { it in 'A'..'Z' }

        fun isCode(value: String): Boolean = CODE.matches(value)

        fun normalizeNames(raw: String): List<String> =
            raw.split(',', ' ', '\n', ';').map { it.trim().removePrefix("@").lowercase() }.filter { ACCOUNT.matches(it) }.distinct()

        fun problem(code: String): String = when (code) {
            "claim_not_ready" -> "Your free chips are not ready yet."
            "table_not_found", "not_found" -> "That table has closed or does not exist."
            "table_full" -> "That table is full."
            "seat_taken" -> "Someone just took that seat. Pick another."
            "already_seated" -> "You already have a seat at this table."
            "not_seated" -> "Take a seat first."
            "not_enough_chips" -> "You do not have enough chips in your wallet for that."
            "invalid_buy_in" -> "Choose a buy-in between the table's minimum and maximum."
            "too_many_tables" -> "You already own five open tables. Close one first."
            "owner_only" -> "Only the table's owner can do that."
            "hand_in_progress" -> "Wait for this hand to finish."
            "hand_changed" -> "The hand moved on before your action arrived."
            "not_your_turn" -> "It is not your turn."
            "invalid_amount" -> "That amount is not allowed right now."
            "invalid_stakes" -> "Pick one of the listed stakes."
            "invalid_text" -> "That message cannot be sent."
            "invalid_code" -> "Table codes are six letters."
            "invalid_session", "unauthorized", "sign_in_required" -> "Sign in to ForkMesh to play poker."
            "plugin_not_installed" -> "Install Poker from the Market to play."
            "network" -> "ForkMesh could not be reached. Check your connection."
            else -> PackagesException.sentence(code)
        }
    }

    private fun table(id: String): String {
        if (!isTableId(id)) throw PackagesException("table_not_found", 404)
        return "$API/tables/$id"
    }

    private fun tableOf(reply: JsonMap): PokerTable = PokerTable.fromJson(reply.obj("table")) ?: throw PackagesException("table_not_found", 404)

    fun lobby(): PokerLobby = PokerLobby.fromJson(client.call("GET", API))

    fun claim(): PokerWallet = PokerWallet.fromJson(client.call("POST", "$API/claim", body = emptyMap()).obj("wallet"))

    fun create(draft: PokerTableDraft): PokerTable = tableOf(client.call("POST", "$API/tables", body = draft.body))

    fun code(raw: String): String {
        val code = normalizeCode(raw)
        if (!isCode(code)) throw PackagesException("invalid_code", 400)
        val id = client.call("GET", "$API/codes/$code").str("tableId")
        if (!isTableId(id)) throw PackagesException("table_not_found", 404)
        return id
    }

    fun open(id: String): PokerTable = tableOf(client.call("GET", table(id)))

    fun sit(id: String, seat: Int, buyIn: Long): PokerTable =
        tableOf(client.call("POST", "${table(id)}/sit", body = linkedMapOf("seat" to seat, "buyIn" to buyIn)))

    fun leave(id: String): PokerTable? = PokerTable.fromJson(client.call("POST", "${table(id)}/leave", body = emptyMap()).obj("table"))

    fun act(id: String, action: String, amount: Long, hand: Long): PokerTable {
        if (action !in ACTIONS) throw PackagesException("invalid_amount", 400)
        return tableOf(client.call("POST", "${table(id)}/act", body = linkedMapOf("action" to action, "amount" to amount, "hand" to hand)))
    }

    fun tick(id: String): PokerTable = tableOf(client.call("POST", "${table(id)}/tick", body = emptyMap()))

    fun away(id: String, away: Boolean): PokerTable = tableOf(client.call("POST", "${table(id)}/away", body = linkedMapOf("away" to away)))

    fun rebuy(id: String, amount: Long): PokerTable = tableOf(client.call("POST", "${table(id)}/rebuy", body = linkedMapOf("amount" to amount)))

    fun addBot(id: String): PokerTable = tableOf(client.call("POST", "${table(id)}/bots", body = linkedMapOf("add" to 1)))

    fun removeBot(id: String, seat: Int): PokerTable = tableOf(client.call("POST", "${table(id)}/bots", body = linkedMapOf("remove" to seat)))

    fun chat(id: String, text: String): PokerTable {
        val line = text.trim()
        if (line.isEmpty() || line.length > CHAT_MAX) throw PackagesException("invalid_text", 400)
        return tableOf(client.call("POST", "${table(id)}/chat", body = linkedMapOf("text" to line)))
    }

    fun invite(id: String, names: List<String>): PokerInviteResult {
        val reply = client.call("POST", "${table(id)}/invite", body = linkedMapOf("names" to names))
        return PokerInviteResult(
            reply.strings("invited"),
            reply.strings("skipped"),
            PokerTable.fromJson(reply.obj("table")),
        )
    }

    fun close(id: String): Boolean = client.call("DELETE", table(id))["closed"] == true
}
