package com.forkmesh.nativeapp.chat

import com.forkmesh.nativeapp.apps.AppReply
import com.forkmesh.nativeapp.apps.ChatApps
import com.forkmesh.nativeapp.inbox.LocalPingLog
import com.forkmesh.nativeapp.json.jsonText
import com.forkmesh.nativeapp.model.ChatMessage
import com.forkmesh.nativeapp.model.ChatReceipt
import com.forkmesh.nativeapp.model.ChatReceiptTimes
import com.forkmesh.nativeapp.model.ChatRetention
import com.forkmesh.nativeapp.model.Member
import com.forkmesh.nativeapp.model.MemberGroup

interface ChatRelay : ChatFrameSink {
    val channels: List<String>
    val currentConversation: String
    val connected: Boolean

    fun messages(conversation: String): List<ChatMessage>

    fun roster(): List<Member>

    fun rosterGroups(): List<MemberGroup>

    fun switchConversation(conversation: String)

    fun addChannel(channel: String)

    fun removeChannel(channel: String)

    fun historySnapshot(): Map<String, List<ChatMessage>>

    fun restoreHistory(messages: List<ChatMessage>)

    fun deleteMessage(message: ChatMessage): Boolean

    fun connect()

    fun setChatRetention(retention: ChatRetention?) {}

    fun pruneExpired(nowMs: Long): Boolean = false

    fun observeMessages(listener: (ChatMessage) -> Unit) {}

    fun noteUnread(conversation: String) {}

    val unreadConversations: List<String> get() = emptyList()

    fun markConversationRead(conversation: String) {}

    fun markAllRead() {}

    fun setReadElsewhere(check: (ChatMessage) -> Boolean) {}

    fun setHiddenChannels(isHidden: (String) -> Boolean) {}

    fun botAccount(node: String): String = ""
}

class RelayRoomChat(val room: RelayRoom) : ChatRelay {
    override val selfId: String get() = room.selfId
    override val channels: List<String> get() = room.channels
    override val currentConversation: String get() = room.currentConversation
    override val connected: Boolean get() = room.state == RelayConnectionState.CONNECTED

    override fun makeFrame(type: String): LinkedHashMap<String, Any?> = room.makeFrame(type)

    override fun sendFrame(frame: Map<String, Any?>): Boolean = room.sendFrame(frame)

    override fun applyFrame(frame: Map<String, Any?>) = room.applyFrame(frame)

    override fun sendMessage(text: String, to: String): Boolean = room.sendMessage(text, to)

    override fun messages(conversation: String): List<ChatMessage> = room.messages(conversation)

    override fun roster(): List<Member> = room.roster()

    override fun rosterGroups(): List<MemberGroup> = room.rosterGroups()

    override fun switchConversation(conversation: String) = room.switchConversation(conversation)

    override fun addChannel(channel: String) = room.addChannel(channel)

    override fun removeChannel(channel: String) = room.removeChannel(channel)

    override fun historySnapshot(): Map<String, List<ChatMessage>> = room.historySnapshot()

    override fun restoreHistory(messages: List<ChatMessage>) = room.restoreHistory(messages)

    override fun deleteMessage(message: ChatMessage): Boolean = room.deleteMessage(message)

    override fun connect() = room.connect()

    override fun setChatRetention(retention: ChatRetention?) = room.setChatRetention(retention)

    override fun pruneExpired(nowMs: Long): Boolean = room.pruneExpired(nowMs)

    override fun observeMessages(listener: (ChatMessage) -> Unit) {
        room.onMessage = listener
    }

    override fun noteUnread(conversation: String) = room.noteUnread(conversation)

    override val unreadConversations: List<String> get() = room.unreadConversations

    override fun markConversationRead(conversation: String) = room.markConversationRead(conversation)

    override fun markAllRead() = room.markAllRead()

    override fun setReadElsewhere(check: (ChatMessage) -> Boolean) {
        room.isReadElsewhere = check
    }

    override fun botAccount(node: String): String = room.botAccount(node)

    override fun setHiddenChannels(isHidden: (String) -> Boolean) {
        room.isHiddenChannel = isHidden
    }
}

class ChatReplyException(val kind: Kind, val name: String = "") : RuntimeException(
    when (kind) {
        Kind.UNKNOWN_CONVERSATION -> "No conversation named $name is open on this phone."
        Kind.OFFLINE -> "Chat is offline."
        Kind.REFUSED -> "The reply could not be sent."
    },
) {
    enum class Kind { UNKNOWN_CONVERSATION, OFFLINE, REFUSED }
}

object ChatReplyTarget {
    private fun best(others: List<Member>, folded: String): Member? {
        val matches = others.filter {
            it.owner.lowercase() == folded || it.name.lowercase() == folded || it.nodeName.lowercase() == folded
        }
        return matches.firstOrNull { it.online } ?: matches.firstOrNull()
    }

    fun resolve(raw: String, channels: List<String>, roster: List<Member>, offices: List<String> = emptyList()): String {
        val name = raw.trim()
        if (name.isEmpty()) throw ChatReplyException(ChatReplyException.Kind.UNKNOWN_CONVERSATION, raw)
        if (name.startsWith("#")) return name
        val others = roster.filter { !it.self }
        if (name.startsWith("@")) {
            val rest = name.drop(1)
            if (others.any { it.id == rest }) return name
            val match = best(others, rest.lowercase()) ?: return name
            return "@${match.id}"
        }
        if ("#$name" in channels) return "#$name"
        val office = OfficeFrames.conversation(name)
        if (office in offices) return office
        others.firstOrNull { it.id == name }?.let { return "@${it.id}" }
        val match = best(others, name.lowercase()) ?: throw ChatReplyException(ChatReplyException.Kind.UNKNOWN_CONVERSATION, name)
        return "@${match.id}"
    }
}

