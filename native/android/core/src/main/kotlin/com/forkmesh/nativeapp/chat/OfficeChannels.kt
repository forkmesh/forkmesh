package com.forkmesh.nativeapp.chat

import com.forkmesh.nativeapp.apps.AppBlocks
import com.forkmesh.nativeapp.crypto.ChatNodeKey
import com.forkmesh.nativeapp.crypto.ChatV3
import com.forkmesh.nativeapp.crypto.RoomCrypto
import com.forkmesh.nativeapp.crypto.RoomSeal
import com.forkmesh.nativeapp.crypto.SealOpen
import com.forkmesh.nativeapp.crypto.SealPolicy
import com.forkmesh.nativeapp.crypto.Sha256
import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.json.jsonLong
import com.forkmesh.nativeapp.json.jsonMaps
import com.forkmesh.nativeapp.json.jsonText
import com.forkmesh.nativeapp.model.ChatMessage
import com.forkmesh.nativeapp.model.ChatReceipt
import com.forkmesh.nativeapp.model.ChatRetention
import com.forkmesh.nativeapp.model.OrgApiException
import com.forkmesh.nativeapp.net.WebSocketClient
import com.forkmesh.nativeapp.security.SecretStore
import java.net.URI
import java.util.UUID
import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.atomic.AtomicInteger

fun interface ChatAccountCalling {
    fun accountApiCall(method: String, path: String, body: Map<String, Any?>?): Map<String, Any?>
}

data class OfficeRoom(
    val id: String,
    val name: String,
    val isPrivate: Boolean,
    val canManage: Boolean,
    val topic: String,
    val members: List<String>,
    val keyVersion: Int,
    val serverVisible: Boolean = false,
    val readOnly: Boolean = false,
) {
    val conversation: String get() = OfficeFrames.conversation(name)
}

object OfficeFrames {
    const val PREFIX = "#office/"
    const val MAX_ROOMS = 100
    const val MAX_LIVE_ROOMS = 8
    const val MAX_TOPIC_CHARS = 200
    const val MAX_MESSAGES_PER_ROOM = 200
    const val MAX_PENDING_REFS = 200
    const val RECIPIENTS_KIND = "forkmesh-recipients"

    fun conversation(name: String): String {
        val trimmed = name.trim().take(ChatLimits.MAX_DISPLAY_NAME_CHARS)
        return if (trimmed.isEmpty()) "" else PREFIX + trimmed
    }

    fun isOffice(conversation: String): Boolean = conversation.startsWith(PREFIX)

    fun validChannelId(id: String): Boolean = id.length == 32 && id.all { it in '0'..'9' || it in 'a'..'f' }

    fun validName(name: String): Boolean {
        if (name.length !in 1..40) return false
        val ok: (Char) -> Boolean = { it in 'a'..'z' || it in '0'..'9' }
        if (!ok(name.first()) || !ok(name.last())) return false
        return name.all { ok(it) || it == '-' }
    }

    private fun collapse(text: String): String = text.split(Regex("\\s+")).filter { it.isNotEmpty() }.joinToString(" ")

    fun rooms(payload: Map<String, Any?>): List<OfficeRoom> {
        val out = ArrayList<OfficeRoom>()
        val seen = HashSet<String>()
        for (channel in jsonMaps(payload["channels"])) {
            if (out.size >= MAX_ROOMS) break
            val id = jsonText(channel["id"])
            if (!validChannelId(id)) continue
            val name = jsonText(channel["name"]).take(ChatLimits.MAX_DISPLAY_NAME_CHARS)
            val conversation = conversation(name)
            if (conversation.isEmpty() || !seen.add(conversation)) continue
            val isPrivate = jsonText(channel["visibility"]) == "private"
            val members = ArrayList<String>()
            if (isPrivate) {
                for (raw in (channel["members"] as? List<*>).orEmpty().map(::jsonText)) {
                    val member = raw.trim().lowercase().take(ChatLimits.MAX_DISPLAY_NAME_CHARS)
                    if (member.isNotEmpty() && member !in members) members.add(member)
                }
            }
            out.add(
                OfficeRoom(
                    id = id,
                    name = name,
                    isPrivate = isPrivate,
                    canManage = channel["canManage"] == true,
                    topic = collapse(jsonText(channel["topic"])).take(MAX_TOPIC_CHARS),
                    members = members,
                    keyVersion = maxOf(1L, jsonLong(channel["keyVersion"], 1)).toInt(),
                    serverVisible = channel["serverVisible"] == true,
                    readOnly = channel["readOnly"] == true,
                ),
            )
        }
        return out
    }

