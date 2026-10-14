package com.forkmesh.nativeapp.chat

import com.forkmesh.nativeapp.crypto.RoomCrypto
import com.forkmesh.nativeapp.crypto.RoomSeal
import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.json.jsonLong
import com.forkmesh.nativeapp.json.jsonMaps
import com.forkmesh.nativeapp.json.jsonText
import com.forkmesh.nativeapp.model.ChatMessage
import com.forkmesh.nativeapp.model.OrgApiException
import com.forkmesh.nativeapp.security.SecretStore
import java.net.URI
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
    const val RECEIVED_REFS_PER_FRAME = 64
    const val OUTBOX_MAX = 64
    const val OUTBOX_MAX_AGE_MS = 7L * 24 * 60 * 60 * 1000
    const val RECONNECT_MAX_SECONDS = 300
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
) {
    private class OutboxEntry(
        var ref: String,
        val plain: Map<String, Any?>,
        var kids: List<String>,
        val notify: Boolean,
        val ts: Long,
    )

    private class RoomSocket(val conversationId: String, val crypto: RoomCrypto, val seal: RoomSeal) {
        var socket: OfficeSocket? = null
        var dead = false
        var historyEnded = false
        var resealAsked = false
        val replayRefs = ArrayList<String>()
        val queued = ArrayList<String>()
        val outbox = ArrayList<OutboxEntry>()
    }

    private val lock = Any()
    private val sockets = HashMap<String, RoomSocket>()
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
    private val media = ViewOnceClient(api)
    private val heldReceipts = LinkedHashMap<String, Pair<String, LinkedHashSet<String>>>()

    val viewOnce = ViewOnceStore()

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
    var meshSender: ((Map<String, Any?>, List<String>) -> Int)? = null

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

    fun serverCopies(conversation: String): Int? =
        synchronized(lock) { serverCopies[DirectMessageFrames.conversationId(conversation)] }

    fun isConnected(conversation: String): Boolean =
        synchronized(lock) { sockets[DirectMessageFrames.conversationId(conversation)]?.socket != null }

    fun refresh() {
        loading = true
        notifyListeners()
        try {
            val payload = call("GET", "${DirectMessageFrames.ENDPOINT}?limit=${DirectMessageFrames.PAGE_LIMIT}", null)
            apply(DirectMessageFrames.conversations(payload), replacing = true)
            nextCursor = jsonText(payload["nextCursor"])
            lastError = null
        } catch (error: Exception) {
            lastError = error.message ?: error.toString()
        } finally {
            loading = false
            notifyListeners()
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
                history.remove(old.conversation)
                viewOnce.forgetConversation(old.conversation)
            }
            records = merged
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

    private fun seal(room: String): RoomSeal {
        synchronized(lock) { seals[room] }?.let { return it }
        val scope = origin()?.let { runCatching { URI(it).host }.getOrNull() } ?: ""
        val loaded = RoomSeal.loadOrCreate(room, storageScope = "direct|$scope", secureStore = secureStore)
        synchronized(lock) { seals[room] = loaded }
        return loaded
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
            val seal = seal(roomName)
            synchronized(lock) { roomNames[record.id] = roomName }
            val holder = RoomSocket(record.id, crypto, seal)
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
        val framed = holder.crypto.decrypt(envelope) ?: return
        val replay = envelope["historyReplay"] == true
        if (RoomSeal.isKeyFrame(framed)) {
            if (holder.seal.acceptKeyFrame(framed)) {
                if (!replay) advertise(holder)
                peerLearned(holder)
            }
            return
        }
        if (!RoomSeal.isSealedFrame(framed)) return
        val kids = RoomSeal.wrappedKeyIds(framed)
        if (holder.seal.keyId !in kids) {
            askForReseal(holder)
            return
        }
        val known = holder.seal.peerKeyIds
        val plain = holder.seal.unseal(framed) ?: return
        val learned = holder.seal.peerKeyIds.any { it !in known }
        val ref = DirectMessageFrames.relayRef(envelope)
        handle(plain, record, if (replay) null else holder)
        if (replay) noteOwn(holder, plain, ref, kids, envelope["notify"] == true)
        if (ref.isNotEmpty() && holdReceipt(plain, holder.conversationId, ref)) {
            if (learned) peerLearned(holder)
            return
        }
        if (ref.isNotEmpty()) {
            if (replay) {
                synchronized(lock) { if (ref !in holder.replayRefs) holder.replayRefs.add(ref) }
            } else {
                sendReceived(holder, listOf(ref))
            }
        }
        if (learned) peerLearned(holder)
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

    private fun handle(plain: Map<String, Any?>, record: DirectConversation, live: RoomSocket?) {
        if (DirectMessageFrames.isCallFrame(plain)) {
            if (live != null) onCallFrame?.invoke(plain, record.conversation)
            return
        }
        onFrame?.invoke(plain, record.conversation)
        val message = DirectMessageFrames.chatMessage(plain, record, selfId()) ?: return
        trackViewOnce(plain, message)
        val added = append(message, record.id)
        notifyListeners()
        if (added && live != null && !message.self) onMessage?.invoke(message)
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
        val seal = synchronized(lock) { roomNames[record.id]?.let { seals[it] } } ?: return emptyList()
        return seal.peerKeyIds
    }

    fun acceptMeshDirect(channel: String, sealKeyId: String, plain: Map<String, Any?>): Boolean {
        val record = record(channel) ?: return false
        if (sealKeyId.isEmpty() || jsonText(plain["channel"]) != channel || sealKeyId !in deviceKeys(channel)) return false
        val frame = LinkedHashMap(plain)
        frame["type"] = "chat"
        val message = DirectMessageFrames.chatMessage(frame, record, selfId()) ?: return false
        trackViewOnce(frame, message)
        if (!append(message, record.id)) return false
        notifyListeners()
        if (!message.self) onMessage?.invoke(message)
        return true
    }

    private fun sendOverMesh(record: DirectConversation, plain: Map<String, Any?>): Int {
        val sender = meshSender ?: return 0
        val keys = deviceKeys(record.conversation)
        if (keys.isEmpty()) return 0
        return runCatching { sender(plain, keys) }.getOrDefault(0)
    }

    private fun keepMeshOnly(record: DirectConversation, plain: Map<String, Any?>, reached: Int): Boolean {
        if (reached <= 0) return false
        DirectMessageFrames.chatMessage(plain, record, selfId())?.let {
            trackViewOnce(plain, it)
            append(it, record.id)
        }
        notifyListeners()
        return true
    }

    fun send(conversation: String, text: String): Boolean {
        val body = text.trim()
        val record = record(conversation) ?: return false
        if (body.isEmpty()) return false
        val plain = frame(record, "chat", linkedMapOf("text" to body.take(ChatLimits.MAX_TEXT_CHARS)))
        return sendSealed(record, plain, sendOverMesh(record, plain))
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
        return sendSealed(record, plain, 0)
    }

    private fun sendSealed(record: DirectConversation, plain: Map<String, Any?>, reached: Int): Boolean {
        if (synchronized(lock) { sockets[record.id]?.socket == null } && !open(record.conversation)) return keepMeshOnly(record, plain, reached)
        val holder = synchronized(lock) { sockets[record.id] } ?: return keepMeshOnly(record, plain, reached)
        val socket = holder.socket ?: return keepMeshOnly(record, plain, reached)
        return try {
            val sealed = holder.seal.seal(plain)
            val envelope = LinkedHashMap(holder.crypto.encrypt(sealed))
            envelope["persist"] = true
            envelope["notify"] = true
            val frameText = Json.stringify(envelope)
            if (!socket.sendText(frameText)) return keepMeshOnly(record, plain, reached)
            remember(holder, OutboxEntry(OfficeFrames.sentRef(frameText), plain, RoomSeal.wrappedKeyIds(sealed), true, now()))
            onFrame?.invoke(plain, record.conversation)
            DirectMessageFrames.chatMessage(plain, record, selfId())?.let {
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
        val sent = sendSealed(record, plain, 0)
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
            socket.sendText(Json.stringify(holder.crypto.encrypt(holder.seal.seal(plain))))
        } catch (_: Exception) {
            false
        }
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