data class ChatRow(
    val message: ChatMessage,
    val continuesGroup: Boolean,
    val deletable: Boolean,
    val text: String = message.text,
    val edited: Boolean = false,
    val editable: Boolean = false,
    val reactions: List<ReactionChip> = emptyList(),
    val stickers: List<StickerChip> = emptyList(),
    val thread: ChatThreadSummary? = null,
    val attachment: ChatAttachment? = null,
    val visitor: Boolean = false,
    val receipt: ChatReceipt? = null,
    val viewOnce: ViewOnceEntry? = null,
)

sealed class ChatHubSend {
    data object Sent : ChatHubSend()
    data object Empty : ChatHubSend()
    data class Issue(val argument: String) : ChatHubSend()
    data class Failed(val message: String) : ChatHubSend()
}

class ChatHub(val relay: ChatRelay, val extras: ChatExtras = ChatExtras()) {
    companion object {
        const val PING_FRESH_MS = 10 * 60 * 1000L
        const val WELCOME_CHANNEL = "#welcome"
        const val PUBLIC_SEAL_SUMMARY =
            "This channel is public: everyone in this room can read it, guests included. Every message is signed by the " +
                "device that sent it and sealed to the devices in the room, so nobody can post, edit or delete as someone else. " +
                "Integrations and webhooks post under their own name.\n\n" +
                "Private channels and direct messages are kept by the server for their members only and add device-to-device encryption."
        const val MESH_SEAL_SUMMARY =
            "Anyone nearby with ForkMesh open can read #mesh. Messages stay on this network and Bluetooth and never reach the server."
        const val GUEST_DIRECT_NOTICE = "Guests have no direct messages. Ask them to sign in first."
        const val PRIVATE_SEAL_SUMMARY =
            "Encryption is automatic. This device keeps a signing key and daily prekeys (X25519 and ML-KEM-768) that never leave it. " +
                "Every message is signed and gets a fresh key wrapped for each approved device, so the server can forward and " +
                "store it but cannot read or forge it. Prekeys are wiped after 8 days.\n\nDirect messages also use a ratchet " +
                "(X25519 and ML-KEM-768): every message has its own key, deleted once used, so a stolen phone cannot open " +
                "messages it already received. In direct messages a new device of " +
                "either person waits until you approve it; compare the safety number to check. A new device cannot open " +
                "older history. ForkBot runs on the relay and is unavailable in end-to-end encrypted rooms."
        const val SERVER_VISIBLE_SEAL_SUMMARY =
            "Server notices: ForkMesh writes these notes itself, so the server can read them. " +
                "They are not end-to-end encrypted. Only you can see this channel, and nobody can post in it."
        const val FORKBOT_PRIVATE_NOTICE = "ForkBot is available only in public, controller-readable channels."
        const val OFFLINE_NOTICE = "Chat is offline. Reconnecting..."
    }

    @Volatile
    var retention: ChatRetention = ChatRetention()
        private set

    @Volatile
    var office: OfficeChannelService? = null
        set(value) {
            value?.retention = retention
            value?.addListener { purgePrivatePings() }
            field = value
            restoreOfficeHistory()
            value?.onMessage = {
                filePing(it)
                if (relay.currentConversation == it.conversation) queueReadMark(it.conversation)
                else if (!isReadElsewhere(it)) relay.noteUnread(it.conversation)
            }
        }

    @Volatile
    var live: DirectLiveEngine? = null

    @Volatile
    var direct: DirectMessageService? = null
        set(value) {
            field = value
            value?.onFrame = { frame, _ -> extras.apply(frame) }
            value?.replyAuthorOf = { extras.replyAuthorOf(it) }
            value?.onRemoved = { _, id -> extras.hideMessage(id) }
            value?.onMessage = { message ->
                filePing(message)
                if (relay.currentConversation == message.conversation) {
                    value.markRead(message.conversation)
                    queueReadMark(message.conversation)
                } else if (!isReadElsewhere(message)) {
                    value.bumpUnread(message.conversation)
                }
            }
            restoreDirectHistory()
        }

    @Volatile
    var readMarks: ChatReadMarks? = null

    @Volatile
    var calls: DirectCallEngine? = null

    @Volatile
    var pings: LocalPingLog? = null
        set(value) {
            field = value
            purgePrivatePings()
        }

    @Volatile
    var onPingsChanged: (() -> Unit)? = null

    @Volatile
    var forkbot: ChatAccountCalling? = null

    @Volatile
    var apps: ChatApps? = null

    @Volatile
    var onNotice: ((String) -> Unit)? = null

    @Volatile
    var revealRequests = 0
        private set

    @Volatile
    var onReveal: (() -> Unit)? = null

    @Volatile
    var selfName: () -> String = { relay.roster().firstOrNull { it.self }?.name ?: "" }

    @Volatile
    var blocks: ChatBlocks? = null

    private var history: ChatHistoryStore? = null
    private var historyScope: () -> String = { "" }
    private var historyFormerScopes: () -> List<String> = { emptyList() }

    @Volatile
    private var loadedScope = ""

    private class DirectHeld(val messages: List<ChatMessage>, val receipts: Map<String, ChatReceipt>, val times: Map<String, ChatReceiptTimes>)

