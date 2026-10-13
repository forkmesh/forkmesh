package com.forkmesh.nativeapp.chat

import com.forkmesh.nativeapp.api.AccountSettings
import com.forkmesh.nativeapp.api.DartException
import com.forkmesh.nativeapp.api.ServerUrl
import com.forkmesh.nativeapp.crypto.RoomCrypto
import com.forkmesh.nativeapp.crypto.RoomSeal
import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.json.jsonText
import com.forkmesh.nativeapp.model.ChatMessage
import com.forkmesh.nativeapp.model.ChatRetention
import com.forkmesh.nativeapp.model.Member
import com.forkmesh.nativeapp.model.MemberGroup
import com.forkmesh.nativeapp.model.UserStatus
import com.forkmesh.nativeapp.net.WebSocketClient
import com.forkmesh.nativeapp.security.DeviceIdentity
import com.forkmesh.nativeapp.security.SecretStore
import java.nio.ByteBuffer
import java.nio.charset.CharacterCodingException
import java.nio.charset.CodingErrorAction
import java.time.Instant
import java.time.LocalTime
import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.Future
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicLong

enum class RelayConnectionState { OFFLINE, CONNECTING, CONNECTED }

data class RelaySnapshot(
    val state: RelayConnectionState,
    val channels: List<String>,
    val currentConversation: String,
    val unread: Map<String, Int>,
    val history: Map<String, List<ChatMessage>>,
    val roster: List<Member>,
    val rosterGroups: List<MemberGroup>,
    val onlineCount: Int,
    val totalUnread: Int,
    val log: List<String>,
    val badgeUnread: Int = totalUnread,
) {
    fun messages(conversation: String): List<ChatMessage> = history[conversation].orEmpty()

    fun hasUnread(conversation: String): Boolean = (unread[conversation] ?: 0) > 0

    fun unreadCountFor(conversation: String): Int = unread[conversation] ?: 0
}

