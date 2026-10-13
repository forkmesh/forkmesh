package com.forkmesh.nativeapp.chat

import com.forkmesh.nativeapp.control.DesktopControlFamily
import com.forkmesh.nativeapp.control.LocalControlException
import com.forkmesh.nativeapp.json.asJsonMap
import com.forkmesh.nativeapp.json.jsonLong
import com.forkmesh.nativeapp.json.jsonMaps
import com.forkmesh.nativeapp.json.jsonText
import java.util.concurrent.CopyOnWriteArrayList

data class DiscordStatus(val available: Boolean = false, val configured: Boolean = false, val connected: Boolean = false, val bot: String = "")

data class DiscordChannel(
    val id: String,
    val name: String,
    val display: String,
    val group: String,
    val topic: String,
    val direct: Boolean,
    val guild: Boolean,
    val canSend: Boolean,
)

data class DiscordGuild(val name: String, val channels: List<DiscordChannel>)

data class DiscordReaction(val value: String, val count: Int) {
    val emoji: String get() = ReactionCatalog.emoji(value)
}

data class DiscordMessage(
    val id: String,
    val sender: String,
    val text: String,
    val timestamp: Long,
    val self: Boolean,
    val rootId: String,
    val fileName: String,
    val edited: Boolean,
    val truncated: Boolean,
    val reactions: List<DiscordReaction>,
)

data class DiscordSendConfirmation(
    val channel: String,
    val text: String,
    val replyTo: String,
    val token: String,
    val summary: String,
    val expiresAtMs: Long,
)

class DiscordBridgeException(val kind: Kind, val code: String = "", reason: String = "") : RuntimeException(
    when (kind) {
        Kind.DESKTOP_TOO_OLD -> "This desktop does not share its Discord bridge yet. Update ForkMesh on the desktop."
        Kind.UNAVAILABLE -> "The Discord package is not running on your desktop."
        Kind.INVALID -> reason
        Kind.REFUSED -> DiscordBridge.errorMessage(code)
    },
) {
    enum class Kind { DESKTOP_TOO_OLD, UNAVAILABLE, INVALID, REFUSED }
}

interface DiscordControl {
    fun call(suffix: String, params: Map<String, Any?>): Map<String, Any?>

    fun addEventTap(tap: (String, Map<String, Any?>) -> Unit): Any

    fun removeEventTap(token: Any)

    fun isUnsupported(error: Throwable): Boolean
}

class FamilyDiscordControl(private val family: DesktopControlFamily) : DiscordControl {
    override fun call(suffix: String, params: Map<String, Any?>): Map<String, Any?> = family.call(suffix, params)

    override fun addEventTap(tap: (String, Map<String, Any?>) -> Unit): Any = family.addEventTap(tap)

    override fun removeEventTap(token: Any) = family.removeEventTap(token)

    override fun isUnsupported(error: Throwable): Boolean = DesktopControlFamily.isUnsupported(error)
}

class DiscordBridge(private val control: DiscordControl, private val now: () -> Long = System::currentTimeMillis) {
    companion object {
        const val MAX_SEND_CHARS = 2_000
        const val MAX_CHANNELS = 300
        const val MAX_MESSAGES = 100

        fun guilds(channels: List<DiscordChannel>): List<DiscordGuild> {
            val order = ArrayList<String>()
            val grouped = LinkedHashMap<String, MutableList<DiscordChannel>>()
            for (channel in channels) {
                val key = channel.group.ifEmpty { "Discord" }
                if (!grouped.containsKey(key)) order.add(key)
                grouped.getOrPut(key) { ArrayList() }.add(channel)
            }
            return order.map { DiscordGuild(it, grouped[it].orEmpty()) }
        }

        fun validChannelId(id: String): Boolean = id.isNotEmpty() && id.length <= 24 && id.all { it in '0'..'9' }

        fun parseStatus(reply: Map<String, Any?>): DiscordStatus = DiscordStatus(
            available = reply["available"] == true,
            configured = reply["configured"] == true,
            connected = reply["connected"] == true,
            bot = jsonText(reply["bot"]).take(120),
        )

        fun parseChannels(reply: Map<String, Any?>): List<DiscordChannel> {
            val out = ArrayList<DiscordChannel>()
            val seen = HashSet<String>()
            for (row in jsonMaps(reply["channels"])) {
                if (out.size >= MAX_CHANNELS) break
                val id = jsonText(row["id"])
                if (!validChannelId(id) || !seen.add(id)) continue
                out.add(
                    DiscordChannel(
                        id = id,
                        name = jsonText(row["name"]).take(120),
                        display = jsonText(row["display"]).take(120),
                        group = jsonText(row["group"]).take(120),
                        topic = jsonText(row["topic"]).take(240),
                        direct = row["direct"] == true,
                        guild = row["guild"] == true,
                        canSend = row["canSend"] == true,
                    ),
                )
            }
            return out
        }

        fun parseMessages(reply: Map<String, Any?>): List<DiscordMessage> {
            val out = ArrayList<DiscordMessage>()
            for (row in jsonMaps(reply["messages"]).takeLast(MAX_MESSAGES)) {
                val id = jsonText(row["id"]).take(64)
                if (id.isEmpty()) continue
                val reactions = ArrayList<DiscordReaction>()
                for ((value, count) in row["reactions"].asJsonMap()) {
                    val n = jsonLong(count).toInt()
                    if (n > 0 && reactions.size < 24) reactions.add(DiscordReaction(ReactionCatalog.valueForEmoji(value), n))
                }
                reactions.sortBy { it.value }
                out.add(
                    DiscordMessage(
                        id = id,
                        sender = jsonText(row["sender"]).take(120),
                        text = jsonText(row["text"]).take(4_000),
                        timestamp = jsonLong(row["ts"]),
                        self = row["self"] == true,
                        rootId = jsonText(row["root"]).take(64),
                        fileName = jsonText(row["fileName"]).take(180),
                        edited = row["edited"] == true,
                        truncated = row["truncated"] == true,
                        reactions = reactions,
                    ),
                )
            }
            return out
        }

        fun errorMessage(code: String): String = when (code) {
            "discord_unavailable" -> "The Discord package is not running on your desktop."
            "unknown_channel" -> "That Discord channel is no longer available on the desktop."
            "not_writable" -> "The desktop's Discord bot cannot post in this channel."
            "bad_text" -> "Discord messages must be 1 to 2,000 characters."
            "confirmation_expired" -> "The confirmation expired. Send again."
            "confirmation_mismatch", "confirmation_required" -> "The desktop did not accept this confirmation. Send again."
            "send_failed" -> "The desktop could not send the message to Discord."
            "not_owner" -> "Only the desktop's owner can use its Discord bridge."
            "" -> "The desktop refused."
            else -> code.replace('_', ' ')
        }
    }