    private val heldLock = Any()
    private var directRestore: DirectHeld? = null
    private var officeRestore: List<ChatMessage>? = null
    private var directAccount = ""

    init {
        extras.sink = relay
        extras.botAccount = { relay.botAccount(it) }
        extras.authorOf = { conversation, id -> messages(conversation).firstOrNull { it.id == id }?.senderId }
        extras.onChannelJoined = {
            relay.addChannel(it)
            purgePrivatePings()
        }
        extras.onChannelRemoved = { relay.removeChannel(it) }
        relay.observeMessages {
            filePing(it)
            if (relay.currentConversation == it.conversation) queueReadMark(it.conversation)
        }
        relay.setReadElsewhere { isReadElsewhere(it) }
        relay.setHiddenChannels(extras::isHiddenChannel)
    }

    fun filePing(message: ChatMessage, nowMs: Long = System.currentTimeMillis()) {
        val log = pings ?: return
        if (message.self || message.id.isEmpty() || nowMs - message.timestamp >= PING_FRESH_MS) return
        val conversation = message.conversation
        if (isPrivateRoom(conversation)) return
        val name = message.senderName.trim()
        val who = name.ifEmpty { "Someone" }
        val text = message.text.trim()
        var body = if (message.fileName.isEmpty()) text else "\uD83D\uDCCE ${message.fileName}" + if (text.isEmpty()) "" else "\n$text"
        val title = when {
            conversation == WELCOME_CHANNEL -> {
                body = if (name.isEmpty()) "Someone new said hello in #welcome" else "$name said hello in #welcome"
                "New user joined"
            }
            else -> "$who in ${office?.room(conversation)?.name ?: conversation}"
        }
        log.recordChat(message.id, name, title, body, conversation, message.timestamp, relay.currentConversation == conversation) ?: return
        onPingsChanged?.invoke()
    }

    fun purgePrivatePings() {
        val log = pings ?: return
        if (log.removeChats { it.isNotEmpty() && isPrivateRoom(it) }) onPingsChanged?.invoke()
    }

    private fun notice(text: String) {
        onNotice?.invoke(text)
    }

    fun attachHistory(store: ChatHistoryStore, formerScopes: () -> List<String> = { emptyList() }, scope: () -> String) {
        history = store
        historyScope = scope
        historyFormerScopes = formerScopes
        restoreHistoryIfNeeded()
    }

    fun retentionKey(conversation: String): String = ChatRetention.key(conversation, office?.room(conversation)?.id.orEmpty())

    fun retentionMs(conversation: String): Long = retention.effectiveMs(retentionKey(conversation))

    fun retentionOverrideMs(conversation: String): Long? = retention.overrideMs(retentionKey(conversation))

    fun setRetention(value: ChatRetention, nowMs: Long = System.currentTimeMillis()): Boolean {
        retention = value
        relay.setChatRetention(value)
        extras.retention = value
        office?.retention = value
        return pruneExpired(nowMs)
    }

    fun pruneExpired(nowMs: Long = System.currentTimeMillis()): Boolean {
        val relayChanged = relay.pruneExpired(nowMs)
        val repliesChanged = extras.pruneExpired(retention, nowMs)
        val officeChanged = office?.pruneExpired(nowMs) == true
        return relayChanged || repliesChanged || officeChanged
    }

    private fun live(message: ChatMessage, nowMs: Long): Boolean = !retention.isExpired(message.timestamp, message.conversation, nowMs)

    fun restoreHistoryIfNeeded(): Boolean {
        val store = history ?: return false
        val scope = historyScope()
        if (scope.isEmpty()) return false
        if (scope == loadedScope) return true
        val loaded = when (val load = store.read(scope, historyFormerScopes())) {
            ChatHistoryLoad.Unreadable -> return false
            ChatHistoryLoad.Missing -> {
                loadedScope = scope
                return true
            }
            is ChatHistoryLoad.Loaded -> load.snapshot
        }
        loadedScope = scope
        val now = System.currentTimeMillis()
        val snapshot = withoutRelayPrivate(loaded)
        extras.restoreChannels(snapshot.hiddenChannels)
        val shown = snapshot.messages.filter { it.conversation !in snapshot.hiddenChannels && live(it, now) }
        relay.restoreHistory(shown.filter { !DirectMessageFrames.isDirect(it.conversation) && !OfficeFrames.isOffice(it.conversation) })
        synchronized(heldLock) {
            officeRestore = shown.filter { OfficeFrames.isOffice(it.conversation) }
            directRestore = DirectHeld(
                snapshot.messages.filter { DirectMessageFrames.isDirect(it.conversation) },
                snapshot.receipts,
                snapshot.receiptTimes,
            )
        }
        restoreOfficeHistory()
        restoreDirectHistory()
        extras.restore(snapshot.replies.filter { live(it, now) }, snapshot.replyRoots, snapshot.reactions, snapshot.reactionOrder, snapshot.edits, snapshot.stickers)
        extras.restoreHiddenMessages(snapshot.hiddenMessages)
        return true
    }