class RelayRoom(
    private val settings: AccountSettings,
    private val identity: DeviceIdentity,
    private val secureStore: SecretStore,
    private val platform: String = "android",
    val loop: EventLoop = EventLoop("RelayRoom"),
) {
    companion object {
        const val MAX_PEER_DIAGNOSTICS = 64
        const val CONNECT_TIMEOUT_MS = 15_000
        const val PING_INTERVAL_MS = 25_000L

        const val PRESENCE_INTERVAL_MS = 60_000L

        const val STALE_MS = 180_000L
        const val KEY_REPLY_DELAY_MS = 25L
        const val LOG_LIMIT = 500

        const val CLIENT_VERSION = "flutter-1.0"

        private val seq = AtomicLong()
    }

    @Volatile
    var roomPassphraseProvider: (() -> String)? = null

    @Volatile
    var onLogLine: ((String) -> Unit)? = null

    @Volatile
    var frameObserver: ((Map<String, Any?>) -> Unit)? = null

    @Volatile
    var onMessage: ((ChatMessage) -> Unit)? = null

    @Volatile
    var isReadElsewhere: ((ChatMessage) -> Boolean)? = null

    private val framesCounter = AtomicInteger()

    val framesReceived: Int get() = framesCounter.get()

    private val listeners = CopyOnWriteArrayList<() -> Unit>()

    fun addListener(listener: () -> Unit) {
        listeners.add(listener)
    }

    fun removeListener(listener: () -> Unit) {
        listeners.remove(listener)
    }

    private val writer = loop.SerialQueue("RelayRoom-writer")

    private var sharedPassphrase = ""

    private var sharedPassphraseScope = ""

    private var openGeneration = 0
    private var channel: WebSocketClient? = null
    private var currentSession: Session? = null
    private var crypto: RoomCrypto? = null

    private var seal: RoomSeal? = null

    private val reportedSealKeyIds = HashSet<String>()
    private var presenceTimer: LoopTimer? = null
    private var keyReplyTimer: LoopTimer? = null
    private var reconnectTimer: LoopTimer? = null
    private var pingTimer: LoopTimer? = null
    private var reconnectAttempt = 0
    private var userStopped = false
    private var disposed = false

    private var connectionState = RelayConnectionState.OFFLINE
    private val channelList = arrayListOf("#general", "#random")
    private var conversation = "#general"
    private val logLines = ArrayList<String>()

    private class Peer {
        var name = ""
        var platform = ""
        var version = ""
        var solana = ""
        var owner = ""
        var nodeName = ""
        var mirrors: List<String> = emptyList()
        var status: UserStatus? = null
        var statusTs = 0L
        var diagnostics: List<Any?>? = null
        var online = true
        var lastSeenMs = 0L
    }

    private val peers = LinkedHashMap<String, Peer>()
    private val history = LinkedHashMap<String, List<ChatMessage>>()
    private val seenIds = HashSet<String>()
    private val seenNotificationIds = HashSet<String>()
    private val unread = LinkedHashMap<String, Int>()
    private val unseenBadge = LinkedHashMap<String, Int>()
    private var retention: ChatRetention? = null

    private val nodeId: String get() = identity.nodeId

    @Volatile
    var accountProvider: (() -> String)? = null

    val persona: ChatPersona
        get() = ChatPersona.resolve(identity.publicKeyB64url, accountProvider?.invoke() ?: "", settings.displayName)

    private var announcedPersona: ChatPersona? = null

    fun identityChanged() = loop.post {
        val now = persona
        if (now == announcedPersona) return@post
        announcedPersona = now
        notifyListeners()
        if (connectionState == RelayConnectionState.CONNECTED) sendPresence()
    }

    private val name: String get() = persona.name

    val selfId: String get() = identity.nodeId

    @Volatile
    private var ownStatusValue: UserStatus? = null

    val ownStatus: UserStatus get() = ownStatusValue ?: UserStatus()

    fun setOwnStatus(status: UserStatus?) = loop.post {
        if (status == ownStatusValue) return@post
        ownStatusValue = status
        notifyListeners()
        if (status != null) sendPresence()
    }

    fun liveStatusFor(senderId: String): UserStatus? = loop.call {
        if (senderId.isEmpty()) return@call null
        if (senderId == nodeId) return@call ownStatusValue
        val p = peers[senderId] ?: return@call null
        if (!p.online || System.currentTimeMillis() - p.lastSeenMs > STALE_MS) return@call null
        p.status
    }

    val state: RelayConnectionState get() = loop.call { connectionState }

    val channels: List<String> get() = loop.call { ArrayList(channelList) }

    val currentConversation: String get() = loop.call { conversation }

    val log: List<String> get() = loop.call { ArrayList(logLines) }

    val unreadConversations: List<String> get() = loop.call { ArrayList(unread.keys) }

    fun hasUnread(conversation: String): Boolean = loop.call { (unread[conversation] ?: 0) > 0 }

    fun unreadCountFor(conversation: String): Int = loop.call { unread[conversation] ?: 0 }

    val totalUnread: Int get() = loop.call { unread.values.sum() }

    val badgeUnread: Int get() = loop.call { badgeUnreadNow() }

    private fun badgeUnreadNow(): Int = unseenBadge.entries.sumOf { minOf(it.value, unread[it.key] ?: 0) }

    fun snapshot(): RelaySnapshot = loop.call {
        val members = rosterNow()
        RelaySnapshot(
            state = connectionState,
            channels = ArrayList(channelList),
            currentConversation = conversation,
            unread = LinkedHashMap(unread),
            history = LinkedHashMap(history),
            roster = members,
            rosterGroups = memberGroupsOf(members),
            onlineCount = onlineCountNow(),
            totalUnread = unread.values.sum(),
            log = ArrayList(logLines),
            badgeUnread = badgeUnreadNow(),
        )
    }

    fun roster(): List<Member> = loop.call { rosterNow() }

    private fun rosterNow(): List<Member> {
        val now = System.currentTimeMillis()
        val self = persona
        val list = ArrayList<Member>()
        list.add(
            Member(
                id = nodeId,
                name = self.name,
                owner = self.ownerUser,
                self = true,
                online = connectionState == RelayConnectionState.CONNECTED,
                platform = platform,
                userStatus = ownStatus,
            ),
        )
        for ((id, p) in peers) {
            if (!p.online || now - p.lastSeenMs > STALE_MS) continue
            list.add(
                Member(
                    id = id,
                    name = p.name,
                    online = true,
                    platform = p.platform,
                    version = p.version,
                    solanaAddress = p.solana,
                    mirrors = p.mirrors,
                    owner = p.owner,
                    nodeName = p.nodeName,
                    userStatus = p.status ?: UserStatus(),
                    diagnostics = p.diagnostics,
                ),
            )
        }
        return list
    }

    val onlineCount: Int get() = loop.call { onlineCountNow() }

    private fun onlineCountNow(): Int {
        val now = System.currentTimeMillis()
        var count = if (connectionState == RelayConnectionState.CONNECTED) 1 else 0
        for (p in peers.values) {
            if (p.online && now - p.lastSeenMs <= STALE_MS) count++
        }
        return count
    }

    fun rosterGroups(): List<MemberGroup> = loop.call { memberGroupsOf(rosterNow()) }

    fun messages(conversation: String): List<ChatMessage> = loop.call { history[conversation].orEmpty() }

    val recentMessages: List<ChatMessage> get() = loop.call { recentNow() }

    private fun recentNow(): List<ChatMessage> =
        history.values.flatten().sortedByDescending { it.timestamp }

    val unreadMessages: List<ChatMessage>
        get() = loop.call { recentNow().filter { !it.self && unread.containsKey(it.conversation) } }

    val notificationMessages: List<ChatMessage> get() = loop.call { recentNow().filter { !it.self } }

    val unseenNotificationCount: Int
        get() = loop.call { recentNow().count { !it.self && it.id !in seenNotificationIds } }

    fun markNotificationsSeen() = loop.call {
        val before = seenNotificationIds.size
        for (message in recentNow()) {
            if (!message.self && message.id.isNotEmpty()) seenNotificationIds.add(message.id)
        }
        if (seenNotificationIds.size != before) notifyListeners()
    }

    fun connect() {
        loop.call {
            userStopped = false
            reconnectTimer?.cancel()
        }
        open()
    }

    fun onResumed() = loop.post {
        if (userStopped || disposed) return@post
        reconnectTimer?.cancel()
        reconnectAttempt = 0
        if (connectionState == RelayConnectionState.CONNECTED) {
            sendPresence()
        } else {
            loop.io { open() }
        }
    }

    private inner class Session(val crypto: RoomCrypto, val seal: RoomSeal) : WebSocketClient.Listener {
        @Volatile
        var abandoned = false

        var installed = false
        val early = ArrayList<() -> Unit>()

        fun onLoop(block: () -> Unit) = loop.post {
            if (abandoned) return@post
            if (installed) block() else early.add(block)
        }

        override fun onText(socket: WebSocketClient, text: String) = onFrame(this, text)

        override fun onBinary(socket: WebSocketClient, data: ByteArray) {
            val text = try {
                Charsets.UTF_8.newDecoder()
                    .onMalformedInput(CodingErrorAction.REPORT)
                    .onUnmappableCharacter(CodingErrorAction.REPORT)
                    .decode(ByteBuffer.wrap(data)).toString()
            } catch (_: CharacterCodingException) {
                framesCounter.incrementAndGet()
                return
            }
            onFrame(this, text)
        }

        override fun onPong(socket: WebSocketClient, payload: ByteArray) = onLoop {
            if (currentSession === this) armPing(this)
        }

        override fun onClosed(socket: WebSocketClient, code: Int, reason: String, error: Throwable?) = onLoop {
            if (currentSession === this) onClosed()
        }
    }

    private fun open() {
        val generation = loop.call {
            val next = ++openGeneration
            teardownSocket()
            next
        }
        if (loop.call { generation != openGeneration || disposed }) return
        loop.call { setState(RelayConnectionState.CONNECTING) }
        var socket: WebSocketClient? = null
        try {
            val room = settings.room
            val passphrase = settings.passphrase
            val roomCrypto = if (passphrase.isNotEmpty()) {
                RoomCrypto.withPassphrase(room, passphrase)
            } else {
                val scope = "${ServerUrl.parse(settings.serverUrl).host}|$room"
                val provider = loop.call {
                    if (sharedPassphraseScope != scope) sharedPassphrase = ""
                    if (sharedPassphrase.isEmpty()) roomPassphraseProvider else null
                }
                if (provider != null) {
                    val fetched = provider()
                    loop.call {
                        sharedPassphrase = fetched
                        sharedPassphraseScope = scope
                    }
                }
                val shared = loop.call { sharedPassphrase }
                if (shared.isEmpty()) throw DartException("Sign in to join chat - room key unavailable.")
                RoomCrypto.withPassphrase(room, shared)
            }
            if (!loop.call { (generation == openGeneration).also { if (it) crypto = roomCrypto } }) return
            val roomSeal = RoomSeal.loadOrCreate(room, storageScope = settings.serverUrl, secureStore = secureStore)
            val current = loop.call {
                (generation == openGeneration).also {
                    if (it) {
                        seal = roomSeal
                        reportedSealKeyIds.clear()
                    }
                }
            }
            if (!current) return
            val url = settings.serverUrl
            val host = ServerUrl.parse(url).host
            val session = Session(roomCrypto, roomSeal)
            val options = WebSocketClient.Options().apply {
                connectTimeoutMs = CONNECT_TIMEOUT_MS
                handshakeTimeoutMs = CONNECT_TIMEOUT_MS
            }
            socket = WebSocketClient.connect(url, session, options)
            val opened: WebSocketClient = socket
            val installed = loop.call {
                if (generation != openGeneration) return@call false
                channel = opened
                currentSession = session
                setState(RelayConnectionState.CONNECTED)
                reconnectAttempt = 0
                logLine("Connected to $host · room ${settings.room}")
                armPing(session)
                session.installed = true
                val buffered = ArrayList(session.early)
                session.early.clear()
                for (block in buffered) block()
                true
            }
            if (!installed) {
                session.abandoned = true
                loop.io { closeQuietly(opened) }
                return
            }
            val persist = roomSeal.shouldPersistAdvertisement()
            loop.call {
                if (currentSession !== session) return@call
                sendKeyAdvertisement(persist)
                sendHello()
                presenceTimer?.cancel()
                presenceTimer = loop.periodic(PRESENCE_INTERVAL_MS) {
                    sendPresence()
                    notifyListeners()
                }
            }
        } catch (error: Exception) {
            val stale = loop.call { generation != openGeneration }
            if (stale) {
                socket?.let { opened -> loop.io { closeQuietly(opened) } }
                return
            }
            loop.call {
                logLine("Connect failed: ${dartErrorText(error)}")
                teardownSocket()
                onClosed()
            }
        }
    }

    fun disconnect() {
        val bye = loop.call {
            userStopped = true
            openGeneration++
            reconnectTimer?.cancel()
            presenceTimer?.cancel()
            try {
                send(envelopeBase("bye"))
            } catch (_: Exception) {
                EventLoop.done(false)
            }
        }
        try {
            bye.get(5, TimeUnit.SECONDS)
        } catch (_: Exception) {
        }
        loop.call {
            teardownSocket()
            peers.clear()
            setState(RelayConnectionState.OFFLINE)
        }
    }

    private fun teardownSocket() {
        presenceTimer?.cancel()
        keyReplyTimer?.cancel()
        pingTimer?.cancel()
        currentSession?.abandoned = true
        currentSession = null
        val socket = channel
        channel = null
        if (socket != null) writer.execute { loop.io { closeQuietly(socket) } }
    }

    private fun onClosed() {
        presenceTimer?.cancel()
        keyReplyTimer?.cancel()
        pingTimer?.cancel()
        currentSession?.abandoned = true
        currentSession = null
        val socket = channel
        channel = null
        if (socket != null && socket.isOpen) loop.io { socket.cancel() }
        if (userStopped || disposed) {
            setState(RelayConnectionState.OFFLINE)
            return
        }
        setState(RelayConnectionState.CONNECTING)
        scheduleReconnect()
    }

    private fun scheduleReconnect() {
        if (userStopped || reconnectTimer?.isActive == true) return
        val delaySec = (1 shl reconnectAttempt.coerceIn(0, 9)).coerceIn(1, 300)
        reconnectAttempt++
        reconnectTimer = loop.timer(delaySec * 1000L) {
            if (!userStopped && !disposed) loop.io { open() }
        }
    }

    private fun armPing(session: Session) {
        pingTimer?.cancel()
        pingTimer = loop.timer(PING_INTERVAL_MS) {
            val socket = channel
            if (currentSession !== session || socket == null) return@timer
            writer.execute { socket.ping() }
            pingTimer = loop.timer(PING_INTERVAL_MS) {
                if (currentSession === session) loop.io { socket.cancel() }
            }
        }
    }

    private fun closeQuietly(socket: WebSocketClient) {
        try {
            socket.close(WebSocketClient.NORMAL_CLOSURE)
        } catch (_: Exception) {
        }
    }

    private fun envelopeBase(type: String): LinkedHashMap<String, Any?> {
        val m = linkedMapOf<String, Any?>(
            "type" to type,
            "id" to uuid(),
            "senderId" to nodeId,
            "ts" to System.currentTimeMillis(),
            "platform" to platform,
            "version" to CLIENT_VERSION,
        )
        val self = persona
        m["sender"] = self.name
        self.stamp(m)
        announcedPersona = self
        val solana = settings.solanaAddress
        if (solana.isNotEmpty()) m["solana"] = solana
        return m
    }

    private fun send(plain: Map<String, Any?>): Future<Boolean> {
        val roomCrypto = crypto
        val socket = channel
        val session = currentSession
        if (roomCrypto == null || socket == null || connectionState != RelayConnectionState.CONNECTED) {
            return EventLoop.done(false)
        }
        val roomSeal = seal
        return writer.submit {
            try {
                val persist = plain["persist"] == true
                val payload = if (persist) LinkedHashMap(plain).apply { remove("persist") } else plain
                if (roomSeal == null) throw IllegalStateException("End-to-end encryption unavailable.")
                val framed = if (isControllerReadableFrame(payload)) payload else roomSeal.seal(payload)
                val envelope = LinkedHashMap(roomCrypto.encrypt(framed))
                if (persist) {
                    envelope["persist"] = true
                    val type = jsonText(payload["type"])
                    if (type == "chat" || type == "thread-reply") envelope["notify"] = true
                }
                socket.sendText(Json.stringify(envelope))
            } catch (error: Exception) {
                loop.post {
                    if (currentSession === session && session != null) {
                        logLine("Send failed: ${dartErrorText(error)}")
                        onClosed()
                    }
                }
                false
            }
        }
    }

    private fun sendKeyAdvertisement(persist: Boolean) {
        val roomCrypto = crypto
        val roomSeal = seal
        val socket = channel
        if (roomCrypto == null || roomSeal == null || socket == null || connectionState != RelayConnectionState.CONNECTED) {
            return
        }
        writer.execute {
            val envelope = LinkedHashMap(roomCrypto.encrypt(roomSeal.keyFrame()))
            if (persist) envelope["persist"] = true
            socket.sendText(Json.stringify(envelope))
        }
    }

    private fun replyWithKeySoon() {
        if (keyReplyTimer?.isActive == true) return
        keyReplyTimer = loop.timer(KEY_REPLY_DELAY_MS) { sendKeyAdvertisement(false) }
    }

    private fun withOwnStatus(frame: LinkedHashMap<String, Any?>): LinkedHashMap<String, Any?> {
        ownStatusValue?.let { frame["userStatus"] = it.toJson() }
        return frame
    }

    private fun sendHello() {
        val hello = withOwnStatus(envelopeBase("hello"))
        hello["channels"] = ArrayList(channelList)
        send(hello)
    }

    private fun sendPresence() {
        if (connectionState != RelayConnectionState.CONNECTED) return
        send(withOwnStatus(envelopeBase("presence")))
    }

    fun deleteMessage(msg: ChatMessage): Boolean {
        val pending = loop.call {
            val target = msg.conversation
            val del = envelopeBase("delete")
            del["conversation"] = target
            del["target"] = msg.id
            if (target.startsWith("#")) {
                del["channel"] = target
                del["persist"] = true
            } else {
                del["to"] = target.removePrefix("@")
            }
            removeMessage(target, msg.id)
            seenIds.add(del["id"] as String)
            notifyListeners()
            send(del)
        }
        return try {
            pending.get()
        } catch (_: Exception) {
            false
        }
    }

    fun resetForRelaySwitch() = loop.call {
        history.clear()
        seenNotificationIds.clear()
        peers.clear()
        logLines.clear()
        sharedPassphrase = ""
        sharedPassphraseScope = ""
        crypto = null
        seal = null
        reportedSealKeyIds.clear()
        notifyListeners()
    }

    fun sendMessage(text: String): Boolean = sendMessage(text, loop.call { conversation })

    fun sendMessage(text: String, to: String): Boolean {
        val trimmed = dartTrimText(text)
        if (trimmed.isEmpty() || !(to.startsWith("#") || to.startsWith("@"))) return false
        val (pending, target, id) = loop.call {
            val target = to
            val msg = envelopeBase(if (target.startsWith("@")) "dm" else "chat")
            msg["text"] = trimmed
            msg["conversation"] = target
            if (target.startsWith("#")) {
                msg["channel"] = target
                msg["persist"] = true
            } else {
                msg["to"] = target.substring(1)
            }
            val id = msg["id"] as String
            val appended = appendMessage(
                ChatMessage(
                    id = id,
                    conversation = target,
                    senderId = nodeId,
                    senderName = name,
                    text = trimmed,
                    timestamp = msg["ts"] as Long,
                    self = true,
                ),
            )
            seenIds.add(id)
            if (appended) notifyListeners()
            Triple(send(msg), target, id)
        }
        val sent = try {
            pending.get()
        } catch (_: Exception) {
            false
        }
        if (!sent) {
            loop.call {
                removeMessage(target, id)
                seenIds.remove(id)
                notifyListeners()
            }
        }
        return sent
    }

    fun makeFrame(type: String): LinkedHashMap<String, Any?> = loop.call { envelopeBase(type) }

    fun sendFrame(frame: Map<String, Any?>): Boolean {
        val pending = loop.call { send(frame) }
        return try {
            pending.get()
        } catch (_: Exception) {
            false
        }
    }

    fun applyFrame(frame: Map<String, Any?>) = loop.call { handle(frame) }

    fun historySnapshot(): Map<String, List<ChatMessage>> = loop.call { LinkedHashMap(history) }

    fun setChatRetention(value: ChatRetention?) = loop.call {
        retention = value
    }

    fun pruneExpired(nowMs: Long = System.currentTimeMillis()): Boolean = loop.call {
        val rule = retention ?: return@call false
        var changed = false
        for (key in ArrayList(history.keys)) {
            val list = history[key] ?: continue
            val (gone, kept) = list.partition { rule.isExpired(it.timestamp, key, nowMs) }
            if (gone.isEmpty()) continue
            changed = true
            for (message in gone) seenNotificationIds.remove(message.id)
            if (kept.isEmpty()) history.remove(key) else history[key] = java.util.Collections.unmodifiableList(kept)
            val count = unread[key] ?: continue
            if (kept.isEmpty()) unread.remove(key) else if (count > kept.size) unread[key] = kept.size
        }
        if (changed) notifyListeners()
        changed
    }

    private fun expired(message: ChatMessage): Boolean =
        retention?.isExpired(message.timestamp, message.conversation, System.currentTimeMillis()) == true

    fun restoreHistory(messages: List<ChatMessage>) = loop.call {
        var changed = false
        for (message in messages) {
            if (message.id.isEmpty() || message.id in seenIds || expired(message)) continue
            seenIds.add(message.id)
            val list = history[message.conversation].orEmpty()
            var at = list.size
            while (at > 0 && compareMessages(list[at - 1], message) > 0) at--
            val next = ArrayList<ChatMessage>(list.size + 1)
            next.addAll(list)
            next.add(at, message)
            history[message.conversation] = java.util.Collections.unmodifiableList(next)
            if (message.conversation.startsWith("#") && message.conversation !in channelList) channelList.add(message.conversation)
            changed = true
        }
        if (changed) notifyListeners()
    }

    fun addChannel(channel: String) = loop.call {
        if (channel.isEmpty() || channel in channelList) return@call
        channelList.add(channel)
        notifyListeners()
    }

    fun removeChannel(channel: String) = loop.call {
        val removed = channelList.remove(channel)
        history.remove(channel)
        unread.remove(channel)
        unseenBadge.remove(channel)
        if (removed) notifyListeners()
    }

    fun switchConversation(conversation: String) = loop.call {
        this.conversation = conversation
        unread.remove(conversation)
        unseenBadge.remove(conversation)
        notifyListeners()
    }

    fun markConversationRead(conversation: String) = loop.call {
        unseenBadge.remove(conversation)
        if (unread.remove(conversation) != null) notifyListeners()
    }

    fun markAllRead() = loop.call {
        unseenBadge.clear()
        if (unread.isEmpty()) return@call
        unread.clear()
        notifyListeners()
    }

    fun markBadgeSeen() = loop.call {
        if (unseenBadge.isEmpty()) return@call
        unseenBadge.clear()
        notifyListeners()
    }

    fun noteUnread(conversation: String) = loop.call {
        if (conversation.isEmpty() || conversation == this.conversation) return@call
        unread[conversation] = (unread[conversation] ?: 0) + 1
        unseenBadge[conversation] = (unseenBadge[conversation] ?: 0) + 1
        notifyListeners()
    }

    private fun onFrame(session: Session, raw: String) {
        framesCounter.incrementAndGet()
        @Suppress("UNCHECKED_CAST")
        val envelope = try {
            Json.parse(raw) as? Map<String, Any?>
        } catch (_: Exception) {
            null
        } ?: return
        val decrypted = session.crypto.decrypt(envelope) ?: return
        val roomSeal = session.seal
        if (RoomSeal.isKeyFrame(decrypted)) {
            if (roomSeal.acceptKeyFrame(decrypted)) session.onLoop { replyWithKeySoon() }
            return
        }
        if (!RoomSeal.isSealedFrame(decrypted)) {
            if (isControllerReadableFrame(decrypted)) {
                session.onLoop { handle(decrypted) }
            } else {
                session.onLoop { noteSealMismatch("") }
            }
            return
        }
        val opened = roomSeal.unseal(decrypted)
        if (opened == null) {
            val keyId = RoomSeal.frameKeyId(decrypted)
            session.onLoop { noteSealMismatch(keyId) }
        } else {
            session.onLoop { handle(opened) }
        }
    }

    private fun isControllerReadableFrame(frame: Map<String, Any?>): Boolean {
        if (jsonText(frame["type"]) != "chat" || frame["private"] == true) return false
        return jsonText(frame["channel"]).startsWith("#")
    }

    private fun noteSealMismatch(keyId: String) {
        if (!reportedSealKeyIds.add(keyId)) return
        if (keyId.isEmpty()) {
            logLine("Automatic end-to-end encryption refused an unencrypted frame in ${settings.room}.")
            return
        }
        logLine(
            "An end-to-end encrypted message from device $keyId was not addressed " +
                "to this device, usually because it predates the first visit.",
        )
    }

    private fun handle(m: Map<String, Any?>) {
        val type = jsonText(m["type"])
        val senderId = jsonText(m["senderId"])
        val sender = jsonText(m["sender"])
        if (type != "typing") {
            val id = jsonText(m["id"])
            if (id.isNotEmpty()) {
                if (id in seenIds) {
                    if (senderId.isNotEmpty() && senderId != nodeId) rememberPeer(senderId, sender, m)
                    return
                }
                seenIds.add(id)
            }
        }
        if (senderId.isNotEmpty() && senderId != nodeId) rememberPeer(senderId, sender, m)

        when (type) {
            "hello" -> {
                val incoming = (m["channels"] as? List<*>)?.map { if (it == null) "null" else jsonText(it) } ?: emptyList()
                for (c in incoming) {
                    if (c.isNotEmpty() && c !in channelList) channelList.add(c)
                }
                if (jsonText(m["to"]).isEmpty() && senderId != nodeId) {
                    val reply = withOwnStatus(envelopeBase("hello"))
                    reply["channels"] = ArrayList(channelList)
                    reply["to"] = senderId
                    send(reply)
                }
            }
            "chat" -> {
                val target = if (m["channel"] == null) "#general" else jsonText(m["channel"])
                if (target !in channelList) channelList.add(target)
                receive(toMessage(m, target))
            }
            "dm" -> if (jsonText(m["to"]) == nodeId) receive(toMessage(m, "@$senderId"))
            "delete" -> {
                val target = jsonText(m["conversation"])
                val id = jsonText(m["target"])
                if (target.isNotEmpty() && id.isNotEmpty()) {
                    val original = history[target]?.firstOrNull { it.id == id }
                    if (original != null && original.senderId == senderId) removeMessage(target, id)
                }
            }
            "bye" -> if (peers.remove(senderId) != null) logLine("$sender left")
        }
        frameObserver?.let { observer ->
            try {
                observer(m)
            } catch (failure: Exception) {
                loop.failures.add(failure)
            }
        }
        notifyListeners()
    }

    private fun toMessage(m: Map<String, Any?>, conversation: String): ChatMessage {
        val senderId = jsonText(m["senderId"])
        val tsRaw = m["ts"]
        return ChatMessage(
            id = jsonText(m["id"]),
            conversation = conversation,
            senderId = senderId,
            senderName = jsonText(m["sender"]),
            text = jsonText(m["text"]),
            timestamp = if (tsRaw is Number) tsRaw.toLong() else System.currentTimeMillis(),
            self = senderId == nodeId,
            fileName = jsonText(m["fileName"]),
        )
    }

    private fun receive(msg: ChatMessage) {
        if (appendMessage(msg) && !msg.self) onMessage?.invoke(msg)
    }

    private fun appendMessage(msg: ChatMessage): Boolean {
        if (expired(msg)) return false
        val list = history[msg.conversation].orEmpty()
        if (list.any { it.id == msg.id }) return false
        var at = list.size
        while (at > 0 && compareMessages(list[at - 1], msg) > 0) at--
        val next = ArrayList<ChatMessage>(list.size + 1)
        next.addAll(list)
        next.add(at, msg)
        history[msg.conversation] = java.util.Collections.unmodifiableList(next)
        if (!msg.self && msg.conversation != conversation && isReadElsewhere?.invoke(msg) != true) {
            unread[msg.conversation] = (unread[msg.conversation] ?: 0) + 1
            unseenBadge[msg.conversation] = (unseenBadge[msg.conversation] ?: 0) + 1
        }
        return true
    }

    private fun removeMessage(conversation: String, id: String) {
        val list = history[conversation] ?: return
        val next = list.filter { it.id != id }
        if (next.size == list.size) return
        if (next.isEmpty()) history.remove(conversation) else history[conversation] = java.util.Collections.unmodifiableList(next)
        seenNotificationIds.remove(id)
    }

    private fun compareMessages(a: ChatMessage, b: ChatMessage): Int {
        val byTimestamp = a.timestamp.compareTo(b.timestamp)
        if (byTimestamp != 0) return byTimestamp
        return a.id.compareTo(b.id)
    }

    private fun rememberPeer(id: String, name: String, m: Map<String, Any?>) {
        val p = peers.getOrPut(id) { Peer() }
        if (name.isNotEmpty()) p.name = name
        val platform = jsonText(m["platform"])
        if (platform.isNotEmpty()) p.platform = platform
        val version = jsonText(m["version"])
        if (version.isNotEmpty()) p.version = version
        val solana = jsonText(m["solana"])
        if (solana.isNotEmpty()) p.solana = solana
        val owner = jsonText(m["ownerUser"] ?: m["owner"])
        if (owner.isNotEmpty()) p.owner = owner
        val nodeName = jsonText(m["nodeName"])
        if (nodeName.isNotEmpty()) p.nodeName = nodeName
        val mirrors = m["mirrors"]
        if (mirrors is List<*>) p.mirrors = mirrors.map { if (it == null) "null" else jsonText(it) }
        val findings = m["dg"]
        if (findings is List<*>) p.diagnostics = findings.take(MAX_PEER_DIAGNOSTICS)
        rememberPeerStatus(p, m)
        p.online = true
        p.lastSeenMs = System.currentTimeMillis()
    }

    private fun rememberPeerStatus(p: Peer, m: Map<String, Any?>) {
        val type = jsonText(m["type"])
        if (type != "hello" && type != "presence") return
        val status = m["userStatus"] as? Map<*, *> ?: return
        val now = System.currentTimeMillis()
        val tsRaw = m["ts"]
        val ts = if (tsRaw is Number) tsRaw.toLong() else now
        if (now - ts > STALE_MS || ts < p.statusTs) return
        p.status = UserStatus.fromJson(status)
        p.statusTs = ts
    }

    private fun setState(next: RelayConnectionState) {
        if (connectionState == next) return
        connectionState = next
        notifyListeners()
    }

    private fun logLine(line: String) {
        val now = LocalTime.now()
        val stamp = "%02d:%02d:%02d".format(now.hour, now.minute, now.second)
        logLines.add("$stamp  $line")
        if (logLines.size > LOG_LIMIT) logLines.removeAt(0)
        onLogLine?.invoke(line)
        notifyListeners()
    }

    private fun notifyListeners() {
        for (listener in listeners) {
            try {
                listener()
            } catch (failure: Exception) {
                loop.failures.add(failure)
            }
        }
    }

    private fun uuid(): String {
        val next = seq.incrementAndGet()
        val now = Instant.now()
        val micros = now.epochSecond * 1_000_000L + now.nano / 1_000
        return "$nodeId-$micros-$next"
    }

    fun dispose() {
        loop.call {
            disposed = true
            userStopped = true
            presenceTimer?.cancel()
            keyReplyTimer?.cancel()
            reconnectTimer?.cancel()
            pingTimer?.cancel()
            currentSession?.abandoned = true
            currentSession = null
            val socket = channel
            channel = null
            if (socket != null) loop.io { socket.cancel() }
        }
        listeners.clear()
        writer.shutdown()
        loop.shutdown()
    }
}
