package com.forkmesh.nativeapp.chat

import com.forkmesh.nativeapp.crypto.ChatDeviceKeys
import com.forkmesh.nativeapp.crypto.ChatTrust
import com.forkmesh.nativeapp.crypto.ChatV3
import com.forkmesh.nativeapp.crypto.ChatV4
import com.forkmesh.nativeapp.crypto.RoomCrypto
import com.forkmesh.nativeapp.crypto.RoomSeal
import com.forkmesh.nativeapp.crypto.SealOpen
import com.forkmesh.nativeapp.crypto.SealPolicy
import com.forkmesh.nativeapp.crypto.TrustChange
import com.forkmesh.nativeapp.crypto.TrustState
import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.json.jsonLong
import com.forkmesh.nativeapp.json.jsonMaps
import com.forkmesh.nativeapp.json.jsonText
import com.forkmesh.nativeapp.model.ChatMessage
import com.forkmesh.nativeapp.model.ChatReceipt
import com.forkmesh.nativeapp.model.ChatReceiptTimes
import com.forkmesh.nativeapp.model.OrgApiException
import com.forkmesh.nativeapp.security.SecretStore
import java.net.URLEncoder
import java.util.Base64
import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.atomic.AtomicInteger

data class DirectConversation(
    val id: String,
    val otherUser: String,
    val updatedAt: Long = 0,
    val keyVersion: Int = 1,
    val unreadCount: Int = 0,
) {
    val conversation: String get() = DirectMessageFrames.PREFIX + id
}

enum class MeshRoute(val label: String) { WIFI("Wi-Fi"), BLUETOOTH("Bluetooth") }

interface DirectMeshLinks {
    fun linkedKids(): Map<String, MeshRoute>

    fun peerFocused(kid: String, channel: String): Boolean

    fun sendDirect(kid: String, frame: Map<String, Any?>): Boolean

    fun setFocus(channel: String, on: Boolean, kids: Set<String>)
}

data class DirectDevice(
    val kid: String,
    val acct: String,
    val state: TrustState,
    val verified: Boolean,
    val firstSeen: Long,
    val ownAccount: Boolean = false,
    val node: String = "",
)

object DirectMessageFrames {
    const val PREFIX = "direct:"
    const val ENDPOINT = "/api/chat/direct-messages"
    const val PAGE_LIMIT = 50
    const val MAX_CONVERSATIONS = 200
    const val MAX_MESSAGES_PER_ROOM = 200
    const val MAX_FILE_BYTES = 1024 * 1024
    const val HISTORY_END = "forkmesh-history-end"
    const val SERVER_COPIES = "forkmesh-server-copies"
    const val RECEIVED = "forkmesh-received"
    const val SUPERSEDED = "forkmesh-superseded"
    const val DELIVERED = "forkmesh-delivered"
    const val READ = "forkmesh-read"
    const val RECEIPTS_MAX = 256
    const val PENDING_MUTATIONS_MAX = 128
    const val RECEIVED_REFS_PER_FRAME = 64
    const val OUTBOX_MAX = 64
    const val OUTBOX_MAX_AGE_MS = 7L * 24 * 60 * 60 * 1000
    const val RECONNECT_MAX_SECONDS = 300
    const val SYSTEM_SENDER = "forkmesh-system"
    const val DIRECT_SEALED = "direct-sealed"
    const val DIRECT_ACK = "direct-ack"
    const val FOCUS = "focus"
    const val DIRECT_ACK_TIMEOUT_MS = 3_000L
    const val BLUETOOTH_FILE_BYTES = 256 * 1024
    const val ROUTE_DIRECT_ONLY = "Direct only"
    private val REF = Regex("^[0-9a-f]{40}$")

    fun supersededFrames(refs: List<String>): List<String> =
        refs.chunked(RECEIVED_REFS_PER_FRAME).map {
            Json.stringify(linkedMapOf<String, Any?>("kind" to SUPERSEDED, "v" to 1, "refs" to it))
        }

    fun reconnectDelayMs(attempt: Int): Long =
        (1 shl attempt.coerceIn(0, 9)).coerceIn(1, RECONNECT_MAX_SECONDS) * 1000L

    fun relayRef(envelope: Map<String, Any?>): String {
        val ref = envelope["ref"] as? String ?: return ""
        return if (REF.matches(ref)) ref else ""
    }

    fun deliveredRefs(envelope: Map<String, Any?>): List<String>? {
        if (envelope["kind"] != DELIVERED) return null
        val refs = envelope["refs"] as? List<*> ?: return emptyList()
        return refs.filterIsInstance<String>().filter { REF.matches(it) }.distinct().take(RECEIVED_REFS_PER_FRAME)
    }

    fun isRead(envelope: Map<String, Any?>): Boolean = envelope["kind"] == READ

    fun isMutation(type: String): Boolean = type == "edit" || type == "delete"

    fun roomName(record: DirectConversation): String = "chat-direct:${record.id}:v${record.keyVersion}"

    fun trustScope(conversationId: String): String = PREFIX + conversationId

    fun pendingDeviceLine(name: String): String =
        "A new device for $name wants to join this chat. Check the safety number, then approve or block."

    fun safetyChangedLine(name: String): String = "Your safety number with $name changed"

    fun routeLabel(route: MeshRoute): String = "Direct \u00b7 ${route.label}"

    fun directSealed(channel: String, frame: Map<String, Any?>): Map<String, Any?> =
        linkedMapOf("type" to DIRECT_SEALED, "channel" to channel, "frame" to frame)

    fun resetFrame(senderId: String): Map<String, Any?> = linkedMapOf("type" to ChatV4.RESET_TYPE, "senderId" to senderId)

    fun isReset(plain: Map<String, Any?>): Boolean = plain["type"] == ChatV4.RESET_TYPE

    fun directAck(channel: String, id: String): Map<String, Any?> = linkedMapOf("type" to DIRECT_ACK, "channel" to channel, "id" to id)

    fun focusFrame(channel: String, on: Boolean): Map<String, Any?> = linkedMapOf("type" to FOCUS, "channel" to channel, "on" to on)

    fun fileBytes(plain: Map<String, Any?>): Int {
        val file = plain["file"] as? String ?: return 0
        return file.length / 4 * 3
    }

    fun safetyGroups(digits: String): List<String> = digits.chunked(5)

    fun serverCopies(envelope: Map<String, Any?>): Int? {
        if (envelope["kind"] != SERVER_COPIES) return null
        val stored = (envelope["stored"] as? Number)?.toLong() ?: return null
        return if (stored in 0..Int.MAX_VALUE) stored.toInt() else null
    }

    fun receivedFrames(refs: List<String>): List<String> =
        refs.chunked(RECEIVED_REFS_PER_FRAME).map {
            Json.stringify(linkedMapOf<String, Any?>("kind" to RECEIVED, "v" to 1, "refs" to it))
        }

    fun serverCopiesLabel(stored: Int): String = when (stored) {
        0 -> "Not stored on the server"
        1 -> "1 message on the server"
        else -> "$stored messages on the server"
    }

    fun isDirect(conversation: String): Boolean =
        conversation.startsWith(PREFIX) && OfficeFrames.validChannelId(conversation.removePrefix(PREFIX))

    fun conversationId(conversation: String): String =
        if (isDirect(conversation)) conversation.removePrefix(PREFIX) else ""

    fun parseConversation(value: Map<String, Any?>): DirectConversation? {
        val id = jsonText(value["id"])
        if (!OfficeFrames.validChannelId(id)) return null
        val other = jsonText(value["otherUser"]).trim().lowercase().take(ChatLimits.MAX_DISPLAY_NAME_CHARS)
        if (other.isEmpty()) return null
        return DirectConversation(
            id = id,
            otherUser = other,
            updatedAt = jsonLong(value["updatedAt"]),
            keyVersion = maxOf(1L, jsonLong(value["keyVersion"], 1)).toInt(),
            unreadCount = maxOf(0L, jsonLong(value["unreadCount"])).toInt(),
        )
    }

    fun conversations(payload: Map<String, Any?>): List<DirectConversation> {
        val out = ArrayList<DirectConversation>()
        val seen = HashSet<String>()
        for (value in jsonMaps(payload["conversations"])) {
            if (out.size >= MAX_CONVERSATIONS) break
            val parsed = parseConversation(value) ?: continue
            if (seen.add(parsed.id)) out.add(parsed)
        }
        return out
    }

    fun users(payload: Map<String, Any?>): List<String> {
        val out = ArrayList<String>()
        for (value in jsonMaps(payload["users"])) {
            val name = jsonText(value["name"]).trim().lowercase().take(ChatLimits.MAX_DISPLAY_NAME_CHARS)
            if (name.isNotEmpty() && name !in out) out.add(name)
        }
        return out
    }

    fun chatMessage(plain: Map<String, Any?>, record: DirectConversation, selfId: String): ChatMessage? {
        if (jsonText(plain["type"]) != "chat") return null
        if (jsonText(plain["channel"]) != record.conversation) return null
        val id = jsonText(plain["id"])
        if (id.isEmpty() || id.length > 64) return null
        val viewOnce = ViewOnceCodec.parse(plain[ViewOnceCodec.FIELD])
        val text = viewOnce?.kind?.text ?: jsonText(plain["text"])
        if (text.length > ChatLimits.MAX_TEXT_CHARS) return null
        val fileName = if (viewOnce != null) "" else jsonText(plain["fileName"]).take(ChatLimits.MAX_FILE_NAME_CHARS)
        if (text.isEmpty() && fileName.isEmpty()) return null
        val senderId = jsonText(plain["senderId"]).take(64)
        val sender = jsonText(plain["sender"]).take(ChatLimits.MAX_DISPLAY_NAME_CHARS).ifEmpty { record.otherUser }
        val ts = plain["ts"]
        return ChatMessage(
            id = id,
            conversation = record.conversation,
            senderId = senderId,
            senderName = sender,
            text = text,
            timestamp = if (ts is Number) ts.toLong() else 0L,
            self = selfId.isNotEmpty() && senderId == selfId,
            fileName = fileName,
        )
    }