    fun withoutRelayPrivate(snapshot: ChatHistorySnapshot): ChatHistorySnapshot {
        val privateNames = snapshot.privateChannels.toSet()
        fun relayPrivate(conversation: String): Boolean = conversation.startsWith("@") || conversation in privateNames
        val goneIds = (snapshot.messages + snapshot.replies).filter { relayPrivate(it.conversation) }.map { it.id }.toSet()
        if (goneIds.isEmpty() && privateNames.isEmpty()) return snapshot
        val replies = snapshot.replies.filter { !relayPrivate(it.conversation) && snapshot.replyRoots[it.id] !in goneIds }
        val kept = replies.map { it.id }.toSet()
        val gone = goneIds + (snapshot.replyRoots.keys - kept)
        return snapshot.copy(
            messages = snapshot.messages.filter { !relayPrivate(it.conversation) },
            replies = replies,
            replyRoots = snapshot.replyRoots.filterKeys { it in kept },
            reactions = snapshot.reactions.filterKeys { it !in gone },
            reactionOrder = snapshot.reactionOrder.filterKeys { it !in gone },
            stickers = snapshot.stickers.filterKeys { it !in gone },
            edits = snapshot.edits.filterKeys { it !in gone },
            privateChannels = emptyList(),
            hiddenMessages = snapshot.hiddenMessages.filter { it !in gone },
            receipts = snapshot.receipts.filterKeys { it !in gone },
            receiptTimes = snapshot.receiptTimes.filterKeys { it !in gone },
        )
    }

    private fun restoreDirectHistory() {
        val service = direct ?: return
        val saved = synchronized(heldLock) { directRestore.also { directRestore = null } } ?: return
        service.restore(saved.messages, saved.receipts, saved.times)
    }

    private fun restoreOfficeHistory() {
        val service = office ?: return
        val saved = synchronized(heldLock) { officeRestore.also { officeRestore = null } } ?: return
        service.restore(saved)
    }

    fun directSessionChanged(account: String) {
        val name = account.trim().lowercase()
        if (name.isEmpty()) {
            val service = direct ?: return
            synchronized(heldLock) {
                val held = (service.historySnapshot().values.flatten() + directRestore?.messages.orEmpty()).distinctBy { it.id }
                val receipts = LinkedHashMap(directRestore?.receipts.orEmpty()).apply { putAll(service.receiptSnapshot()) }
                val times = LinkedHashMap(directRestore?.times.orEmpty()).apply { putAll(service.receiptTimeSnapshot()) }
                if (held.isNotEmpty()) directRestore = DirectHeld(held, receipts, times)
            }
            return
        }
        synchronized(heldLock) {
            if (directAccount.isNotEmpty() && directAccount != name) directRestore = null
            directAccount = name
        }
        restoreDirectHistory()
    }

    fun saveHistory() {
        val store = history ?: return
        if (!restoreHistoryIfNeeded()) return
        store.save(historySnapshot(), loadedScope)
    }

    fun historySnapshot(nowMs: Long = System.currentTimeMillis()): ChatHistorySnapshot {
        val conversations = LinkedHashMap(relay.historySnapshot())
        for (source in listOf(direct?.historySnapshot().orEmpty(), office?.historySnapshot().orEmpty())) {
            for ((key, list) in source) if (key !in conversations) conversations[key] = list
        }
        val (heldDirect, heldOffice) = synchronized(heldLock) { directRestore to officeRestore }
        for (held in listOf(heldDirect?.messages.orEmpty(), heldOffice.orEmpty())) {
            for ((conversation, list) in held.groupBy { it.conversation }) {
                val ids = conversations[conversation].orEmpty().map { it.id }.toSet()
                val missing = list.filter { it.id !in ids }
                if (missing.isEmpty()) continue
                conversations[conversation] = (conversations[conversation].orEmpty() + missing).sortedBy { it.timestamp }
            }
        }
        val messages = ChatHistoryCodec.bounded(conversations, retention, nowMs)
        val kept = messages.map { it.id }.toSet()
        val replies = extras.snapshotThreads().filter { live(it, nowMs) }
        val replyIds = replies.map { it.id }.toSet()
        val known = kept + replyIds
        val receipts = LinkedHashMap(heldDirect?.receipts.orEmpty()).apply { putAll(direct?.receiptSnapshot().orEmpty()) }
        val times = LinkedHashMap(heldDirect?.times.orEmpty()).apply { putAll(direct?.receiptTimeSnapshot().orEmpty()) }
        return ChatHistorySnapshot(
            messages = messages,
            replies = replies,
            replyRoots = extras.snapshotReplyRoots().filterKeys { it in replyIds },
            reactions = extras.snapshotReactions().filterKeys { it in known },
            reactionOrder = extras.snapshotReactionOrder().filterKeys { it in known },
            stickers = extras.snapshotStickers().filterKeys { it in known },
            edits = extras.snapshotEdits().filterKeys { it in known },
            privateChannels = emptyList(),
            hiddenChannels = extras.hiddenChannels.sorted(),
            hiddenMessages = extras.snapshotHiddenMessages(),
            receipts = receipts.filterKeys { it in kept },
            receiptTimes = times.filterKeys { it in kept },
        )
    }

    val currentConversation: String get() = relay.currentConversation

    val inOffice: Boolean get() = OfficeFrames.isOffice(relay.currentConversation)

    val inDirect: Boolean get() = DirectMessageFrames.isDirect(relay.currentConversation)

    val directConversations: List<DirectConversation> get() = direct?.records.orEmpty()

    fun listPreview(conversation: String): ChatListPreview? {
        val inDirect = DirectMessageFrames.isDirect(conversation)
        val rich = inDirect || (!OfficeFrames.isOffice(conversation) && conversation != MESH_CONVERSATION)
        return ChatListPreviews.newest(
            messages(conversation),
            direct = inDirect,
            skip = { extras.isHidden(it.id) || extras.isThreadReply(it.id) },
            text = { if (rich) extras.text(it) else it.text },
            fileName = { it.fileName.ifEmpty { extras.attachment(it.id)?.fileName.orEmpty() } },
        )
    }