    fun recipients(envelope: Map<String, Any?>): Int? {
        val raw = envelope["recipients"]
        val count = when (raw) {
            is Long -> raw
            is Int -> raw.toLong()
            else -> return null
        }
        return if (count in 0..Int.MAX_VALUE.toLong()) count.toInt() else null
    }

    fun sentRef(text: String): String = Sha256.hex(text.toByteArray(Charsets.UTF_8)).take(40)

    fun recipientsAck(envelope: Map<String, Any?>): Pair<String, Int>? {
        if (jsonText(envelope["kind"]) != RECIPIENTS_KIND) return null
        val count = recipients(envelope) ?: return null
        val ref = jsonText(envelope["ref"])
        if (ref.length != 40) return null
        return ref to count
    }

    fun recipientsLabel(count: Int?): String? = if (count == null || count < 0) null else "to $count"

    fun recipientsDescription(count: Int?): String? = when {
        count == null || count < 0 -> null
        count == 1 -> "Delivered to 1 person"
        else -> "Delivered to $count people"
    }

    fun chatMessage(
        plain: Map<String, Any?>,
        conversation: String,
        selfId: String,
        recipients: Int? = null,
        botAccount: (String) -> String = { "" },
    ): ChatMessage? {
        if (conversation.isEmpty() || jsonText(plain["type"]) != "chat") return null
        val id = jsonText(plain["id"])
        if (id.isEmpty() || id.length > 64) return null
        val rawText = jsonText(plain["text"])
        if (rawText.length > ChatLimits.MAX_TEXT_CHARS) return null
        val senderId = RelayFrames.shownSender(plain).take(64)
        val sender = RelayFrames.shownName(plain, botAccount)
            .let { if (RelayFrames.botName(plain).isEmpty()) it.take(ChatLimits.MAX_DISPLAY_NAME_CHARS) else it }
            .ifEmpty { "Office visitor" }
        val fileName = jsonText(plain["fileName"]).take(ChatLimits.MAX_FILE_NAME_CHARS)
        val blocks = if (senderId.startsWith("app:")) AppBlocks.parse(plain["blocks"]) else emptyList()
        if (rawText.isEmpty() && fileName.isEmpty() && blocks.isEmpty()) return null
        val ts = plain["ts"]
        return ChatMessage(
            id = id,
            conversation = conversation,
            senderId = senderId,
            senderName = sender,
            text = rawText,
            timestamp = if (ts is Number) ts.toLong() else 0L,
            self = selfId.isNotEmpty() && senderId == selfId,
            fileName = fileName,
            recipients = recipients,
            blocks = blocks,
            appMessageId = if (blocks.isEmpty()) "" else jsonText(plain["appMessageId"]).take(64),
            serverVisible = RelayFrames.serverVisible(plain),
        )
    }

    fun webSocketUrl(path: String, origin: String): String? {
        if (path.isEmpty()) return null
        val base = try {
            URI(origin)
        } catch (_: Exception) {
            return null
        }
        val resolved = try {
            base.resolve(path)
        } catch (_: Exception) {
            return null
        }
        val scheme = when (resolved.scheme?.lowercase()) {
            "https", "wss" -> "wss"
            "http", "ws" -> "ws"
            else -> return null
        }
        if (resolved.host != base.host) return null
        val port = if (resolved.port >= 0) ":${resolved.port}" else ""
        val query = resolved.rawQuery?.let { "?$it" } ?: ""
        return "$scheme://${resolved.host}$port${resolved.rawPath}$query"
    }