    fun threadReply(plain: Map<String, Any?>, record: DirectConversation, selfId: String): ChatMessage? {
        if (jsonText(plain["type"]) != "thread-reply") return null
        if (jsonText(plain["rootId"]).isEmpty()) return null
        val asChat = LinkedHashMap(plain)
        asChat["type"] = "chat"
        return chatMessage(asChat, record, selfId)
    }

    fun isCallFrame(plain: Map<String, Any?>): Boolean = jsonText(plain["type"]).startsWith("call-")

    fun groupChannelName(names: List<String>, suffix: String = "%04x".format((0..0xffff).random())): String {
        val parts = ArrayList<String>()
        for (name in names) {
            val slug = StringBuilder()
            var lastDash = true
            for (character in name.lowercase()) {
                val ok = character in 'a'..'z' || character in '0'..'9'
                if (ok) {
                    slug.append(character)
                    lastDash = false
                } else if (!lastDash) {
                    slug.append('-')
                    lastDash = true
                }
            }
            val cut = slug.toString().trimEnd('-').take(10).trimEnd('-')
            if (cut.isNotEmpty()) parts.add(cut)
        }
        var base = parts.joinToString("-").take(34).trimEnd('-')
        if (base.isEmpty()) base = "group"
        return "$base-$suffix"
    }

    fun errorMessage(code: String): String = when (code) {
        "user_not_found" -> "That active registered user was not found."
        "cannot_message_self" -> "Choose another person to message."
        "rate_limited" -> "Too many new conversations. Try again later."
        "invalid_session" -> "Sign in again to use direct messages."
        "" -> "The direct message could not be opened."
        else -> code.replace('_', ' ')
    }
}

class DirectMessageException(val code: String) : RuntimeException(
    if (code == "unavailable") "Direct messages could not be reached." else DirectMessageFrames.errorMessage(code),
)