    fun directListRows(): List<Pair<DirectConversation, ChatListPreview?>> =
        directConversations
            .filter { !isBlocked(it.otherUser) }
            .map { it to listPreview(it.conversation) }
            .sortedByDescending { (record, preview) -> maxOf(ChatListPreviews.recencyMs(record.updatedAt), preview?.timestamp ?: 0L) }

    val channels: List<String>
        get() {
            val hidden = extras.hiddenChannels
            return relay.channels.filter { it !in hidden && !OfficeFrames.isOffice(it) }
        }

    val officeChannels: List<String> get() = office?.conversations.orEmpty()

    fun messages(conversation: String): List<ChatMessage> = when {
        OfficeFrames.isOffice(conversation) -> office?.messages(conversation).orEmpty()
        DirectMessageFrames.isDirect(conversation) -> direct?.messages(conversation).orEmpty()
        else -> relay.messages(conversation)
    }

    fun receipt(message: ChatMessage): ChatReceipt? {
        if (!message.self || message.ephemeral || message.conversation == MESH_CONVERSATION) return null
        return when {
            DirectMessageFrames.isDirect(message.conversation) -> direct?.receipt(message.id)
            OfficeFrames.isOffice(message.conversation) -> office?.receipt(message.id)
            else -> null
        } ?: ChatReceipt.SENT
    }

    fun receiptTimes(message: ChatMessage): ChatReceiptTimes {
        if (!message.self || !DirectMessageFrames.isDirect(message.conversation)) return ChatReceiptTimes()
        return direct?.receiptTime(message.id) ?: ChatReceiptTimes()
    }

    fun expiresAt(message: ChatMessage): Long = message.timestamp + retentionMs(message.conversation)

    fun isOnServer(message: ChatMessage, receipt: ChatReceipt?): Boolean {
        if (!message.self || receipt != ChatReceipt.SENT || !DirectMessageFrames.isDirect(message.conversation)) return false
        return direct?.serverCopies(message.conversation) != 0
    }

    fun isFromBlocked(message: ChatMessage): Boolean {
        val list = blocks ?: return false
        if (message.self) return false
        if (DirectMessageFrames.isDirect(message.conversation)) return list.isBlocked(direct?.record(message.conversation)?.otherUser.orEmpty())
        val owner = relay.roster().firstOrNull { it.id == message.senderId }?.owner.orEmpty()
        return owner.isNotEmpty() && list.isBlocked(owner)
    }

    fun isBlocked(account: String): Boolean = blocks?.isBlocked(account) == true

    fun setBlocked(account: String, blocked: Boolean): Boolean {
        val list = blocks ?: return false
        if (!list.set(account, blocked)) return false
        val name = ChatBlocks.normalized(account)
        notice(if (blocked) "$name is blocked. You won't see their messages on this device." else "$name is unblocked.")
        return true
    }

    fun rows(conversation: String): List<ChatRow> {
        val stored = messages(conversation).filter { !extras.isHidden(it.id) && !isFromBlocked(it) }
        val list = apps?.merge(stored, conversation) ?: stored
        val inDirect = DirectMessageFrames.isDirect(conversation)
        val relayRoom = !OfficeFrames.isOffice(conversation) && !inDirect && conversation != MESH_CONVERSATION
        val rich = relayRoom || inDirect
        return list.mapIndexed { index, message ->
            val previous = list.getOrNull(index - 1)
            val base = ChatRow(
                message = message,
                continuesGroup = !message.ephemeral && previous?.ephemeral != true && continuesGroup(previous, message),
                deletable = rich && message.self && !message.ephemeral,
                visitor = !message.self && ChatVisitorPresence.isTransientVisitor(name = message.senderName),
                receipt = receipt(message),
            )
            val once = if (inDirect && !message.ephemeral) direct?.viewOnce?.entry(message.id) else null
            if (once != null) {
                base.copy(text = "", viewOnce = once, deletable = false)
            } else if (!rich || message.ephemeral) {
                base
            } else {
                base.copy(
                    text = extras.text(message),
                    edited = extras.isEdited(message.id),
                    editable = message.self && message.text.isNotEmpty(),
                    reactions = extras.reactions(message.id),
                    stickers = extras.stickers(message.id),
                    thread = if (message.conversation.startsWith("#") || inDirect) extras.threadSummary(message.id) else null,
                    attachment = extras.attachment(message.id),
                )
            }
        }
    }

    fun threadRows(rootId: String): List<ChatRow> {
        val list = extras.replies(rootId).filter { !extras.isHidden(it.id) }
        return list.mapIndexed { index, message ->
            ChatRow(
                message = message,
                continuesGroup = continuesGroup(list.getOrNull(index - 1), message),
                deletable = message.self,
                text = extras.text(message),
                edited = extras.isEdited(message.id),
                editable = message.self && message.text.isNotEmpty(),
                reactions = extras.reactions(message.id),
                stickers = extras.stickers(message.id),
                attachment = extras.attachment(message.id),
                receipt = receipt(message),
            )
        }
    }

    fun rootMessage(rootId: String, channel: String): ChatMessage? = messages(channel).firstOrNull { it.id == rootId }

    fun canReplyInThread(conversation: String): Boolean =
        (conversation.startsWith("#") && conversation != MESH_CONVERSATION) || DirectMessageFrames.isDirect(conversation)