    private var tap: Any? = null
    private val listeners = CopyOnWriteArrayList<() -> Unit>()

    @Volatile
    var watching: Set<String> = emptySet()

    @Volatile
    var status: DiscordStatus = DiscordStatus()
        private set

    @Volatile
    var channels: List<DiscordChannel> = emptyList()
        private set

    @Volatile
    var messages: Map<String, List<DiscordMessage>> = emptyMap()
        private set

    @Volatile
    var loading: Boolean = false
        private set

    @Volatile
    var lastError: String? = null
        private set

    val guilds: List<DiscordGuild> get() = guilds(channels)

    fun channel(id: String): DiscordChannel? = channels.firstOrNull { it.id == id }

    fun addListener(listener: () -> Unit) {
        listeners.add(listener)
    }

    fun removeListener(listener: () -> Unit) {
        listeners.remove(listener)
    }

    private fun changed() {
        for (listener in listeners) runCatching { listener() }
    }

    private fun call(method: String, params: Map<String, Any?> = emptyMap()): Map<String, Any?> = try {
        control.call(method, params)
    } catch (error: Exception) {
        if (control.isUnsupported(error)) throw DiscordBridgeException(DiscordBridgeException.Kind.DESKTOP_TOO_OLD)
        if (error is LocalControlException) {
            if (error.code == "discord_unavailable") throw DiscordBridgeException(DiscordBridgeException.Kind.UNAVAILABLE)
            throw DiscordBridgeException(DiscordBridgeException.Kind.REFUSED, error.code)
        }
        throw error
    }

    fun refresh() {
        loading = true
        changed()
        try {
            status = parseStatus(call("status"))
            if (!status.available) {
                channels = emptyList()
                lastError = DiscordBridgeException(DiscordBridgeException.Kind.UNAVAILABLE).message
                return
            }
            channels = parseChannels(call("list"))
            val live = channels.map { it.id }.toSet()
            messages = messages.filterKeys { it in live }
            lastError = null
        } catch (error: Exception) {
            lastError = error.message ?: error.toString()
        } finally {
            loading = false
            changed()
        }
    }

    fun load(channelId: String, limit: Int = 50) {
        if (!validChannelId(channelId)) return
        try {
            val reply = call("messages", linkedMapOf("channel" to channelId, "limit" to limit.coerceIn(1, MAX_MESSAGES)))
            messages = messages + (channelId to parseMessages(reply))
            lastError = null
        } catch (error: Exception) {
            lastError = error.message ?: error.toString()
        }
        changed()
    }

    fun prepareSend(channelId: String, text: String, replyTo: String = ""): DiscordSendConfirmation {
        val body = text.trim()
        if (!validChannelId(channelId)) throw DiscordBridgeException(DiscordBridgeException.Kind.INVALID, reason = "Pick a Discord channel.")
        if (body.isEmpty() || body.length > MAX_SEND_CHARS) {
            throw DiscordBridgeException(DiscordBridgeException.Kind.INVALID, reason = errorMessage("bad_text"))
        }
        val params = linkedMapOf<String, Any?>("channel" to channelId, "text" to body)
        if (replyTo.isNotEmpty()) params["replyTo"] = replyTo
        val reply = call("prepare", params)
        val token = jsonText(reply["token"])
        if (token.isEmpty()) throw DiscordBridgeException(DiscordBridgeException.Kind.REFUSED, "confirmation_required")
        val expires = (reply["expiresInMs"] as? Number)?.toLong() ?: 60_000L
        return DiscordSendConfirmation(
            channel = channelId,
            text = body,
            replyTo = replyTo,
            token = token,
            summary = (reply["summary"]?.let(::jsonText) ?: "Send to Discord").take(300),
            expiresAtMs = now() + expires,
        )
    }

    fun send(confirmation: DiscordSendConfirmation) {
        if (now() >= confirmation.expiresAtMs) throw DiscordBridgeException(DiscordBridgeException.Kind.REFUSED, "confirmation_expired")
        val params = linkedMapOf<String, Any?>("channel" to confirmation.channel, "text" to confirmation.text, "token" to confirmation.token)
        if (confirmation.replyTo.isNotEmpty()) params["replyTo"] = confirmation.replyTo
        call("send", params)
        load(confirmation.channel)
    }

    @Synchronized
    fun startWatching(onChange: () -> Unit) {
        if (tap != null) return
        tap = control.addEventTap { suffix, _ -> if (suffix == "changed") onChange() }
    }

    @Synchronized
    fun stopWatching() {
        tap?.let(control::removeEventTap)
        tap = null
    }
}