    fun errorMessage(code: String): String = when (code) {
        "invalid_channel_name" -> "Use 1 to 40 lowercase letters, digits or dashes."
        "channel_name_taken" -> "A channel with that name already exists."
        "too_many_channels" -> "This project already has the most channels it can hold."
        "manager_required" -> "Only the channel's creator or an administrator can change it."
        "user_not_found" -> "One of those people has no account here."
        "members_not_allowed" -> "Public channels have no member list."
        "not_found" -> "That channel is gone."
        "" -> "The server refused the change."
        else -> code.replace('_', ' ')
    }
}

class OfficeException(val code: String) : RuntimeException(if (code == "unavailable") "The office room could not be reached." else OfficeFrames.errorMessage(code))

interface OfficeSocket {
    fun sendText(text: String): Boolean

    fun close()
}

interface OfficeSocketConnector {
    fun connect(url: String, onText: (String) -> Unit, onClosed: () -> Unit): OfficeSocket
}

object WebSocketOfficeConnector : OfficeSocketConnector {
    override fun connect(url: String, onText: (String) -> Unit, onClosed: () -> Unit): OfficeSocket {
        val options = WebSocketClient.Options().apply {
            connectTimeoutMs = RelayRoom.CONNECT_TIMEOUT_MS
            handshakeTimeoutMs = RelayRoom.CONNECT_TIMEOUT_MS
        }
        val client = WebSocketClient.connect(
            url,
            object : WebSocketClient.Listener {
                override fun onText(socket: WebSocketClient, text: String) = onText(text)

                override fun onBinary(socket: WebSocketClient, data: ByteArray) = onText(String(data, Charsets.UTF_8))

                override fun onClosed(socket: WebSocketClient, code: Int, reason: String, error: Throwable?) = onClosed()
            },
            options,
        )
        return object : OfficeSocket {
            override fun sendText(text: String): Boolean = client.sendText(text)

            override fun close() {
                try {
                    client.close(WebSocketClient.NORMAL_CLOSURE)
                } catch (_: Exception) {
                    client.cancel()
                }
            }
        }
    }
}