    fun threadTitle(conversation: String): String {
        if (!DirectMessageFrames.isDirect(conversation)) return "Thread in $conversation"
        val other = direct?.record(conversation)?.otherUser.orEmpty()
        return if (other.isEmpty()) "Thread" else "Thread with $other"
    }

    val canSetTopic: Boolean get() = office?.room(relay.currentConversation)?.canManage == true

    fun setTopic(topic: String): Boolean {
        val service = office
        val room = service?.room(relay.currentConversation)
        if (service == null || room == null) {
            notice("/topic is available in managed Office channels.")
            return false
        }
        if (!room.canManage) {
            notice("Only the channel creator or an administrator can change this channel topic.")
            return false
        }
        return try {
            service.update(room, topic = topic)
            true
        } catch (error: Exception) {
            notice(error.message ?: error.toString())
            false
        }
    }

    fun sendText(conversation: String, text: String): ChatHubSend {
        if (text.isBlank()) return ChatHubSend.Empty
        when (val slash = ChatComposerTools.parseSlash(text)) {
            is ChatSlash.Issue -> return ChatHubSend.Issue(slash.argument)
            is ChatSlash.Topic -> return if (setTopic(slash.topic)) ChatHubSend.Sent else ChatHubSend.Failed("")
            null -> {}
        }
        apps?.let { if (it.handle(text, conversation, appChannelId(conversation))) return ChatHubSend.Sent }
        if (OfficeFrames.isOffice(conversation)) {
            val service = office
            if (isReadOnly(conversation)) return ChatHubSend.Failed("$conversation is read-only.")
            val sentId = service?.sendFrame(conversation, text).orEmpty()
            if (service == null || sentId.isEmpty()) {
                return ChatHubSend.Failed("$conversation is not writable right now.")
            }
            val channelId = appChannelId(conversation)
            if (channelId.isNotEmpty()) apps?.forwardMentions(text.trim(), channelId, sentId)
            maybeAskForkbot(conversation, text)
            return ChatHubSend.Sent
        }
        if (DirectMessageFrames.isDirect(conversation)) {
            val service = direct
            if (service == null || !service.send(conversation, text)) {
                return ChatHubSend.Failed("The direct message could not be sent. Reconnecting...")
            }
            return ChatHubSend.Sent
        }
        extras.sendTyping(conversation, false)
        val sent = relay.sendMessage(text, conversation)
        if (!sent) {
            relay.connect()
            return ChatHubSend.Failed(OFFLINE_NOTICE)
        }
        if (conversation.startsWith("#")) maybeAskForkbot(conversation, text)
        return ChatHubSend.Sent
    }

    fun appChannelId(conversation: String): String =
        office?.room(conversation)?.takeIf { !it.isPrivate }?.id.orEmpty()

    fun appAction(message: ChatMessage, actionId: String, value: String): AppReply? {
        val service = apps ?: return null
        val channelId = if (message.ephemeral) "" else appChannelId(message.conversation)
        return service.interact(message, actionId, value, channelId)
    }

    fun replyInThread(channel: String, rootId: String, text: String): Boolean {
        if (DirectMessageFrames.isDirect(channel)) {
            val sent = direct?.sendThreadReply(channel, rootId, text) == true
            if (!sent) notice("The reply could not be sent. Reconnecting...")
            return sent
        }
        if (!relay.connected) {
            notice(OFFLINE_NOTICE)
            relay.connect()
            return false
        }
        val sent = extras.sendThreadReply(channel, rootId, text)
        if (sent) maybeAskForkbot(channel, text, rootId)
        return sent
    }

    fun toggleReaction(message: ChatMessage, value: String) {
        if (DirectMessageFrames.isDirect(message.conversation)) {
            val added = !extras.hasReacted(message.id, value)
            if (direct?.react(message.conversation, message.id, ReactionCatalog.emoji(value), added) != true) notice("The reaction could not be sent.")
            return
        }
        if (!extras.toggleReaction(message.conversation, message.id, value)) notice(OFFLINE_NOTICE)
    }

    fun canSticker(conversation: String): Boolean = StickerCatalog.canOffer(conversation)

    fun toggleSticker(message: ChatMessage, slug: String) {
        val entry = StickerCatalog.entry(slug) ?: return
        if (!canSticker(message.conversation)) return
        if (DirectMessageFrames.isDirect(message.conversation)) {
            val added = !extras.hasStuck(message.id, slug)
            if (direct?.react(message.conversation, message.id, entry.fallback, added, slug) != true) notice("The sticker could not be sent.")
            return
        }
        if (!extras.toggleSticker(message.conversation, message.id, slug)) notice(OFFLINE_NOTICE)
    }

    fun edit(message: ChatMessage, text: String): Boolean {
        val sent = if (DirectMessageFrames.isDirect(message.conversation)) {
            message.self && direct?.edit(message.conversation, message.id, text) == true
        } else {
            extras.edit(message.conversation, message.id, text)
        }
        if (!sent) notice("The message could not be changed.")
        return sent
    }

    fun deleteMessage(message: ChatMessage): Boolean {
        if (DirectMessageFrames.isDirect(message.conversation)) {
            val ok = message.self && direct?.deleteMessage(message) == true
            if (!ok) notice("The message could not be deleted.")
            return ok
        }
        if (extras.isThreadReply(message.id)) {
            val ok = extras.deleteReply(message)
            if (!ok) notice("The reply could not be deleted.")
            return ok
        }
        return relay.deleteMessage(message)
    }