class DirectMessageService(
    private val api: ChatAccountCalling,
    private val connector: OfficeSocketConnector,
    private val secureStore: SecretStore,
    private val origin: () -> String?,
    private val selfId: () -> String,
    private val displayName: () -> String,
    private val now: () -> Long = System::currentTimeMillis,
    private val platform: String = "android",
    private val loop: EventLoop = sharedLoop,
    private val account: () -> String = { "" },
) {
    private class OutboxEntry(
        var ref: String,
        val plain: Map<String, Any?>,
        var kids: List<String>,
        val notify: Boolean,
        val ts: Long,
    )

    private class RoomSocket(
        val conversationId: String,
        val crypto: RoomCrypto,
        val seal: RoomSeal,
        val outbox: ArrayList<OutboxEntry> = ArrayList(),
    ) {
        var socket: OfficeSocket? = null
        var dead = false
        var historyEnded = false
        var resealAsked = false
        val replayRefs = ArrayList<String>()
        val queued = ArrayList<String>()
    }

    private val lock = Any()
    private val sockets = HashMap<String, RoomSocket>()
    private val outboxes = HashMap<String, ArrayList<OutboxEntry>>()
    private val resets = HashMap<String, Long>()
    private val openLocks = HashMap<String, Any>()
    private val retries = HashMap<String, Int>()
    private val timers = HashMap<String, LoopTimer>()
    private var epoch = 0
    private val cryptos = HashMap<String, RoomCrypto>()
    private val seals = HashMap<String, RoomSeal>()
    private val roomNames = HashMap<String, String>()
    private val seen = HashMap<String, HashSet<String>>()
    private val history = HashMap<String, List<ChatMessage>>()
    private val sequence = AtomicInteger()
    private val listeners = CopyOnWriteArrayList<() -> Unit>()
    private val serverCopies = HashMap<String, Int>()
    private val liveBudgets = HashMap<String, DirectLiveBudget>()
    private val media = ViewOnceClient(api)
    private val heldReceipts = LinkedHashMap<String, Pair<String, LinkedHashSet<String>>>()

    val viewOnce = ViewOnceStore()

    private class Receipt(val conversationId: String, var state: ChatReceipt)

    private val receipts = LinkedHashMap<String, Receipt>()
    private val receiptTimes = HashMap<String, ChatReceiptTimes>()
    private val refIds = LinkedHashMap<String, String>()
    private val idRefs = LinkedHashMap<String, String>()
    private val pendingMutations = LinkedHashMap<String, MutableList<Map<String, Any?>>>()
    private val removed = LinkedHashMap<String, Boolean>()

    @Volatile
    var replyAuthorOf: ((String) -> String?)? = null

    @Volatile
    var onRemoved: ((String, String) -> Unit)? = null

    @Volatile
    private var focused = ""

    @Volatile
    var records: List<DirectConversation> = emptyList()
        private set

    @Volatile
    var nextCursor: String = ""
        private set

    @Volatile
    var lastError: String? = null
        private set

    @Volatile
    var loading: Boolean = false
        private set

    @Volatile
    var onMessage: ((ChatMessage) -> Unit)? = null

    @Volatile
    var onCallFrame: ((Map<String, Any?>, String) -> Unit)? = null

    @Volatile
    var onFrame: ((Map<String, Any?>, String) -> Unit)? = null

    @Volatile
    var onLiveFrame: ((Map<String, Any?>, String) -> Unit)? = null

    @Volatile
    var mesh: DirectMeshLinks? = null

    private class PendingDirect(val record: DirectConversation, val plain: Map<String, Any?>, val notify: Boolean, val kids: Set<String>) {
        var timer: LoopTimer? = null
    }

    private val pendingDirect = LinkedHashMap<String, PendingDirect>()

    @Volatile
    private var screenFocused = ""

    fun addListener(listener: () -> Unit) {
        listeners.add(listener)
    }

    fun removeListener(listener: () -> Unit) {
        listeners.remove(listener)
    }

    private fun notifyListeners() {
        for (listener in listeners) runCatching { listener() }
    }

    val conversations: List<String> get() = records.map { it.conversation }

    val unreadTotal: Int get() = records.sumOf { it.unreadCount }

    fun record(conversation: String): DirectConversation? = records.firstOrNull { it.conversation == conversation }

    fun messages(conversation: String): List<ChatMessage> = synchronized(lock) { history[conversation].orEmpty() }

    fun receipt(messageId: String): ChatReceipt? = synchronized(lock) { receipts[messageId]?.state }

    fun receiptTime(messageId: String): ChatReceiptTimes = synchronized(lock) { receiptTimes[messageId] ?: ChatReceiptTimes() }

    fun receiptTimeSnapshot(): Map<String, ChatReceiptTimes> = synchronized(lock) {
        receiptTimes.filterKeys { receipts[it]?.state == ChatReceipt.DELIVERED || receipts[it]?.state == ChatReceipt.READ }
    }

    fun historySnapshot(): Map<String, List<ChatMessage>> = synchronized(lock) { HashMap(history) }

    fun receiptSnapshot(): Map<String, ChatReceipt> = synchronized(lock) {
        receipts.filterValues { it.state == ChatReceipt.DELIVERED || it.state == ChatReceipt.READ }.mapValues { it.value.state }
    }

    fun restore(messages: List<ChatMessage>, saved: Map<String, ChatReceipt>, times: Map<String, ChatReceiptTimes> = emptyMap()) {
        val owners = HashMap<String, String>()
        var changed = false
        for (message in messages) {
            if (!DirectMessageFrames.isDirect(message.conversation)) continue
            if (synchronized(lock) { removed.containsKey(message.id) }) continue
            val conversationId = DirectMessageFrames.conversationId(message.conversation)
            owners[message.id] = conversationId
            if (append(message, conversationId)) changed = true
        }
        for ((id, receipt) in saved) {
            if (receipt != ChatReceipt.DELIVERED && receipt != ChatReceipt.READ) continue
            val conversationId = owners[id] ?: continue
            track(conversationId, id, "", receipt, stamped = times[id] ?: ChatReceiptTimes())
            changed = true
        }
        if (changed) notifyListeners()
    }

    fun clear() {
        closeAll()
        synchronized(lock) {
            for (pending in pendingDirect.values) pending.timer?.cancel()
            pendingDirect.clear()
            records = emptyList()
            nextCursor = ""
            history.clear()
            seen.clear()
            outboxes.clear()
            receipts.clear()
            receiptTimes.clear()
            refIds.clear()
            idRefs.clear()
            pendingMutations.clear()
            removed.clear()
            heldReceipts.clear()
            serverCopies.clear()
            liveBudgets.clear()
            resets.clear()
            seals.clear()
            roomNames.clear()
            focused = ""
            screenFocused = ""
        }
        viewOnce.clear()
        notifyListeners()
    }

    fun serverCopies(conversation: String): Int? =
        synchronized(lock) { serverCopies[DirectMessageFrames.conversationId(conversation)] }

    fun isConnected(conversation: String): Boolean =
        synchronized(lock) { sockets[DirectMessageFrames.conversationId(conversation)]?.socket != null }

    fun refresh() {
        val shows = records.isEmpty() && !loading
        if (shows) {
            loading = true
            notifyListeners()
        }
        var changed = shows
        try {
            val payload = call("GET", "${DirectMessageFrames.ENDPOINT}?limit=${DirectMessageFrames.PAGE_LIMIT}", null)
            val before = records
            apply(DirectMessageFrames.conversations(payload), replacing = true)
            if (records != before) changed = true
            val cursor = jsonText(payload["nextCursor"])
            if (cursor != nextCursor) {
                nextCursor = cursor
                changed = true
            }
            if (lastError != null) {
                lastError = null
                changed = true
            }
        } catch (error: Exception) {
            val message = error.message ?: error.toString()
            if (lastError != message) {
                lastError = message
                changed = true
            }
        } finally {
            if (shows) loading = false
            if (changed) notifyListeners()
        }
        keepOpen()
    }

    fun loadMore() {
        val cursor = nextCursor
        if (cursor.isEmpty()) return
        try {
            val encoded = URLEncoder.encode(cursor, "UTF-8")
            val payload = call(
                "GET", "${DirectMessageFrames.ENDPOINT}?limit=${DirectMessageFrames.PAGE_LIMIT}&cursor=$encoded", null)
            apply(DirectMessageFrames.conversations(payload), replacing = false)
            nextCursor = jsonText(payload["nextCursor"])
        } catch (error: Exception) {
            lastError = error.message ?: error.toString()
        } finally {
            notifyListeners()
        }
        keepOpen()
    }

    private fun apply(incoming: List<DirectConversation>, replacing: Boolean) {
        val closing = synchronized(lock) {
            val merged = ArrayList(if (replacing) emptyList() else records)
            for (conversation in incoming) if (merged.none { it.id == conversation.id }) merged.add(conversation)
            val gone = if (replacing) {
                records.filter { old -> merged.none { it.id == old.id } }
            } else {
                emptyList()
            }
            for (old in gone) {
                outboxes.remove(old.id)
                history.remove(old.conversation)
                viewOnce.forgetConversation(old.conversation)
            }
            if (records != merged) records = merged
            gone.mapNotNull { sockets.remove(it.id) }
        }
        for (holder in closing) holder.socket?.close()
    }

    fun focus(conversation: String) {
        focused = DirectMessageFrames.conversationId(conversation)
    }

    private fun wantedLocked(id: String): Boolean =
        id.isNotEmpty() && (id == focused || records.any { it.id == id && it.unreadCount > 0 })

    fun keepOpen() {
        val due = synchronized(lock) {
            records.filter { wantedLocked(it.id) && sockets[it.id]?.socket == null }.map { it.conversation }
        }
        for (conversation in due) open(conversation)
    }

    private fun scheduleReconnect(id: String) {
        val (delay, generation) = synchronized(lock) {
            if (!wantedLocked(id) || sockets[id]?.socket != null || timers[id]?.isActive == true) return
            val attempt = retries[id] ?: 0
            retries[id] = attempt + 1
            DirectMessageFrames.reconnectDelayMs(attempt) to epoch
        }
        val timer = loop.timer(delay) {
            loop.io {
                val conversation = synchronized(lock) {
                    if (epoch != generation || !wantedLocked(id) || sockets[id]?.socket != null) null
                    else records.firstOrNull { it.id == id }?.conversation
                }
                if (conversation != null) open(conversation)
            }
        }
        synchronized(lock) { if (epoch == generation) timers[id] = timer else timer.cancel() }
    }

    fun searchUsers(query: String): List<String> {
        val trimmed = query.trim().lowercase()
        if (trimmed.isEmpty()) return emptyList()
        return try {
            val encoded = URLEncoder.encode(trimmed, "UTF-8")
            DirectMessageFrames.users(call("GET", "${DirectMessageFrames.ENDPOINT}/users?query=$encoded", null))
        } catch (_: Exception) {
            emptyList()
        }
    }

    fun start(username: String): DirectConversation {
        val normalized = username.trim().lowercase()
        if (normalized.isEmpty()) throw DirectMessageException("user_not_found")
        val payload = call("POST", DirectMessageFrames.ENDPOINT, linkedMapOf<String, Any?>("username" to normalized))
        val value = payload["conversation"]
        val parsed = (value as? Map<*, *>)
            ?.let { raw -> DirectMessageFrames.parseConversation(raw.entries.associate { jsonText(it.key) to it.value }) }
            ?: throw DirectMessageException("unavailable")
        synchronized(lock) {
            val index = records.indexOfFirst { it.id == parsed.id }
            records = if (index >= 0) {
                records.toMutableList().also { it[index] = parsed }
            } else {
                listOf(parsed) + records
            }
        }
        notifyListeners()
        return parsed
    }

    fun markRead(conversation: String) {
        val id = DirectMessageFrames.conversationId(conversation)
        if (id.isEmpty()) return
        val known = synchronized(lock) {
            val index = records.indexOfFirst { it.id == id }
            if (index < 0) return@synchronized false
            if (records[index].unreadCount != 0) {
                records = records.toMutableList().also { it[index] = it[index].copy(unreadCount = 0) }
            }
            true
        }
        if (!known) return
        runCatching { call("POST", "${DirectMessageFrames.ENDPOINT}/$id/read", null) }
        notifyListeners()
    }

    fun clearUnread(conversation: String) {
        val id = DirectMessageFrames.conversationId(conversation)
        val cleared = synchronized(lock) {
            val index = records.indexOfFirst { it.id == id }
            if (index < 0 || records[index].unreadCount == 0) return@synchronized false
            records = records.toMutableList().also { it[index] = it[index].copy(unreadCount = 0) }
            true
        }
        if (cleared) notifyListeners()
    }

    fun bumpUnread(conversation: String) {
        val id = DirectMessageFrames.conversationId(conversation)
        synchronized(lock) {
            val index = records.indexOfFirst { it.id == id }
            if (index < 0) return
            records = records.toMutableList().also { it[index] = it[index].copy(unreadCount = it[index].unreadCount + 1) }
        }
        notifyListeners()
    }

    private fun derive(room: String, passphrase: String): RoomCrypto {
        val key = "$room|$passphrase"
        synchronized(lock) { cryptos[key] }?.let { return it }
        val crypto = RoomCrypto.withPassphrase(room, passphrase)
        synchronized(lock) { cryptos[key] = crypto }
        return crypto
    }

    private fun sealAccount(): String = account().trim().lowercase().ifEmpty { selfId() }

    private fun participants(conversationId: String): Set<String>? {
        val other = records.firstOrNull { it.id == conversationId }?.otherUser ?: return null
        return setOf(sealAccount(), other)
    }

    private fun seal(room: String, conversationId: String): RoomSeal {
        synchronized(lock) { seals[room] }?.let {
            it.setIdentity(sealAccount(), selfId())
            it.participants = participants(conversationId)
            return it
        }
        val loaded = RoomSeal.loadOrCreate(
            room,
            scope = DirectMessageFrames.trustScope(conversationId),
            policy = SealPolicy.STRICT,
            acct = sealAccount(),
            secureStore = secureStore,
            clock = now,
            node = selfId(),
        )
        loaded.participants = participants(conversationId)
        synchronized(lock) { seals[room] = loaded }
        return loaded
    }

    private fun sealFor(record: DirectConversation): RoomSeal {
        val room = synchronized(lock) { roomNames[record.id] } ?: DirectMessageFrames.roomName(record)
        return seal(room, record.id)
    }

    private fun trustFor(record: DirectConversation): ChatTrust =
        ChatTrust.forStore(secureStore, DirectMessageFrames.trustScope(record.id))

    private fun ownKid(): String = ChatDeviceKeys.forStore(secureStore).kid

    fun openRinging(conversation: String): Boolean {
        if (!conversation.startsWith(DirectMessageFrames.PREFIX)) return false
        if (record(conversation) == null) refresh()
        return open(conversation)
    }

    fun open(conversation: String): Boolean {
        val record = record(conversation) ?: return false
        val lockForRoom = synchronized(lock) { openLocks.getOrPut(record.id) { Any() } }
        val opened = synchronized(lockForRoom) { connect(record) }
        if (!opened) scheduleReconnect(record.id)
        return opened
    }

    private fun connect(record: DirectConversation): Boolean {
        if (synchronized(lock) { sockets[record.id]?.socket != null }) return true
        val base = origin() ?: return false
        val generation = synchronized(lock) { epoch }
        return try {
            val access = call("GET", "${DirectMessageFrames.ENDPOINT}/${record.id}/room-access", null)
            val roomName = jsonText(access["room"])
            val passphrase = jsonText(access["passphrase"])
            val url = OfficeFrames.webSocketUrl(jsonText(access["webSocketUrl"]), base)
            if (roomName.isEmpty() || passphrase.isEmpty() || url == null) {
                lastError = DirectMessageException("unavailable").message
                notifyListeners()
                return false
            }
            val crypto = derive(roomName, passphrase)
            val seal = seal(roomName, record.id)
            synchronized(lock) { roomNames[record.id] = roomName }
            val holder = RoomSocket(record.id, crypto, seal, synchronized(lock) { outboxes.getOrPut(record.id) { ArrayList() } })
            val socket = connector.connect(
                url,
                onText = { text -> receive(text, holder) },
                onClosed = { dropped(holder) },
            )
            val queued = synchronized(lock) {
                if (holder.dead || epoch != generation) {
                    null
                } else {
                    holder.socket = socket
                    sockets[record.id] = holder
                    holder.queued.toList().also { holder.queued.clear() }
                }
            }
            if (queued == null) {
                socket.close()
                return false
            }
            for (text in queued) socket.sendText(text)
            advertise(holder, persist = seal.shouldPersistAdvertisement(now()))
            for (kid in trustFor(record).heldKids(roomName)) {
                if (trustFor(record).record(kid)?.state == TrustState.APPROVED) releaseHeld(record, seal, kid, holder)
            }
            notifyListeners()
            true
        } catch (error: Exception) {
            lastError = error.message ?: error.toString()
            notifyListeners()
            false
        }
    }

    private fun receive(text: String, holder: RoomSocket) {
        val record = records.firstOrNull { it.id == holder.conversationId } ?: return
        val envelope = try {
            Json.parseObject(text)
        } catch (_: Exception) {
            return
        }
        if (envelope.isEmpty()) return
        if (envelope["kind"] == DirectMessageFrames.HISTORY_END) {
            val refs = synchronized(lock) {
                holder.historyEnded = true
                retries.remove(holder.conversationId)
                holder.replayRefs.toList().also { holder.replayRefs.clear() }
            }
            sendReceived(holder, refs)
            reseal(holder)
            records.firstOrNull { it.id == holder.conversationId }?.let { done -> loop.io { refreshViewOnce(done.conversation) } }
            return
        }
        ViewOnceFrames.relayState(envelope)?.let { (mediaId, state) ->
            applyViewOnceState(holder, mediaId, state)
            return
        }
        DirectMessageFrames.serverCopies(envelope)?.let { stored ->
            synchronized(lock) {
                serverCopies[holder.conversationId] = stored
                if (stored == 0) holder.outbox.clear()
            }
            notifyListeners()
            return
        }
        DirectLiveFrames.budget(envelope)?.let { budget ->
            synchronized(lock) { liveBudgets[holder.conversationId] = budget }
            return
        }
        OfficeFrames.recipientsAck(envelope)?.let { (ref, _) ->
            if (advance(listOf(ref), ChatReceipt.SENT)) notifyListeners()
            return
        }
        DirectMessageFrames.deliveredRefs(envelope)?.let { refs ->
            if (advance(refs, ChatReceipt.DELIVERED)) notifyListeners()
            return
        }
        if (DirectMessageFrames.isRead(envelope)) {
            if (peerRead(holder.conversationId)) notifyListeners()
            return
        }
        val framed = holder.crypto.decrypt(envelope) ?: return
        val replay = envelope["historyReplay"] == true
        if (RoomSeal.isLegacyFrame(framed)) return
        if (RoomSeal.isKeyFrame(framed)) {
            acceptBundle(record, holder.seal, framed, holder, replay)
            return
        }
        if (!holder.seal.accepts(framed)) return
        val ref = DirectMessageFrames.relayRef(envelope)
        val meta = linkedMapOf<String, Any?>("ref" to ref, "replay" to replay, "notify" to (envelope["notify"] == true))
        processSealed(record, holder, framed, meta)
    }

    private fun acceptBundle(record: DirectConversation, seal: RoomSeal, frame: Map<String, Any?>, holder: RoomSocket?, replay: Boolean) {
        val outcome = seal.acceptKeyFrame(frame, now()) ?: return
        if (outcome.isNew && holder != null && !replay) advertise(holder)
        if (outcome.isNew && outcome.state == TrustState.PENDING) {
            systemLine(record, "pending-${outcome.kid}", DirectMessageFrames.pendingDeviceLine(deviceOwnerName(record, outcome.acct)))
        }
        if (outcome.state == TrustState.APPROVED && outcome.updated) {
            if (holder != null) peerLearned(holder)
            releaseHeld(record, seal, outcome.kid, holder)
        }
        notifyListeners()
    }

    private fun deviceOwnerName(record: DirectConversation, acct: String): String =
        if (acct == sealAccount()) "your account" else record.otherUser

    private fun releaseHeld(record: DirectConversation, seal: RoomSeal, kid: String, holder: RoomSocket?) {
        val live = holder ?: synchronized(lock) { sockets[record.id] }
        for (held in seal.takeHeld(kid)) {
            if (live != null && seal === live.seal) {
                processSealed(record, live, held.frame, held.meta)
            } else {
                openDirectFrame(record, seal, held.frame, kid, held.meta)
            }
        }
    }

    private fun processSealed(record: DirectConversation, holder: RoomSocket, framed: Map<String, Any?>, meta: Map<String, Any?>) {
        val replay = meta["replay"] == true
        val ref = jsonText(meta["ref"])
        val kids = RoomSeal.wrappedKeyIds(framed)
        when (val result = holder.seal.open(framed, now(), history = replay, meta = meta)) {
            is SealOpen.Opened -> {
                val plain = result.plain
                if (!senderMatches(result, plain)) return
                if (DirectMessageFrames.isReset(plain)) {
                    resealTo(record, holder.seal, result.senderKid)
                    acknowledgeRef(holder, ref, replay)
                    return
                }
                handle(plain, record, !replay, ref)
                if (replay) noteOwn(holder, plain, ref, kids, meta["notify"] == true)
                if (ref.isNotEmpty() && holdReceipt(plain, holder.conversationId, ref)) return
                acknowledgeRef(holder, ref, replay)
            }
            is SealOpen.NotForMe -> askForReseal(holder)
            is SealOpen.Replay -> acknowledgeRef(holder, ref, replay)
            is SealOpen.Held -> {
                if (result.blocked) return
                if (holder.seal.keyId !in kids) askForReseal(holder)
                notifyListeners()
            }
            is SealOpen.NoSession -> recover(record, holder.seal, result.kid, framed, holder)
            is SealOpen.Own, is SealOpen.Invalid -> {}
        }
    }

    private fun recover(record: DirectConversation, seal: RoomSeal, kid: String, frame: Map<String, Any?>, holder: RoomSocket?, via: String? = null) {
        val ctr = ChatV3.integer(frame["ctr"]) ?: return
        if (seal.openedSince(kid, ctr)) return
        val at = now()
        val key = "${seal.room}|$kid"
        synchronized(lock) {
            val last = resets[key]
            if (last != null && at - last in 0 until ChatV4.RESET_INTERVAL_MS) return
            resets[key] = at
            while (resets.size > DirectMessageFrames.RECEIPTS_MAX) resets.remove(resets.keys.first())
        }
        seal.resetSessions(kid)
        val plain = DirectMessageFrames.resetFrame(selfId())
        val live = holder ?: synchronized(lock) { sockets[record.id] }
        if (live != null && live.seal === seal) {
            runCatching {
                val envelope = LinkedHashMap(live.crypto.encrypt(seal.seal(plain, now(), only = listOf(kid))))
                envelope["persist"] = true
                sendOrQueue(live, Json.stringify(envelope))
            }
            advertise(live, persist = true)
            return
        }
        val links = mesh ?: return
        if (via == null) return
        runCatching { links.sendDirect(via, DirectMessageFrames.directSealed(record.conversation, seal.seal(plain, now(), only = listOf(kid)))) }
    }

    private fun resealTo(record: DirectConversation, seal: RoomSeal, kid: String) {
        val holder = synchronized(lock) { sockets[record.id] } ?: return
        if (holder.seal !== seal) return
        synchronized(holder) {
            val due = synchronized(lock) {
                pruneOutbox(holder)
                holder.outbox.toList()
            }
            for (entry in due) {
                val sealed = runCatching { seal.seal(entry.plain, now(), only = listOf(kid)) }.getOrNull() ?: continue
                if (kid !in RoomSeal.wrappedKeyIds(sealed)) break
                val text = runCatching {
                    val envelope = LinkedHashMap(holder.crypto.encrypt(sealed))
                    envelope["persist"] = true
                    if (entry.notify) envelope["notify"] = true
                    envelope["resend"] = true
                    Json.stringify(envelope)
                }.getOrNull() ?: continue
                if (!sendOrQueue(holder, text)) break
            }
        }
    }

    private fun acknowledgeRef(holder: RoomSocket, ref: String, replay: Boolean) {
        if (ref.isEmpty()) return
        if (replay) {
            synchronized(lock) { if (ref !in holder.replayRefs) holder.replayRefs.add(ref) }
        } else {
            sendReceived(holder, listOf(ref))
        }
    }

    private fun senderMatches(result: SealOpen.Opened, plain: Map<String, Any?>): Boolean {
        val node = result.senderNode ?: return true
        if (jsonText(plain["senderId"]) != node) return false
        if (jsonText(plain["type"]) == "reaction" && jsonText(plain["reactorId"]).let { it.isNotEmpty() && it != node }) return false
        return true
    }

    private fun systemLine(record: DirectConversation, key: String, text: String) {
        val message = ChatMessage(
            id = "system-$key",
            conversation = record.conversation,
            senderId = DirectMessageFrames.SYSTEM_SENDER,
            senderName = "ForkMesh",
            text = text,
            timestamp = now(),
            ephemeral = true,
        )
        if (append(message, record.id)) notifyListeners()
    }

    private fun sendOrQueue(holder: RoomSocket, text: String): Boolean {
        val socket = synchronized(lock) {
            if (holder.dead) return false
            val current = holder.socket
            if (current == null) {
                holder.queued.add(text)
                return true
            }
            current
        }
        return socket.sendText(text)
    }

    private fun sendReceived(holder: RoomSocket, refs: List<String>) {
        for (frame in DirectMessageFrames.receivedFrames(refs)) sendOrQueue(holder, frame)
    }

    private fun handle(plain: Map<String, Any?>, record: DirectConversation, live: Boolean, ref: String = "") {
        if (DirectMessageFrames.isCallFrame(plain)) {
            if (live) onCallFrame?.invoke(plain, record.conversation)
            return
        }
        if (DirectLiveFrames.isLiveFrame(plain)) {
            if (live) onLiveFrame?.invoke(plain, record.conversation)
            return
        }
        val type = jsonText(plain["type"])
        if (DirectMessageFrames.isMutation(type) && !mutationAllowed(plain, record)) return
        onFrame?.invoke(plain, record.conversation)
        val me = selfId()
        val message = DirectMessageFrames.chatMessage(plain, record, me) ?: DirectMessageFrames.threadReply(plain, record, me)
        if (message == null) {
            notifyListeners()
            return
        }
        if (type == "chat") trackViewOnce(plain, message)
        val added = if (type == "chat") {
            append(message, record.id)
        } else {
            replyAuthorOf?.invoke(message.id) != null && synchronized(lock) { seen.getOrPut(record.id) { HashSet() }.add(message.id) }
        }
        if (message.self) track(record.id, message.id, ref, ChatReceipt.SENT)
        val kept = !added || applyPending(record, message.id)
        notifyListeners()
        if (added && kept && live && !message.self) onMessage?.invoke(message)
    }

    private fun authorOf(record: DirectConversation, target: String): String? =
        synchronized(lock) { history[record.conversation]?.firstOrNull { it.id == target }?.senderId }
            ?: replyAuthorOf?.invoke(target)

    private fun mutationAllowed(plain: Map<String, Any?>, record: DirectConversation): Boolean {
        val target = jsonText(plain["target"])
        val sender = jsonText(plain["senderId"])
        if (target.isEmpty() || sender.isEmpty()) return false
        val author = authorOf(record, target)
        if (author == null) {
            holdMutation(record.id, target, plain)
            return false
        }
        if (author != sender) return false
        if (jsonText(plain["type"]) == "delete") remove(record, target)
        return true
    }

    private fun holdMutation(conversationId: String, target: String, plain: Map<String, Any?>) = synchronized(lock) {
        val list = pendingMutations.getOrPut("$conversationId|$target") { ArrayList() }
        if (list.size < 8) list.add(plain)
        trim(pendingMutations, DirectMessageFrames.PENDING_MUTATIONS_MAX)
    }

    private fun applyPending(record: DirectConversation, id: String): Boolean {
        val held = synchronized(lock) { pendingMutations.remove("${record.id}|$id") } ?: return true
        for (frame in held) handle(frame, record, false)
        return authorOf(record, id) != null
    }

    private fun remove(record: DirectConversation, target: String) {
        synchronized(lock) {
            history[record.conversation]?.let { list -> history[record.conversation] = list.filter { it.id != target } }
            seen.getOrPut(record.id) { HashSet() }.add(target)
            removed[target] = true
            trim(removed, DirectMessageFrames.RECEIPTS_MAX)
            receipts.remove(target)
            receiptTimes.remove(target)
            sockets[record.id]?.outbox?.removeAll {
                jsonText(it.plain["id"]) == target || (jsonText(it.plain["target"]) == target && jsonText(it.plain["type"]) != "delete")
            }
        }
        onRemoved?.invoke(record.conversation, target)
    }

    private fun <V> trim(map: LinkedHashMap<String, V>, max: Int) {
        while (map.size > max) map.remove(map.keys.first())
    }

    private fun linkRef(id: String, ref: String) {
        if (id.isEmpty() || ref.isEmpty()) return
        refIds[ref] = id
        idRefs.remove(id)
        idRefs[id] = ref
        trim(refIds, DirectMessageFrames.RECEIPTS_MAX)
        trim(idRefs, DirectMessageFrames.RECEIPTS_MAX)
    }

    private fun track(conversationId: String, id: String, ref: String, state: ChatReceipt, stamped: ChatReceiptTimes? = null) = synchronized(lock) {
        val current = receipts[id]
        val moved = if (current == null) {
            receipts[id] = Receipt(conversationId, state)
            while (receipts.size > DirectMessageFrames.RECEIPTS_MAX) {
                val evicted = receipts.keys.first()
                receipts.remove(evicted)
                receiptTimes.remove(evicted)
            }
            true
        } else if (state > current.state) {
            current.state = state
            true
        } else {
            false
        }
        if (moved) {
            if (stamped != null) {
                if (!stamped.isEmpty) receiptTimes[id] = stamped
            } else {
                stamp(id, state)
            }
        }
        linkRef(id, ref)
    }

    private fun stamp(id: String, state: ChatReceipt) {
        val times = receiptTimes[id] ?: ChatReceiptTimes()
        receiptTimes[id] = when (state) {
            ChatReceipt.DELIVERED -> times.copy(delivered = times.delivered ?: now())
            ChatReceipt.READ -> times.copy(read = times.read ?: now())
            else -> return
        }
    }

    private fun advance(refs: List<String>, state: ChatReceipt): Boolean = synchronized(lock) {
        var changed = false
        for (ref in refs) {
            val id = refIds[ref] ?: continue
            val receipt = receipts[id] ?: continue
            if (state > receipt.state) {
                receipt.state = state
                stamp(id, state)
                changed = true
            }
        }
        changed
    }

    private fun peerRead(conversationId: String): Boolean = synchronized(lock) {
        var changed = false
        for ((id, receipt) in receipts) {
            if (receipt.conversationId != conversationId) continue
            if (receipt.state == ChatReceipt.SENT || receipt.state == ChatReceipt.DELIVERED) {
                receipt.state = ChatReceipt.READ
                stamp(id, ChatReceipt.READ)
                changed = true
            }
        }
        changed
    }

    private fun append(message: ChatMessage, conversationId: String): Boolean = synchronized(lock) {
        val ids = seen.getOrPut(conversationId) { HashSet() }
        if (!ids.add(message.id)) return@synchronized false
        var list = insertSorted(history[message.conversation].orEmpty(), message).toList()
        if (list.size > DirectMessageFrames.MAX_MESSAGES_PER_ROOM) {
            list = list.takeLast(DirectMessageFrames.MAX_MESSAGES_PER_ROOM)
        }
        history[message.conversation] = list
        true
    }

    private fun noteOwn(holder: RoomSocket, plain: Map<String, Any?>, ref: String, kids: List<String>, notify: Boolean) {
        val me = selfId()
        if (ref.isEmpty() || me.isEmpty() || jsonText(plain["senderId"]) != me || DirectMessageFrames.isCallFrame(plain)) return
        if (synchronized(lock) { removed.containsKey(jsonText(plain["id"])) }) return
        remember(holder, OutboxEntry(ref, plain, kids, notify, (plain["ts"] as? Number)?.toLong() ?: now()))
    }

    private fun remember(holder: RoomSocket, entry: OutboxEntry) {
        val id = jsonText(entry.plain["id"])
        synchronized(lock) {
            holder.outbox.removeAll { it.ref == entry.ref || (id.isNotEmpty() && jsonText(it.plain["id"]) == id) }
            holder.outbox.add(entry)
            pruneOutbox(holder)
        }
    }

    private fun pruneOutbox(holder: RoomSocket) {
        val cutoff = now() - DirectMessageFrames.OUTBOX_MAX_AGE_MS
        holder.outbox.removeAll { it.ts < cutoff }
        while (holder.outbox.size > DirectMessageFrames.OUTBOX_MAX) holder.outbox.removeAt(0)
    }

    private fun peerLearned(holder: RoomSocket) {
        if (synchronized(lock) { holder.historyEnded }) reseal(holder)
    }

    private fun askForReseal(holder: RoomSocket) {
        val first = synchronized(lock) {
            val already = holder.resealAsked
            holder.resealAsked = true
            !already
        }
        if (first) advertise(holder, persist = true)
    }

    private fun reseal(holder: RoomSocket) {
        synchronized(holder) {
            val peers = holder.seal.peerKeyIds
            if (peers.isEmpty()) return
            val due = synchronized(lock) {
                pruneOutbox(holder)
                holder.outbox.filter { !it.kids.containsAll(peers) }
            }
            val superseded = ArrayList<String>()
            for (entry in due) {
                val built = runCatching {
                    val sealed = holder.seal.seal(entry.plain)
                    val envelope = LinkedHashMap(holder.crypto.encrypt(sealed))
                    envelope["persist"] = true
                    if (entry.notify) envelope["notify"] = true
                    envelope["resend"] = true
                    Json.stringify(envelope) to RoomSeal.wrappedKeyIds(sealed)
                }.getOrNull() ?: continue
                val (text, kids) = built
                if (!sendOrQueue(holder, text)) break
                synchronized(lock) {
                    superseded.add(entry.ref)
                    entry.ref = OfficeFrames.sentRef(text)
                    entry.kids = kids
                    linkRef(jsonText(entry.plain["id"]), entry.ref)
                }
            }
            for (frame in DirectMessageFrames.supersededFrames(superseded)) sendOrQueue(holder, frame)
        }
    }

    internal fun frame(record: DirectConversation, type: String, extra: Map<String, Any?> = emptyMap()): LinkedHashMap<String, Any?> {
        val seq = sequence.incrementAndGet()
        val me = selfId()
        val at = now()
        val plain = linkedMapOf<String, Any?>(
            "type" to type,
            "id" to "$me-$at-$seq",
            "senderId" to me,
            "sender" to displayName().take(ChatLimits.MAX_DISPLAY_NAME_CHARS),
            "accountKind" to "user",
            "ts" to at,
            "platform" to platform,
            "version" to RelayRoom.CLIENT_VERSION,
            "channel" to record.conversation,
        )
        plain.putAll(extra)
        return plain
    }

    fun deviceKeys(conversation: String): List<String> {
        val record = record(conversation) ?: return emptyList()
        val own = ownKid()
        return trustFor(record).approved().map { it.kid }.filter { it != own }
    }

    fun devices(conversation: String): List<DirectDevice> {
        val record = record(conversation) ?: return emptyList()
        val own = ownKid()
        val acct = sealAccount()
        return trustFor(record).records().filter { it.kid != own }.map {
            DirectDevice(it.kid, it.acct, it.state, it.verified, it.firstSeen, ownAccount = acct.isNotEmpty() && it.acct == acct, node = it.node)
        }
    }

    fun pendingDevices(conversation: String): List<DirectDevice> = devices(conversation).filter { it.state == TrustState.PENDING }

    fun approveDevice(conversation: String, kid: String): Boolean {
        val record = record(conversation) ?: return false
        val change = trustFor(record).approve(kid, sealAccount()) ?: return false
        afterTrustChange(record, change)
        releaseHeld(record, sealFor(record), kid, null)
        synchronized(lock) { sockets[record.id] }?.let { peerLearned(it) }
        notifyListeners()
        return true
    }

    fun blockDevice(conversation: String, kid: String): Boolean {
        val record = record(conversation) ?: return false
        val change = trustFor(record).block(kid, sealAccount()) ?: return false
        afterTrustChange(record, change)
        notifyListeners()
        return true
    }

    private fun afterTrustChange(record: DirectConversation, change: TrustChange) {
        if (!change.changed || !change.hadOtherApproved) return
        systemLine(record, "safety-${now()}-${sequence.incrementAndGet()}", DirectMessageFrames.safetyChangedLine(record.otherUser))
    }

    fun safetyNumber(conversation: String): ChatV3.SafetyNumber? {
        val record = record(conversation) ?: return null
        val keys = ChatDeviceKeys.forStore(secureStore)
        val idks = LinkedHashMap<String, ByteArray>()
        idks[keys.kid] = keys.idk
        for (approved in trustFor(record).approved()) idks[approved.kid] = approved.idkBytes
        return ChatV3.safetyNumber(DirectMessageFrames.trustScope(record.id), idks.values)
    }

    fun isVerified(conversation: String): Boolean = record(conversation)?.let { trustFor(it).verified() } == true

    fun markVerified(conversation: String): Boolean {
        val record = record(conversation) ?: return false
        trustFor(record).markVerified()
        notifyListeners()
        return true
    }

    fun verifyScanned(conversation: String, scanned: String): Boolean {
        val number = safetyNumber(conversation) ?: return false
        if (scanned.trim() != number.qr) return false
        return markVerified(conversation)
    }

    fun verifySafetyCode(conversation: String, scanned: String): DirectSafetyScan {
        val number = safetyNumber(conversation) ?: return DirectSafetyScan.NOT_SAFETY_CODE
        return DirectSafetyScan.result(scanned, number.qr, verifyScanned(conversation, scanned))
    }

    fun trustNotices(conversation: String): List<DirectTrustNotice> {
        if (!DirectMessageFrames.isDirect(conversation)) return emptyList()
        return DirectTrustNotices.pending(conversation, devices(conversation), record(conversation)?.otherUser.orEmpty())
    }

    fun screenFocus(conversation: String) {
        val id = DirectMessageFrames.conversationId(conversation)
        val previous = synchronized(lock) {
            val old = screenFocused
            if (old == id) return
            screenFocused = id
            old
        }
        val links = mesh
        if (links != null) {
            if (previous.isNotEmpty()) runCatching { links.setFocus(DirectMessageFrames.PREFIX + previous, false, partnerKids(DirectMessageFrames.PREFIX + previous)) }
            if (id.isNotEmpty()) runCatching { links.setFocus(DirectMessageFrames.PREFIX + id, true, partnerKids(DirectMessageFrames.PREFIX + id)) }
        }
        notifyListeners()
    }

    fun partnerKids(conversation: String): Set<String> {
        val record = record(conversation) ?: return emptySet()
        val me = sealAccount()
        val own = ownKid()
        return trustFor(record).approved().filter { it.kid != own && it.acct != me }.map { it.kid }.toSet()
    }

    private fun linkedDevices(record: DirectConversation): Map<String, MeshRoute> {
        val links = mesh ?: return emptyMap()
        val linked = runCatching { links.linkedKids() }.getOrDefault(emptyMap())
        if (linked.isEmpty()) return emptyMap()
        val own = ownKid()
        val approved = trustFor(record).approved().map { it.kid }.filter { it != own }.toSet()
        return linked.filterKeys { it in approved }
    }

    private fun focusedOthers(record: DirectConversation, linked: Map<String, MeshRoute>): Set<String> {
        val links = mesh ?: return emptySet()
        if (screenFocused != record.id || linked.isEmpty()) return emptySet()
        val me = sealAccount()
        val trust = trustFor(record)
        return linked.keys.filter { kid ->
            trust.record(kid)?.acct != me && runCatching { links.peerFocused(kid, record.conversation) }.getOrDefault(false)
        }.toSet()
    }

    fun routeLabel(conversation: String): String {
        val record = record(conversation) ?: return ""
        val linked = linkedDevices(record)
        if (linked.isEmpty()) return ""
        if (focusedOthers(record, linked).isNotEmpty()) return DirectMessageFrames.ROUTE_DIRECT_ONLY
        val route = if (MeshRoute.WIFI in linked.values) MeshRoute.WIFI else MeshRoute.BLUETOOTH
        return DirectMessageFrames.routeLabel(route)
    }

    private class DirectSend(val kids: Set<String>, val directOnly: Boolean)

    private fun sendDirectCopies(record: DirectConversation, plain: Map<String, Any?>): DirectSend {
        val links = mesh ?: return DirectSend(emptySet(), false)
        val linked = linkedDevices(record)
        if (linked.isEmpty() || jsonText(plain["id"]).isEmpty()) return DirectSend(emptySet(), false)
        val focused = focusedOthers(record, linked)
        val size = DirectMessageFrames.fileBytes(plain)
        val seal = sealFor(record)
        val reached = LinkedHashSet<String>()
        for ((kid, route) in linked) {
            if (route == MeshRoute.BLUETOOTH && size > DirectMessageFrames.BLUETOOTH_FILE_BYTES) continue
            val sent = runCatching {
                val frame = seal.seal(plain, now(), only = listOf(kid))
                links.sendDirect(kid, DirectMessageFrames.directSealed(record.conversation, frame))
            }.getOrDefault(false)
            if (sent) reached.add(kid)
        }
        return DirectSend(reached, focused.any { it in reached })
    }

    private fun awaitAck(record: DirectConversation, plain: Map<String, Any?>, notify: Boolean, kids: Set<String>) {
        val id = jsonText(plain["id"])
        val entry = PendingDirect(record, plain, notify, kids)
        val replaced = synchronized(lock) {
            val old = pendingDirect.put(id, entry)
            while (pendingDirect.size > DirectMessageFrames.OUTBOX_MAX) {
                val oldest = pendingDirect.keys.first()
                pendingDirect.remove(oldest)?.timer?.cancel()
            }
            old
        }
        replaced?.timer?.cancel()
        entry.timer = loop.timer(DirectMessageFrames.DIRECT_ACK_TIMEOUT_MS) { loop.io { relayFallback(id) } }
    }

    private fun relayFallback(id: String) {
        val entry = synchronized(lock) { pendingDirect.remove(id) } ?: return
        entry.timer?.cancel()
        sendSealed(entry.record, entry.plain, entry.notify, direct = false)
    }

    fun meshLinksChanged() {
        val linked = runCatching { mesh?.linkedKids()?.keys }.getOrNull() ?: emptySet()
        val due = synchronized(lock) { pendingDirect.filter { (_, entry) -> entry.kids.any { it !in linked } }.keys.toList() }
        for (id in due) relayFallback(id)
        notifyListeners()
    }

    fun acceptMeshFrame(linkKid: String, frame: Map<String, Any?>) {
        val record = record(jsonText(frame["channel"])) ?: return
        when (jsonText(frame["type"])) {
            DirectMessageFrames.DIRECT_SEALED -> {
                @Suppress("UNCHECKED_CAST")
                val inner = frame["frame"] as? Map<String, Any?> ?: return
                val seal = sealFor(record)
                if (!seal.accepts(inner) || inner["kid"] != linkKid) return
                openDirectFrame(record, seal, inner, linkKid, emptyMap(), ackVia = linkKid)
            }
            DirectMessageFrames.DIRECT_ACK -> {
                val id = jsonText(frame["id"])
                val entry = synchronized(lock) {
                    val pending = pendingDirect[id]
                    if (pending != null && linkKid in pending.kids) pendingDirect.remove(id) else null
                }
                entry?.timer?.cancel()
                if (synchronized(lock) { receipts.containsKey(id) }) {
                    track(record.id, id, "", ChatReceipt.DELIVERED)
                    notifyListeners()
                }
            }
            DirectMessageFrames.FOCUS -> notifyListeners()
        }
    }

    private fun openDirectFrame(
        record: DirectConversation,
        seal: RoomSeal,
        frame: Map<String, Any?>,
        kid: String,
        meta: Map<String, Any?>,
        ackVia: String? = null,
    ) {
        val result = seal.open(frame, now(), history = ackVia != null || meta["replay"] == true, meta = meta)
        if (result is SealOpen.NoSession) {
            if (result.kid == kid) recover(record, seal, kid, frame, null, ackVia)
            return
        }
        if (result !is SealOpen.Opened) {
            if (result is SealOpen.Held && !result.blocked) notifyListeners()
            return
        }
        val plain = result.plain
        if (result.senderKid != kid || !senderMatches(result, plain)) return
        if (DirectMessageFrames.isReset(plain)) {
            resealTo(record, seal, kid)
            return
        }
        if (jsonText(plain["channel"]) != record.conversation) return
        if (!result.replayed) handle(plain, record, meta["replay"] != true, jsonText(meta["ref"]))
        val id = jsonText(plain["id"])
        val links = mesh
        if (ackVia != null && links != null && id.isNotEmpty() && !DirectLiveFrames.isLiveFrame(plain)) {
            runCatching { links.sendDirect(ackVia, DirectMessageFrames.directAck(record.conversation, id)) }
        }
    }

    private fun keepMeshOnly(record: DirectConversation, plain: Map<String, Any?>, reached: Int): Boolean {
        if (reached <= 0) return false
        return applyLocally(record, plain)
    }

    private fun applyLocally(record: DirectConversation, plain: Map<String, Any?>): Boolean {
        val type = jsonText(plain["type"])
        if (type == "delete") remove(record, jsonText(plain["target"]))
        onFrame?.invoke(plain, record.conversation)
        if (type == "chat") DirectMessageFrames.chatMessage(plain, record, selfId())?.let {
            trackViewOnce(plain, it)
            append(it, record.id)
        }
        if (type == "chat" || type == "thread-reply") track(record.id, jsonText(plain["id"]), "", ChatReceipt.SENT)
        notifyListeners()
        return true
    }

    fun send(conversation: String, text: String): Boolean {
        val body = text.trim()
        val record = record(conversation) ?: return false
        if (body.isEmpty()) return false
        val plain = frame(record, "chat", linkedMapOf("text" to body.take(ChatLimits.MAX_TEXT_CHARS)))
        return sendSealed(record, plain)
    }

    fun sendFile(conversation: String, fileName: String, mime: String, data: ByteArray): Boolean {
        val name = fileName.trim().take(ChatLimits.MAX_FILE_NAME_CHARS)
        val record = record(conversation) ?: return false
        if (name.isEmpty() || data.isEmpty() || data.size > DirectMessageFrames.MAX_FILE_BYTES) return false
        val plain = frame(
            record,
            "chat",
            linkedMapOf(
                "fileName" to name,
                "fileMime" to mime.take(ChatLimits.MAX_MIME_CHARS),
                "file" to Base64.getEncoder().encodeToString(data),
            ),
        )
        return sendSealed(record, plain)
    }

    private fun sendSealed(record: DirectConversation, plain: Map<String, Any?>, notify: Boolean = true, direct: Boolean = true): Boolean {
        val copies = if (direct) sendDirectCopies(record, plain) else DirectSend(emptySet(), false)
        if (copies.directOnly) {
            awaitAck(record, plain, notify, copies.kids)
            return applyLocally(record, plain)
        }
        val reached = copies.kids.size
        if (synchronized(lock) { sockets[record.id]?.socket == null } && !open(record.conversation)) return keepMeshOnly(record, plain, reached)
        val holder = synchronized(lock) { sockets[record.id] } ?: return keepMeshOnly(record, plain, reached)
        val socket = holder.socket ?: return keepMeshOnly(record, plain, reached)
        val id = jsonText(plain["id"])
        val type = jsonText(plain["type"])
        val content = type == "chat" || type == "thread-reply"
        return try {
            val sealed = holder.seal.seal(plain, now())
            if (holder.seal.needsAdvertisement()) advertise(holder, persist = true)
            val envelope = LinkedHashMap(holder.crypto.encrypt(sealed))
            envelope["persist"] = true
            if (notify) envelope["notify"] = true
            val frameText = Json.stringify(envelope)
            val ref = OfficeFrames.sentRef(frameText)
            if (content) track(record.id, id, ref, ChatReceipt.SENDING)
            if (!socket.sendText(frameText)) {
                synchronized(lock) {
                    if (reached == 0) {
                        receipts.remove(id)
                        receiptTimes.remove(id)
                    }
                }
                return keepMeshOnly(record, plain, reached)
            }
            remember(holder, OutboxEntry(ref, plain, RoomSeal.wrappedKeyIds(sealed), notify, now()))
            if (type == "delete") remove(record, jsonText(plain["target"]))
            onFrame?.invoke(plain, record.conversation)
            if (type == "chat") DirectMessageFrames.chatMessage(plain, record, selfId())?.let {
                trackViewOnce(plain, it)
                append(it, record.id)
            }
            notifyListeners()
            true
        } catch (error: Exception) {
            lastError = error.message ?: error.toString()
            notifyListeners()
            keepMeshOnly(record, plain, reached)
        }
    }

    fun sendThreadReply(conversation: String, rootId: String, text: String): Boolean {
        val body = text.trim().take(ChatLimits.MAX_TEXT_CHARS)
        val root = rootId.trim().take(ChatLimits.MAX_ROOT_ID_CHARS)
        val record = record(conversation) ?: return false
        if (body.isEmpty() || root.isEmpty()) return false
        return sendSealed(record, frame(record, "thread-reply", linkedMapOf("rootId" to root, "text" to body)))
    }

    fun react(conversation: String, target: String, emoji: String, added: Boolean, sticker: String = ""): Boolean {
        val record = record(conversation) ?: return false
        if (target.isEmpty() || emoji.isEmpty()) return false
        val extra = linkedMapOf<String, Any?>(
            "conversation" to record.conversation,
            "target" to target,
            "emoji" to emoji,
            "reactorId" to selfId(),
            "reactorName" to displayName().take(ChatLimits.MAX_DISPLAY_NAME_CHARS),
            "added" to added,
        )
        if (sticker.isNotEmpty()) extra["sticker"] = sticker
        return sendSealed(record, frame(record, "reaction", extra), notify = false)
    }

    fun edit(conversation: String, target: String, text: String): Boolean {
        val body = text.trim().take(ChatLimits.MAX_TEXT_CHARS)
        val record = record(conversation) ?: return false
        if (body.isEmpty() || target.isEmpty()) return false
        val me = selfId()
        if (me.isEmpty() || authorOf(record, target) != me) return false
        val extra = linkedMapOf<String, Any?>("conversation" to record.conversation, "target" to target, "text" to body, "editedAt" to now())
        return sendSealed(record, frame(record, "edit", extra), notify = false)
    }

    fun deleteMessage(message: ChatMessage): Boolean {
        val record = record(message.conversation) ?: return false
        val me = selfId()
        if (me.isEmpty() || message.senderId != me || authorOf(record, message.id) != me) return false
        val ref = synchronized(lock) { idRefs[message.id] }
        val plain = frame(record, "delete", linkedMapOf("conversation" to record.conversation, "target" to message.id))
        if (!sendSealed(record, plain, notify = false)) return false
        val holder = synchronized(lock) { sockets[record.id] }
        if (ref != null && holder != null) for (frame in DirectMessageFrames.supersededFrames(listOf(ref))) sendOrQueue(holder, frame)
        return true
    }

    private fun trackViewOnce(plain: Map<String, Any?>, message: ChatMessage) {
        val parsed = ViewOnceCodec.parse(plain[ViewOnceCodec.FIELD]) ?: return
        viewOnce.accept(message.id, message.conversation, parsed, message.self, message.timestamp)
    }

    private fun holdReceipt(plain: Map<String, Any?>, conversationId: String, ref: String): Boolean {
        if (jsonText(plain["type"]) != "chat") return false
        val me = selfId()
        if (me.isNotEmpty() && jsonText(plain["senderId"]) == me) return false
        val parsed = ViewOnceCodec.parse(plain[ViewOnceCodec.FIELD]) ?: return false
        val entry = viewOnce.byMedia(parsed.media) ?: return false
        if (entry.self || (entry.state.terminal && !viewOnce.consumeDue(parsed.media))) return false
        synchronized(lock) {
            val held = heldReceipts.getOrPut(entry.messageId) { conversationId to LinkedHashSet() }
            held.second.add(ref)
            while (held.second.size > DirectMessageFrames.RECEIVED_REFS_PER_FRAME) held.second.remove(held.second.first())
            while (heldReceipts.size > DirectMessageFrames.MAX_MESSAGES_PER_ROOM * 4) heldReceipts.remove(heldReceipts.keys.first())
        }
        return true
    }

    private fun releaseReceipt(messageId: String) {
        val (holder, refs) = synchronized(lock) {
            val held = heldReceipts[messageId] ?: return
            val socket = sockets[held.first] ?: return
            heldReceipts.remove(messageId)
            socket to held.second.toList()
        }
        sendReceived(holder, refs)
    }

    fun heldReceiptCount(): Int = synchronized(lock) { heldReceipts.size }

    private fun applyViewOnceState(holder: RoomSocket?, mediaId: String, state: String) {
        val updated = viewOnce.update(mediaId, state) ?: return
        if (updated.state.terminal) {
            forgetViewOnceFrame(holder, mediaId)
            releaseReceipt(updated.messageId)
        }
        notifyListeners()
    }

    private fun forgetViewOnceFrame(holder: RoomSocket?, mediaId: String) {
        synchronized(lock) {
            val holders = if (holder != null) listOf(holder) else sockets.values.toList()
            for (each in holders) {
                each.outbox.removeAll { ViewOnceCodec.parse(it.plain[ViewOnceCodec.FIELD])?.media == mediaId }
            }
        }
    }

    fun sendViewOnce(
        conversation: String,
        kind: ViewOnceKind,
        mime: String,
        durationMs: Long,
        data: ByteArray,
        onProgress: (Int, Int) -> Unit = { _, _ -> },
    ): Boolean {
        val record = record(conversation) ?: throw ViewOnceException("Open the conversation first.")
        val type = ViewOnceCodec.normalizeMime(mime)
        if (type !in kind.mimes) throw ViewOnceException("This recording format cannot be sent.")
        if (data.isEmpty()) throw ViewOnceException("The recording is empty.")
        if (data.size.toLong() + ViewOnceCodec.TAG_BYTES > kind.maxBytes) throw ViewOnceException(ViewOnceFrames.errorMessage("media_too_large"))
        val duration = durationMs.coerceIn(0, kind.maxDurationMs)
        val key = ViewOnceCrypto.newKey()
        val iv = ViewOnceCrypto.newIv()
        val sealed = ViewOnceCrypto.encrypt(data, key, iv)
        val mediaId = try {
            media.upload(record.id, kind, sealed, duration, onProgress)
        } catch (error: OrgApiException) {
            throw ViewOnceException(ViewOnceFrames.errorMessage(error.code))
        }
        val field = ViewOnceCodec.field(
            ViewOnceMedia(kind, mediaId, key, iv, type, duration, sealed.size.toLong(), ViewOnceCodec.partsFor(sealed.size.toLong())),
        )
        key.fill(0)
        val plain = frame(record, "chat", linkedMapOf("text" to kind.text, ViewOnceCodec.FIELD to field))
        val sent = sendSealed(record, plain)
        if (!sent) {
            runCatching { media.withdraw(record.id, mediaId) }
            viewOnce.byMedia(mediaId)?.let { viewOnce.mark(it.messageId, ViewOnceState.WITHDRAWN) }
            forgetViewOnceFrame(null, mediaId)
            notifyListeners()
        }
        return sent
    }

    fun downloadViewOnce(messageId: String): ByteArray {
        val entry = viewOnce.entry(messageId) ?: throw ViewOnceException("This message is no longer available.")
        val conversationId = DirectMessageFrames.conversationId(entry.conversation)
        if (!entry.playable || conversationId.isEmpty()) throw ViewOnceGone(entry.state.wire)
        val (key, iv) = viewOnce.keyFor(messageId) ?: throw ViewOnceGone(entry.state.wire)
        try {
            val sealed = media.download(conversationId, entry.mediaId, entry.parts, entry.size)
            return ViewOnceCrypto.decrypt(sealed, key, iv) ?: throw ViewOnceException("The message could not be decrypted.")
        } catch (gone: ViewOnceGone) {
            applyViewOnceState(null, entry.mediaId, gone.state)
            throw gone
        } catch (error: OrgApiException) {
            throw ViewOnceException(ViewOnceFrames.errorMessage(error.code))
        } finally {
            key.fill(0)
            iv.fill(0)
        }
    }

    fun consumeViewOnce(messageId: String): Boolean {
        val entry = viewOnce.entry(messageId) ?: return false
        val conversationId = DirectMessageFrames.conversationId(entry.conversation)
        if (entry.self || entry.state != ViewOnceState.READY || conversationId.isEmpty()) return false
        viewOnce.mark(messageId, ViewOnceState.OPENED)
        viewOnce.needsConsume(entry.mediaId, true)
        notifyListeners()
        val done = try {
            media.consume(conversationId, entry.mediaId)
            true
        } catch (gone: ViewOnceGone) {
            true
        } catch (_: Exception) {
            false
        }
        if (done) {
            viewOnce.needsConsume(entry.mediaId, false)
            releaseReceipt(messageId)
        }
        return done
    }

    fun withdrawViewOnce(messageId: String): Boolean {
        val entry = viewOnce.entry(messageId) ?: return false
        val conversationId = DirectMessageFrames.conversationId(entry.conversation)
        if (!entry.self || entry.state.terminal || conversationId.isEmpty()) return false
        val state = try {
            media.withdraw(conversationId, entry.mediaId)
        } catch (error: OrgApiException) {
            throw ViewOnceException(ViewOnceFrames.errorMessage(error.code))
        }
        applyViewOnceState(null, entry.mediaId, state)
        return true
    }

    fun refreshViewOnce(conversation: String) {
        val conversationId = DirectMessageFrames.conversationId(conversation)
        if (conversationId.isEmpty()) return
        val ids = viewOnce.pending(conversation)
        if (ids.isEmpty()) return
        val states = runCatching { media.status(conversationId, ids) }.getOrNull() ?: return
        for ((mediaId, state) in states) {
            if (viewOnce.consumeDue(mediaId)) {
                if (state == "ready") {
                    viewOnce.byMedia(mediaId)?.let { entry ->
                        if (runCatching { media.consume(conversationId, mediaId) }.isSuccess) {
                            viewOnce.needsConsume(mediaId, false)
                            releaseReceipt(entry.messageId)
                        }
                        if (entry.state != ViewOnceState.OPENED) viewOnce.mark(entry.messageId, ViewOnceState.OPENED)
                    }
                    continue
                }
                viewOnce.needsConsume(mediaId, false)
                viewOnce.byMedia(mediaId)?.let { releaseReceipt(it.messageId) }
            }
            applyViewOnceState(null, mediaId, state)
        }
    }

    fun sendCallFrame(conversation: String, type: String, extra: Map<String, Any?>): Boolean {
        if (!type.startsWith("call-")) return false
        val record = record(conversation) ?: return false
        val holder = synchronized(lock) { sockets[record.id] } ?: return false
        val socket = holder.socket ?: return false
        val plain = frame(record, type, extra)
        return try {
            val envelope = LinkedHashMap(holder.crypto.encrypt(holder.seal.seal(plain)))
            if (type == DirectCallEngine.CALL_INVITE_TYPE) {
                envelope["ring"] = true
                extra["call"]?.let { envelope["call"] = it }
            }
            socket.sendText(Json.stringify(envelope))
        } catch (_: Exception) {
            false
        }
    }

    fun liveInterval(conversation: String): Long {
        val budget = synchronized(lock) { liveBudgets[DirectMessageFrames.conversationId(conversation)] }
            ?: return DirectLiveFrames.FALLBACK_INTERVAL_MS
        return DirectLiveFrames.interval(budget.perWindow, budget.windowMs)
    }

    fun sendLive(conversation: String, fields: Map<String, Any?>): Boolean {
        val record = record(conversation) ?: return false
        val plain = frame(record, DirectLiveFrames.TYPE, fields)
        val copies = sendDirectCopies(record, plain)
        val reached = copies.kids.isNotEmpty()
        if (copies.directOnly && reached) return true
        val holder = synchronized(lock) { sockets[record.id] } ?: return reached
        val socket = holder.socket ?: return reached
        return try {
            val sealed = holder.seal.seal(plain, now())
            if (holder.seal.needsAdvertisement()) advertise(holder, persist = true)
            val envelope = LinkedHashMap(holder.crypto.encrypt(sealed))
            envelope["live"] = true
            val text = Json.stringify(envelope)
            if (text.toByteArray(Charsets.UTF_8).size > DirectLiveFrames.MAX_ENVELOPE_BYTES) return reached
            socket.sendText(text) || reached
        } catch (_: Exception) {
            reached
        }
    }

    fun makeLiveEngine(): DirectLiveEngine {
        val engine = DirectLiveEngine(
            selfId = selfId,
            send = { conversation, fields -> sendLive(conversation, fields) },
            interval = { conversation -> liveInterval(conversation) },
            clock = now,
        )
        onLiveFrame = { plain, conversation -> engine.receive(plain, conversation) }
        return engine
    }

    private fun advertise(holder: RoomSocket, persist: Boolean = false) {
        val envelope = runCatching { LinkedHashMap(holder.crypto.encrypt(holder.seal.keyFrame())) }.getOrNull() ?: return
        if (persist) envelope["persist"] = true
        sendOrQueue(holder, Json.stringify(envelope))
    }

    private fun dropped(holder: RoomSocket) {
        val removed = synchronized(lock) {
            holder.dead = true
            if (sockets[holder.conversationId] === holder) {
                sockets.remove(holder.conversationId)
                true
            } else {
                false
            }
        }
        if (!removed) return
        notifyListeners()
        scheduleReconnect(holder.conversationId)
    }

    fun closeAll() {
        val all = synchronized(lock) {
            epoch++
            for (timer in timers.values) timer.cancel()
            timers.clear()
            retries.clear()
            val list = sockets.values.toList()
            sockets.clear()
            list
        }
        for (holder in all) holder.socket?.close()
        notifyListeners()
    }

    private fun call(method: String, path: String, body: Map<String, Any?>?): Map<String, Any?> = try {
        api.accountApiCall(method, path, body)
    } catch (error: OrgApiException) {
        throw DirectMessageException(error.code)
    }

    companion object {
        private val sharedLoop: EventLoop by lazy { EventLoop("direct-messages") }
    }
}
