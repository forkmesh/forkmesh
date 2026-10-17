package com.forkmesh.nativeapp.chat

import com.forkmesh.nativeapp.apps.AppReply
import com.forkmesh.nativeapp.apps.ChatApps
import com.forkmesh.nativeapp.inbox.LocalPingLog
import com.forkmesh.nativeapp.json.jsonText
import com.forkmesh.nativeapp.model.ChatMessage
import com.forkmesh.nativeapp.model.ChatReceipt
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
            "This channel is public and controller-readable. It still uses authenticated AES-256-GCM transport, " +
                "while allowing ForkBot, MCP, and incoming webhooks to read and post messages.\n\n" +
                "Private channels and direct messages add automatic device-to-device encryption beyond the controller key."
        const val PRIVATE_SEAL_SUMMARY =
            "Encryption is automatic. This device created a private X25519 key locally and sends only its public key. " +
                "Each message uses a fresh key wrapped for known room devices, so the controller's room key alone can forward " +
                "and retain it but cannot decrypt it.\n\nA new device can read messages addressed after its first visit; it " +
                "cannot retroactively open older history. ForkBot runs on the relay and is unavailable in end-to-end encrypted rooms."
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
            value?.onMessage = {
                filePing(it)
                if (relay.currentConversation == it.conversation) queueReadMark(it.conversation)
                else if (!isReadElsewhere(it)) relay.noteUnread(it.conversation)
            }
            field = value
        }

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
    var selfName: () -> String = { relay.roster().firstOrNull { it.self }?.name ?: "" }

    private var history: ChatHistoryStore? = null
    private var historyScope: () -> String = { "" }

    @Volatile
    private var loadedScope = ""

    init {
        extras.sink = relay
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

    fun attachHistory(store: ChatHistoryStore, scope: () -> String) {
        history = store
        historyScope = scope
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

    fun restoreHistoryIfNeeded() {
        val store = history ?: return
        val scope = historyScope()
        if (scope.isEmpty() || scope == loadedScope) return
        loadedScope = scope
        val snapshot = store.load(scope) ?: return
        val now = System.currentTimeMillis()
        extras.restoreChannels(snapshot.privateChannels, snapshot.hiddenChannels)
        relay.restoreHistory(snapshot.messages.filter { it.conversation !in snapshot.hiddenChannels && live(it, now) })
        extras.restore(snapshot.replies.filter { live(it, now) }, snapshot.replyRoots, snapshot.reactions, snapshot.reactionOrder, snapshot.edits)
        extras.restoreHiddenMessages(snapshot.hiddenMessages)
    }

    fun saveHistory() {
        val store = history ?: return
        restoreHistoryIfNeeded()
        val scope = loadedScope
        if (scope.isEmpty()) return
        store.save(historySnapshot(), scope)
    }

    fun historySnapshot(nowMs: Long = System.currentTimeMillis()): ChatHistorySnapshot {
        val messages = ChatHistoryCodec.bounded(relay.historySnapshot(), retention, nowMs)
        val kept = messages.map { it.id }.toSet()
        val replies = extras.snapshotThreads().filter { live(it, nowMs) }
        val replyIds = replies.map { it.id }.toSet()
        val known = kept + replyIds
        return ChatHistorySnapshot(
            messages = messages,
            replies = replies,
            replyRoots = extras.snapshotReplyRoots().filterKeys { it in replyIds },
            reactions = extras.snapshotReactions().filterKeys { it in known },
            reactionOrder = extras.snapshotReactionOrder().filterKeys { it in known },
            edits = extras.snapshotEdits().filterKeys { it in known },
            privateChannels = extras.privateChannels.sorted(),
            hiddenChannels = extras.hiddenChannels.sorted(),
            hiddenMessages = extras.snapshotHiddenMessages(),
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

    fun rows(conversation: String): List<ChatRow> {
        val stored = messages(conversation).filter { !extras.isHidden(it.id) }
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
        val sent = if (extras.isPrivate(conversation)) extras.sendPrivateChat(conversation, text) else relay.sendMessage(text, conversation)
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
        office?.room(conversation)?.let { return it.isPrivate }
        return extras.isPrivate(conversation)
    }

    fun isServerVisible(conversation: String): Boolean = office?.room(conversation)?.serverVisible == true

    fun isReadOnly(conversation: String): Boolean = office?.room(conversation)?.readOnly == true

    fun sealSummary(conversation: String): String {
        if (conversation == MESH_CONVERSATION) return ""
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
        return !extras.isPrivate(conversation)
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

    fun switchConversation(conversation: String) = select(resolve(conversation) ?: conversation)

    fun reply(channel: String, text: String) {
        val body = text.trim()
        if (body.isEmpty()) throw ChatReplyException(ChatReplyException.Kind.REFUSED)
        val conversation = ChatReplyTarget.resolve(channel, relay.channels, relay.roster(), officeChannels)
        if (OfficeFrames.isOffice(conversation)) {
            if (office?.send(conversation, body) != true) throw ChatReplyException(ChatReplyException.Kind.REFUSED)
            return
        }
        if (!relay.connected) throw ChatReplyException(ChatReplyException.Kind.OFFLINE)
        if (!extras.sendText(conversation, body)) throw ChatReplyException(ChatReplyException.Kind.REFUSED)
    }
}