    fun hideMessage(message: ChatMessage) = extras.hideMessage(message.id)

    fun composerChanged(text: String) {
        val conversation = relay.currentConversation
        if (conversation == MESH_CONVERSATION || OfficeFrames.isOffice(conversation) ||
            DirectMessageFrames.isDirect(conversation)) return
        extras.sendTyping(conversation, text.isNotBlank())
    }

    fun typingLine(conversation: String): String = ChatExtras.typingLabel(extras.typingNames(conversation))

    fun mentionNames(conversation: String): List<String> {
        val names = ArrayList<String>()
        for (group in relay.rosterGroups()) if (!group.self) names.add(group.name)
        for (message in messages(conversation).takeLast(200)) if (!message.self) names.add(message.senderName)
        office?.room(conversation)?.let { names.addAll(it.members) }
        return names
    }

    fun isPrivateRoom(conversation: String): Boolean {
        if (conversation.startsWith("@") || DirectMessageFrames.isDirect(conversation)) return true
        return office?.room(conversation)?.isPrivate == true
    }

    fun isConversation(conversation: String): Boolean {
        if (conversation.startsWith("@") || DirectMessageFrames.isDirect(conversation)) return true
        return office?.room(conversation)?.let { it.isPrivate && !it.serverVisible && !it.readOnly } == true
    }

    fun isGroupConversation(conversation: String): Boolean =
        isConversation(conversation) && !conversation.startsWith("@") && !DirectMessageFrames.isDirect(conversation)

    fun conversationItems(conversation: String): List<ChatConversationItem> =
        ChatConversationLayout.items(rows(conversation), isGroupConversation(conversation))

    fun isServerVisible(conversation: String): Boolean = office?.room(conversation)?.serverVisible == true

    fun isReadOnly(conversation: String): Boolean = office?.room(conversation)?.readOnly == true

    fun sealSummary(conversation: String): String {
        if (conversation == MESH_CONVERSATION) return MESH_SEAL_SUMMARY
        if (isServerVisible(conversation)) return SERVER_VISIBLE_SEAL_SUMMARY
        return if (isPrivateRoom(conversation)) PRIVATE_SEAL_SUMMARY else PUBLIC_SEAL_SUMMARY
    }

    fun serverCopiesLine(conversation: String): String {
        if (!DirectMessageFrames.isDirect(conversation)) return ""
        val stored = direct?.serverCopies(conversation) ?: return ""
        return DirectMessageFrames.serverCopiesLabel(stored)
    }

    fun badge(conversation: String): String = when {
        isServerVisible(conversation) -> "server-visible"
        isPrivateRoom(conversation) -> "end-to-end"
        else -> "public"
    }

    fun canAskForkbot(conversation: String): Boolean {
        if (!conversation.startsWith("#") || conversation == MESH_CONVERSATION) return false
        office?.room(conversation)?.let { return !it.isPrivate }
        return true
    }

    fun maybeAskForkbot(conversation: String, text: String, threadRootId: String = "") {
        val api = forkbot ?: return
        if (!ChatComposerTools.mentionsForkbot(text)) return
        if (!canAskForkbot(conversation)) {
            notice(FORKBOT_PRIVATE_NOTICE)
            return
        }
        val replies = if (threadRootId.isEmpty()) emptyList() else extras.replies(threadRootId)
        val context = ChatComposerTools.forkbotContext(messages(conversation), threadRootId, replies)
        try {
            val answer = api.accountApiCall("POST", "/api/forkbot/chat", ChatComposerTools.forkbotPayload(text, selfName(), context))
            val bot = jsonText(answer["botMessage"]).trim()
            if (bot.isEmpty()) return
            if (OfficeFrames.isOffice(conversation)) office?.send(conversation, bot, asBot = true)
            else extras.sendBotReply(conversation, threadRootId, bot)
        } catch (_: Exception) {
            notice("ForkBot did not answer.")
        }
    }

    fun sendAttachment(conversation: String, fileName: String, mime: String, data: ByteArray): Boolean {
        if (conversation == MESH_CONVERSATION || OfficeFrames.isOffice(conversation)) return false
        if (DirectMessageFrames.isDirect(conversation)) return sendDirectFile(conversation, fileName, mime, data)
        if (data.size > ChatLimits.MAX_SEND_FILE_BYTES) {
            notice("Files up to 2 MB can be sent from this phone.")
            return false
        }
        val sent = extras.sendFile(conversation, fileName, mime, data)
        if (!sent) notice("The file could not be sent.")
        return sent
    }

    fun readMarkKey(conversation: String): String? = when {
        DirectMessageFrames.isDirect(conversation) -> conversation
        OfficeFrames.isOffice(conversation) -> office?.room(conversation)?.let { "private:${it.id}" }
        ChatReadMarks.isSyncable(conversation) -> conversation
        else -> null
    }

    fun isReadElsewhere(message: ChatMessage): Boolean {
        val marks = readMarks ?: return false
        val key = readMarkKey(message.conversation) ?: return false
        return marks.isRead(key, message.timestamp)
    }

    fun queueReadMark(conversation: String) {
        val marks = readMarks ?: return
        val key = readMarkKey(conversation) ?: return
        val newest = messages(conversation).lastOrNull()?.timestamp ?: return
        marks.queue(key, newest)
    }