class OfficeChannelService(
    private val api: ChatAccountCalling,
    private val connector: OfficeSocketConnector,
    private val secureStore: SecretStore,
    private val origin: () -> String?,
    private val selfId: () -> String,
    private val displayName: () -> String,
    private val now: () -> Long = System::currentTimeMillis,
    private val platform: String = "android",
    private val account: () -> String = { "" },
    private val nodeKey: () -> ChatNodeKey? = { null },
    private val owners: () -> NodeOwners? = { null },
) {
    private class RoomSocket(val roomId: String, val crypto: RoomCrypto, val seal: RoomSeal, var socket: OfficeSocket? = null)

    private val lock = Any()
    private val sockets = HashMap<String, RoomSocket>()
    private val cryptos = HashMap<String, RoomCrypto>()
    private val seals = HashMap<String, RoomSeal>()
    private val lastTs = HashMap<String, Long>()
    private val seen = HashMap<String, HashSet<String>>()
    private val history = HashMap<String, List<ChatMessage>>()
    private val pendingRefs = LinkedHashMap<String, String>()
    private val ackedCounts = HashMap<String, Int>()
    private val sendingIds = HashSet<String>()
    private val sequence = AtomicInteger()
    private val listeners = CopyOnWriteArrayList<() -> Unit>()

    @Volatile
    var rooms: List<OfficeRoom> = emptyList()
        private set

    @Volatile
    var lastError: String? = null
        private set

    @Volatile
    var loading: Boolean = false
        private set

    @Volatile
    var retention: ChatRetention? = null

    @Volatile
    var onMessage: ((ChatMessage) -> Unit)? = null

    @Volatile
    var onLiveFrame: ((Map<String, Any?>, String) -> Unit)? = null
    var onRoomOpened: ((String, String) -> Unit)? = null

    fun sendLiveFrame(conversation: String, type: String, fields: Map<String, Any?>): Boolean {
        if (type !in GroupCallFrames.LIVE_TYPES) return false
        val room = room(conversation) ?: return false
        if (!room.isPrivate || room.readOnly) return false
        if (synchronized(lock) { sockets[room.id]?.socket == null } && !open(conversation)) return false
        val holder = synchronized(lock) { sockets[room.id] } ?: return false
        val socket = holder.socket ?: return false
        val plain = frame(room, "", bot = false)
        plain.remove("text")
        plain["type"] = type
        for ((key, value) in fields) if (key !in GroupCallFrames.RESERVED_FIELDS) plain[key] = value
        return try {
            val framed = holder.seal.seal(plain, now())
            if (holder.seal.needsAdvertisement()) advertise(holder)
            socket.sendText(Json.stringify(holder.crypto.encrypt(framed)))
        } catch (_: Exception) {
            false
        }
    }

    fun callIdentity(conversation: String): GroupCallIdentity? {
        val room = room(conversation) ?: return null
        if (!room.isPrivate) return null
        val seal = synchronized(lock) { sockets[room.id]?.takeIf { it.socket != null }?.seal } ?: return null
        val acct = seal.acct.trim().lowercase()
        if (!ChatV3.validAcct(acct)) return null
        val keys = seal.keys
        return GroupCallIdentity(call = seal.room, acct = acct, kid = keys.kid, idk = keys.idk.copyOf(), signer = keys::sign)
    }

    fun callTicket(conversation: String, body: Map<String, Any?> = emptyMap()): Map<String, Any?>? {
        val room = room(conversation) ?: return null
        if (!room.isPrivate) return null
        return call("POST", "/api/chat/channels/${room.id}/call-ticket", body)
    }

    fun addListener(listener: () -> Unit) {
        listeners.add(listener)
    }

    fun removeListener(listener: () -> Unit) {
        listeners.remove(listener)
    }

    private fun notifyListeners() {
        for (listener in listeners) runCatching { listener() }
    }

    val conversations: List<String> get() = rooms.map { it.conversation }

    fun room(conversation: String): OfficeRoom? = rooms.firstOrNull { it.conversation == conversation }

    fun messages(conversation: String): List<ChatMessage> = synchronized(lock) { history[conversation].orEmpty() }

    fun receipt(messageId: String): ChatReceipt? = synchronized(lock) { if (messageId in sendingIds) ChatReceipt.SENDING else null }

    fun historySnapshot(): Map<String, List<ChatMessage>> = synchronized(lock) { HashMap(history) }

    fun restore(messages: List<ChatMessage>) {
        val changed = synchronized(lock) {
            val ids = HashMap<String, MutableSet<String>>()
            val added = LinkedHashMap<String, MutableList<ChatMessage>>()
            for (message in messages) {
                if (!OfficeFrames.isOffice(message.conversation)) continue
                val known = ids.getOrPut(message.conversation) { history[message.conversation].orEmpty().mapTo(HashSet()) { it.id } }
                if (!known.add(message.id)) continue
                added.getOrPut(message.conversation) { ArrayList() }.add(message)
            }
            for ((conversation, incoming) in added) {
                history[conversation] = mergeSorted(history[conversation].orEmpty(), incoming).takeLast(OfficeFrames.MAX_MESSAGES_PER_ROOM)
            }
            added.isNotEmpty()
        }
        if (changed) notifyListeners()
    }

    fun isConnected(conversation: String): Boolean {
        val room = room(conversation) ?: return false
        return synchronized(lock) { sockets[room.id]?.socket != null }
    }

    fun refresh() {
        loading = true
        notifyListeners()
        try {
            val next = OfficeFrames.rooms(api.accountApiCall("GET", "/api/chat/channels", null))
            val live = next.map { it.id }.toSet()
            val closing = synchronized(lock) {
                val gone = sockets.filterKeys { it !in live }.values.toList()
                for (socket in gone) sockets.remove(socket.roomId)
                for (old in rooms) if (old.id !in live) history.remove(old.conversation)
                for (room in next) {
                    val previous = rooms.firstOrNull { it.id == room.id } ?: continue
                    if (previous.conversation == room.conversation) continue
                    history[room.conversation] = history.remove(previous.conversation).orEmpty().map { it.copy(conversation = room.conversation) }
                }
                rooms = next
                gone
            }
            for (socket in closing) socket.socket?.close()
            lastError = null
        } catch (error: Exception) {
            lastError = error.message ?: error.toString()
        } finally {
            loading = false
            notifyListeners()
        }
    }

    private fun derive(room: String, passphrase: String): RoomCrypto {
        val key = "$room|$passphrase"
        synchronized(lock) { cryptos[key] }?.let { return it }
        val crypto = RoomCrypto.withPassphrase(room, passphrase)
        synchronized(lock) { cryptos[key] = crypto }
        return crypto
    }

    private fun seal(room: String): RoomSeal {
        synchronized(lock) { seals[room] }?.let {
            it.setIdentity(sealAccount(), selfId())
            return it
        }
        val loaded = RoomSeal.loadOrCreate(room, scope = room, policy = SealPolicy.OPEN, acct = sealAccount(), secureStore = secureStore, clock = now, node = selfId(), nodeKey = nodeKey())
        synchronized(lock) { seals[room] = loaded }
        return loaded
    }

    fun botAccount(node: String): String =
        if (node == selfId()) account().trim().lowercase() else owners()?.account(node).orEmpty()

    fun relabelBots() {
        val changed = synchronized(lock) {
            var any = false
            for ((key, list) in HashMap(history)) {
                val next = list.map { RelayFrames.relabel(it, ::botAccount) }
                if (next != list) {
                    history[key] = next
                    any = true
                }
            }
            any
        }
        if (changed) notifyListeners()
    }

    private fun sealAccount(): String = account().trim().lowercase().ifEmpty { selfId() }

    private fun watchClaim(plain: Map<String, Any?>) {
        val type = jsonText(plain["type"])
        if (type != "chat" && type != "thread-reply") return
        val node = jsonText(plain["senderId"])
        val claim = jsonText(plain["ownerUser"] ?: plain["owner"])
        if (node.isEmpty() || node == selfId() || claim.isBlank() || RelayFrames.isIntegration(plain)) return
        val known = owners() ?: return
        if (known.verified(node, claim).isEmpty()) known.watch(node, claim)
    }

    fun loadHistory(conversation: String) {
        val room = room(conversation) ?: return
        try {
            val since = synchronized(lock) { lastTs[room.id] ?: 0L }
            val payload = api.accountApiCall("GET", "/api/chat/channels/${room.id}/history?since=$since", null)
            val roomName = jsonText(payload["room"])
            val passphrase = jsonText(payload["passphrase"])
            if (roomName.isEmpty() || passphrase.isEmpty()) return
            val crypto = derive(roomName, passphrase)
            val seal = seal(roomName)
            for (entry in jsonMaps(payload["messages"])) {
                val ts = jsonLong(entry["ts"])
                synchronized(lock) { lastTs[room.id] = maxOf(lastTs[room.id] ?: 0L, ts) }
                val envelope = try {
                    Json.parseObject(jsonText(entry["body"]))
                } catch (_: Exception) {
                    continue
                }
                if (envelope.isEmpty()) continue
                val plain = crypto.decrypt(envelope) ?: continue
                accept(plain, room, seal, null, OfficeFrames.recipients(envelope), history = true, server = envelope["srv"] == true)
            }
            notifyListeners()
        } catch (error: Exception) {
            lastError = error.message ?: error.toString()
            notifyListeners()
        }
    }

    private fun accept(
        frame: Map<String, Any?>,
        room: OfficeRoom,
        seal: RoomSeal,
        live: RoomSocket?,
        recipients: Int? = null,
        history: Boolean = false,
        server: Boolean = false,
    ) {
        var plain = frame
        if (RoomSeal.isLegacyFrame(plain)) return
        if (RoomSeal.isKeyFrame(plain)) {
            val outcome = seal.acceptKeyFrame(plain, now()) ?: return
            if (outcome.isNew && live != null) advertise(live)
            for (held in seal.takeHeld(outcome.kid)) {
                accept(held.frame, room, seal, live, (held.meta["recipients"] as? Number)?.toInt(), held.meta["history"] == true)
            }
            return
        }
        if (RoomSeal.isSealedFrame(plain)) {
            val meta = LinkedHashMap<String, Any?>()
            if (history) meta["history"] = true
            if (recipients != null) meta["recipients"] = recipients
            val opened = seal.open(plain, now(), history = history, meta = meta) as? SealOpen.Opened ?: return
            plain = LinkedHashMap(opened.plain).also { copy ->
                copy.remove(RelayFrames.INTEGRATION_KEY)
                opened.senderNode?.let { copy["senderId"] = it }
            }
            plain = GroupCallE2ee.annotate(plain, opened.senderKid, opened.senderAcct)
        } else if (server) {
            val marked = LinkedHashMap(plain).also {
                it.remove("private")
                it["channel"] = "#${room.name}"
            }
            plain = RelayFrames.serverFrame(mapOf("srv" to true), GroupCallE2ee.annotate(marked, null, null)) ?: return
        } else {
            return
        }
        if (jsonText(plain["type"]) in GroupCallFrames.LIVE_TYPES) {
            if (room.isPrivate && live != null && !history) onLiveFrame?.invoke(plain, room.conversation)
            return
        }
        watchClaim(plain)
        val message = OfficeFrames.chatMessage(plain, room.conversation, selfId(), recipients, ::botAccount) ?: return
        if (append(message, room.id) && live != null && !message.self) onMessage?.invoke(message)
    }

    fun pruneExpired(nowMs: Long = now()): Boolean {
        val rule = retention ?: return false
        val changed = synchronized(lock) {
            var any = false
            for (room in rooms) {
                val list = history[room.conversation] ?: continue
                val key = ChatRetention.officeKey(room.id)
                val kept = list.filter { !rule.isExpired(it.timestamp, key, nowMs) }
                if (kept.size == list.size) continue
                history[room.conversation] = kept
                any = true
            }
            any
        }
        if (changed) notifyListeners()
        return changed
    }

    private fun append(incoming: ChatMessage, roomId: String): Boolean = synchronized(lock) {
        if (retention?.isExpired(incoming.timestamp, ChatRetention.officeKey(roomId), now()) == true) return@synchronized false
        val acked = if (incoming.recipients == null) ackedCounts[incoming.id] else null
        val message = if (acked != null) incoming.copy(recipients = acked) else incoming
        val ids = seen.getOrPut(roomId) { HashSet() }
        if (!ids.add(message.id)) {
            message.recipients?.let { setRecipients(it, message.id, message.conversation) }
            return@synchronized false
        }
        val current = history[message.conversation].orEmpty()
        val restored = current.indexOfFirst { it.id == message.id }
        if (restored >= 0) {
            history[message.conversation] = current.toMutableList().also { it[restored] = message }
            return@synchronized false
        }
        var list = insertSorted(current, message).toList()
        if (list.size > OfficeFrames.MAX_MESSAGES_PER_ROOM) list = list.takeLast(OfficeFrames.MAX_MESSAGES_PER_ROOM)
        history[message.conversation] = list
        true
    }

    fun open(conversation: String): Boolean {
        val room = room(conversation) ?: return false
        if (synchronized(lock) { sockets[room.id]?.socket != null }) return true
        val base = origin() ?: return false
        try {
            val access = api.accountApiCall("GET", "/api/chat/channels/${room.id}/room-access", null)
            val roomName = jsonText(access["room"])
            val passphrase = jsonText(access["passphrase"])
            val url = OfficeFrames.webSocketUrl(jsonText(access["webSocketUrl"]), base)
            if (roomName.isEmpty() || passphrase.isEmpty() || url == null) {
                lastError = OfficeException("unavailable").message
                notifyListeners()
                return false
            }
            val crypto = derive(roomName, passphrase)
            val seal = seal(roomName)
            val holder = RoomSocket(room.id, crypto, seal)
            val socket = connector.connect(
                url,
                onText = { text -> receive(text, holder) },
                onClosed = { dropped(holder) },
            )
            holder.socket = socket
            synchronized(lock) { sockets[room.id] = holder }
            advertise(holder)
            notifyListeners()
            onRoomOpened?.invoke(room.conversation, roomName)
            return true
        } catch (error: Exception) {
            lastError = error.message ?: error.toString()
            notifyListeners()
            return false
        }
    }

    private fun receive(text: String, holder: RoomSocket) {
        val room = rooms.firstOrNull { it.id == holder.roomId } ?: return
        val envelope = try {
            Json.parseObject(text)
        } catch (_: Exception) {
            return
        }
        if (envelope.isEmpty()) return
        val ack = OfficeFrames.recipientsAck(envelope)
        if (ack != null) {
            applyAck(ack.first, ack.second, room.conversation)
            notifyListeners()
            return
        }
        val plain = holder.crypto.decrypt(envelope) ?: return
        accept(plain, room, holder.seal, holder, OfficeFrames.recipients(envelope), server = envelope["srv"] == true)
        notifyListeners()
    }

    private fun rememberSent(ref: String, id: String) = synchronized(lock) {
        pendingRefs[ref] = id
        sendingIds.add(id)
        while (pendingRefs.size > OfficeFrames.MAX_PENDING_REFS) {
            val oldest = pendingRefs.keys.first()
            pendingRefs.remove(oldest)?.let {
                ackedCounts.remove(it)
                sendingIds.remove(it)
            }
        }
    }

    private fun applyAck(ref: String, count: Int, conversation: String) = synchronized(lock) {
        val id = pendingRefs[ref] ?: return@synchronized
        ackedCounts[id] = count
        sendingIds.remove(id)
        setRecipients(count, id, conversation)
    }

    private fun setRecipients(count: Int, id: String, conversation: String) {
        val list = history[conversation] ?: return
        val index = list.indexOfFirst { it.id == id }
        if (index < 0 || list[index].recipients == count) return
        history[conversation] = list.toMutableList().also { it[index] = it[index].copy(recipients = count) }
    }

    private fun dropped(holder: RoomSocket) {
        val removed = synchronized(lock) {
            if (sockets[holder.roomId] === holder) {
                sockets.remove(holder.roomId)
                true
            } else {
                false
            }
        }
        if (removed) notifyListeners()
    }

    private fun advertise(holder: RoomSocket) {
        val envelope = runCatching { holder.crypto.encrypt(holder.seal.keyFrame(now())) }.getOrNull() ?: return
        holder.socket?.sendText(Json.stringify(envelope))
    }

    fun closeAll() {
        val all = synchronized(lock) {
            val list = sockets.values.toList()
            sockets.clear()
            list
        }
        for (holder in all) holder.socket?.close()
        notifyListeners()
    }

    internal fun frame(room: OfficeRoom, text: String, bot: Boolean): LinkedHashMap<String, Any?> {
        val seq = sequence.incrementAndGet()
        val me = selfId()
        val at = now()
        val plain = linkedMapOf<String, Any?>(
            "type" to "chat",
            "id" to if (bot) UUID.randomUUID().toString().lowercase() else "$me-$at-$seq",
            "senderId" to me,
            "sender" to displayName().take(ChatLimits.MAX_DISPLAY_NAME_CHARS),
            "ts" to at,
            "platform" to platform,
            "version" to RelayRoom.CLIENT_VERSION,
            "channel" to "#${room.name}",
            "text" to text.take(ChatLimits.MAX_TEXT_CHARS),
        )
        if (bot) plain["bot"] = "forkbot"
        if (room.isPrivate) plain["private"] = true
        return plain
    }

    fun send(conversation: String, text: String, asBot: Boolean = false): Boolean = sendFrame(conversation, text, asBot).isNotEmpty()

    fun sendFrame(conversation: String, text: String, asBot: Boolean = false): String {
        val body = text.trim()
        val room = room(conversation) ?: return ""
        if (body.isEmpty() || room.readOnly) return ""
        if (asBot && room.isPrivate) return ""
        if (synchronized(lock) { sockets[room.id]?.socket == null } && !open(conversation)) return ""
        val holder = synchronized(lock) { sockets[room.id] } ?: return ""
        val socket = holder.socket ?: return ""
        val plain = frame(room, body, asBot)
        return try {
            val framed = holder.seal.seal(plain, now())
            if (holder.seal.needsAdvertisement()) advertise(holder)
            val envelope = LinkedHashMap(holder.crypto.encrypt(framed))
            envelope["persist"] = true
            envelope["notify"] = true
            val wire = Json.stringify(envelope)
            rememberSent(OfficeFrames.sentRef(wire), jsonText(plain["id"]))
            if (!socket.sendText(wire)) return ""
            OfficeFrames.chatMessage(plain, room.conversation, selfId(), botAccount = ::botAccount)?.let { append(it, room.id) }
            notifyListeners()
            jsonText(plain["id"])
        } catch (error: Exception) {
            lastError = error.message ?: error.toString()
            notifyListeners()
            ""
        }
    }

    private fun call(method: String, path: String, body: Map<String, Any?>?): Map<String, Any?> = try {
        api.accountApiCall(method, path, body)
    } catch (error: OrgApiException) {
        throw OfficeException(error.code)
    }

    fun create(name: String, isPrivate: Boolean, members: List<String>): OfficeRoom? {
        val normalized = name.trim().lowercase()
        if (!OfficeFrames.validName(normalized)) throw OfficeException("invalid_channel_name")
        val body = linkedMapOf<String, Any?>("name" to normalized, "visibility" to if (isPrivate) "private" else "public")
        if (isPrivate) body["members"] = members.map { it.trim().lowercase() }.filter { it.isNotEmpty() }.take(500)
        call("POST", "/api/chat/channels", body)
        refresh()
        return rooms.firstOrNull { it.name == normalized }
    }

    fun update(room: OfficeRoom, name: String? = null, topic: String? = null, isPrivate: Boolean? = null) {
        val body = LinkedHashMap<String, Any?>()
        if (name != null) {
            val normalized = name.trim().lowercase()
            if (!OfficeFrames.validName(normalized)) throw OfficeException("invalid_channel_name")
            body["name"] = normalized
        }
        if (topic != null) {
            val collapsed = topic.split(Regex("\\s+")).filter { it.isNotEmpty() }.joinToString(" ")
            if (collapsed.length > OfficeFrames.MAX_TOPIC_CHARS) throw OfficeException("topic_too_long")
            body["topic"] = collapsed
        }
        if (isPrivate != null) body["visibility"] = if (isPrivate) "private" else "public"
        if (body.isEmpty()) return
        call("PATCH", "/api/chat/channels/${room.id}", body)
        if (isPrivate != null) closeRoom(room.id)
        refresh()
    }

    private fun closeRoom(roomId: String) {
        val holder = synchronized(lock) { sockets.remove(roomId) } ?: return
        holder.socket?.close()
    }

    fun delete(room: OfficeRoom) {
        call("DELETE", "/api/chat/channels/${room.id}", null)
        closeRoom(room.id)
        refresh()
    }

    fun members(room: OfficeRoom): List<String> =
        jsonMaps(call("GET", "/api/chat/channels/${room.id}/members", null)["members"]).map { jsonText(it["username"]) }.filter { it.isNotEmpty() }

    fun addMember(room: OfficeRoom, username: String) {
        call("POST", "/api/chat/channels/${room.id}/members", linkedMapOf("username" to username.lowercase()))
        refresh()
    }

    fun removeMember(room: OfficeRoom, username: String) {
        call("DELETE", "/api/chat/channels/${room.id}/members", linkedMapOf("username" to username.lowercase()))
        refresh()
    }
}