    fun applyReadMarks() {
        val marks = readMarks ?: return
        if (!marks.fetch()) return
        val unread = relay.unreadConversations +
            direct?.records.orEmpty().filter { it.unreadCount > 0 }.map { it.conversation }
        for (conversation in unread) {
            val key = readMarkKey(conversation) ?: continue
            val newest = messages(conversation).lastOrNull()?.timestamp ?: continue
            if (!marks.isRead(key, newest)) continue
            if (DirectMessageFrames.isDirect(conversation)) direct?.clearUnread(conversation)
            else relay.markConversationRead(conversation)
        }
    }

    fun markAllRead() {
        for (conversation in relay.unreadConversations) queueReadMark(conversation)
        relay.markAllRead()
    }

    private fun sendDirectFile(conversation: String, fileName: String, mime: String, data: ByteArray): Boolean {
        val service = direct ?: return false
        if (data.size > DirectMessageFrames.MAX_FILE_BYTES) {
            notice("Direct messages take files up to 1 MB.")
            return false
        }
        val sent = service.sendFile(conversation, fileName, mime, data)
        if (!sent) notice("The file could not be sent.")
        return sent
    }

    fun select(conversation: String) {
        extras.sendTyping(relay.currentConversation, false)
        relay.switchConversation(conversation)
        queueReadMark(conversation)
        if (pings?.markConversationRead(conversation) == true) onPingsChanged?.invoke()
        if (OfficeFrames.isOffice(conversation)) {
            office?.let {
                it.loadHistory(conversation)
                it.open(conversation)
            }
            queueReadMark(conversation)
        }
        val service = direct ?: return
        service.focus(conversation)
        if (DirectMessageFrames.isDirect(conversation)) {
            if (service.record(conversation) == null) service.refresh()
            service.open(conversation)
            service.markRead(conversation)
            queueReadMark(conversation)
            service.refreshViewOnce(conversation)
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
        val service = direct
        if (service == null || !DirectMessageFrames.isDirect(conversation)) {
            notice("View-once messages can only be sent in direct messages.")
            return false
        }
        return try {
            val sent = service.sendViewOnce(conversation, kind, mime, durationMs, data, onProgress)
            if (!sent) notice("The message could not be sent. Reconnecting...")
            sent
        } catch (error: ViewOnceException) {
            notice(error.message ?: "The message could not be sent.")
            false
        } catch (error: Exception) {
            notice("The message could not be sent.")
            false
        }
    }

    fun resolve(conversation: String): String? = try {
        if (DirectMessageFrames.isDirect(conversation)) conversation
        else ChatReplyTarget.resolve(conversation, relay.channels, relay.roster(), officeChannels)
    } catch (_: ChatReplyException) {
        null
    }

    fun accountRecipient(conversation: String): String? {
        if (!conversation.startsWith("@")) return null
        val name = conversation.drop(1).trim()
        if (name.isEmpty()) return null
        val others = relay.roster().filter { !it.self }
        others.firstOrNull { it.id == name }?.let { return it.owner.trim().lowercase().ifEmpty { null } }
        val folded = name.lowercase()
        val matches = others.filter { it.owner.lowercase() == folded || it.name.lowercase() == folded || it.nodeName.lowercase() == folded }
        if (matches.isEmpty()) return folded
        val peer = matches.firstOrNull { it.online && it.owner.isNotEmpty() } ?: matches.firstOrNull { it.owner.isNotEmpty() }
        return peer?.owner?.trim()?.lowercase()?.ifEmpty { null }
    }

    fun messageAccount(member: Member): String? = if (member.self) null else member.owner.trim().lowercase().ifEmpty { null }

    fun directConversation(account: String): String? {
        val service = direct ?: return null
        val name = account.trim().lowercase()
        if (name.isEmpty()) return null
        service.records.firstOrNull { it.otherUser == name }?.let { return it.conversation }
        return service.start(name).conversation
    }

    fun switchConversation(conversation: String) {
        revealRequests++
        if (conversation.startsWith("@")) {
            val account = accountRecipient(conversation)
            if (account == null) {
                notice(GUEST_DIRECT_NOTICE)
            } else {
                try {
                    val target = directConversation(account)
                    if (target == null) notice("Sign in to send direct messages.") else select(target)
                } catch (error: Exception) {
                    notice(error.message ?: "The direct message could not be opened.")
                }
            }
        } else {
            select(resolve(conversation) ?: conversation)
        }
        onReveal?.invoke()
    }

    fun reply(channel: String, text: String) {
        val body = text.trim()
        if (body.isEmpty()) throw ChatReplyException(ChatReplyException.Kind.REFUSED)
        val conversation = ChatReplyTarget.resolve(channel, relay.channels, relay.roster(), officeChannels)
        if (conversation.startsWith("@")) {
            val account = accountRecipient(conversation) ?: throw ChatReplyException(ChatReplyException.Kind.REFUSED)
            val target = try {
                directConversation(account)
            } catch (_: Exception) {
                null
            } ?: throw ChatReplyException(ChatReplyException.Kind.REFUSED)
            if (direct?.send(target, body) != true) throw ChatReplyException(ChatReplyException.Kind.REFUSED)
            return
        }
        if (OfficeFrames.isOffice(conversation)) {
            if (office?.send(conversation, body) != true) throw ChatReplyException(ChatReplyException.Kind.REFUSED)
            return
        }
        if (!relay.connected) throw ChatReplyException(ChatReplyException.Kind.OFFLINE)
        if (!extras.sendText(conversation, body)) throw ChatReplyException(ChatReplyException.Kind.REFUSED)
    }
}
