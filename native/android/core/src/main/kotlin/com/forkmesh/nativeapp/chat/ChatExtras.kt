package com.forkmesh.nativeapp.chat

import com.forkmesh.nativeapp.json.jsonMaps
import com.forkmesh.nativeapp.json.jsonText
import com.forkmesh.nativeapp.model.ChatMessage
import com.forkmesh.nativeapp.model.ChatRetention
import java.util.Base64
import java.util.UUID

enum class ChatAttachmentKind {
    IMAGE, AUDIO, VIDEO, FILE;

    companion object {
        private val imageExt = setOf("png", "jpg", "jpeg", "gif", "heic", "webp", "bmp")
        private val audioExt = setOf("mp3", "m4a", "aac", "wav", "ogg", "opus", "flac", "caf")
        private val videoExt = setOf("mp4", "mov", "m4v", "webm")

        fun of(fileName: String, mime: String): ChatAttachmentKind {
            val type = mime.lowercase()
            if (type.startsWith("image/")) return IMAGE
            if (type.startsWith("audio/")) return AUDIO
            if (type.startsWith("video/")) return VIDEO
            val ext = fileName.substringAfterLast('.', "").lowercase()
            if (ext in imageExt) return IMAGE
            if (ext in audioExt) return AUDIO
            if (ext in videoExt) return VIDEO
            return FILE
        }
    }
}

class ChatAttachment(val messageId: String, val fileName: String, val mime: String, val data: ByteArray) {
    val kind: ChatAttachmentKind get() = ChatAttachmentKind.of(fileName, mime)

    override fun equals(other: Any?): Boolean =
        other is ChatAttachment && other.messageId == messageId && other.fileName == fileName && other.mime == mime &&
            other.data.contentEquals(data)

    override fun hashCode(): Int = messageId.hashCode() * 31 + data.contentHashCode()
}

data class ChatThreadSummary(
    val rootId: String,
    val replyCount: Int,
    val latestReplier: String,
    val latestAt: Long,
    val faces: List<String>,
) {
    val label: String get() = if (replyCount == 1) "1 reply" else "$replyCount replies"
}

data class ReactionChip(val value: String, val count: Int, val names: List<String>, val mine: Boolean) {
    val emoji: String get() = ReactionCatalog.emoji(value)
    val label: String get() = ReactionCatalog.label(value)
}

data class ChatTypingPeer(val name: String, val untilMs: Long)

class ChatExtras(private val now: () -> Long = System::currentTimeMillis) {
    companion object {
        const val MAX_REACTIONS_PER_MESSAGE = 32
        const val MAX_REACTORS_PER_REACTION = 500
        const val MAX_REPLIES_PER_THREAD = 500

        fun typingLabel(names: List<String>): String = when (names.size) {
            0 -> ""
            1 -> "${names[0]} is typing..."
            2 -> "${names[0]} and ${names[1]} are typing..."
            else -> "${names.size} people are typing..."
        }

        fun normalizedChannel(raw: String): String {
            val trimmed = raw.trim()
            if (trimmed.isEmpty() || trimmed == "#") return ""
            return if (trimmed.startsWith("#")) trimmed else "#$trimmed"
        }

        fun validChannelName(raw: String): Boolean {
            val name = normalizedChannel(raw)
            if (name.length < 2 || name.length > 41 || name == "#team") return false
            return name.drop(1).all { it.isLetterOrDigit() || it == '-' || it == '_' }
        }
    }

    private val lock = Any()

    @Volatile
    var sink: ChatFrameSink? = null

    @Volatile
    var authorOf: ((String, String) -> String?)? = null

    @Volatile
    var onChannelJoined: ((String) -> Unit)? = null

    @Volatile
    var onChannelRemoved: ((String) -> Unit)? = null

    @Volatile
    var onChange: (() -> Unit)? = null

    @Volatile
    var retention: ChatRetention? = null

    private val lastTypingSent = HashMap<String, Long>()
    private var revisionValue = 0
    private val privateSet = LinkedHashSet<String>()
    private val hiddenSet = LinkedHashSet<String>()
    private var notice: String? = null
    private val replies = HashMap<String, List<ChatMessage>>()
    private val replyRoot = HashMap<String, String>()
    private val rootChannel = HashMap<String, String>()
    private val reactionState = HashMap<String, LinkedHashMap<String, LinkedHashMap<String, String>>>()
    private val reactionOrder = HashMap<String, List<String>>()
    private val editedText = HashMap<String, String>()
    private val typingState = HashMap<String, LinkedHashMap<String, ChatTypingPeer>>()
    private val files = HashMap<String, ChatAttachment>()
    private val fileOrder = ArrayList<String>()
    private var fileBytes = 0
    private val hiddenMessageSet = LinkedHashSet<String>()

    private val selfId: String get() = sink?.selfId ?: ""

    val revision: Int get() = synchronized(lock) { revisionValue }

    val privateChannels: Set<String> get() = synchronized(lock) { LinkedHashSet(privateSet) }

    val hiddenChannels: Set<String> get() = synchronized(lock) { LinkedHashSet(hiddenSet) }

    val inviteNotice: String? get() = synchronized(lock) { notice }

    private inner class Batch {
        var changed = false
        val after = ArrayList<() -> Unit>()

        fun changed() {
            changed = true
        }
    }

    private inline fun <T> batch(block: Batch.() -> T): T {
        val b = Batch()
        val result = synchronized(lock) {
            val r = b.block()
            if (b.changed) revisionValue++
            r
        }
        for (action in b.after) action()
        if (b.changed) onChange?.invoke()
        return result
    }

    fun clearInviteNotice() = synchronized(lock) { notice = null }

    fun replies(rootId: String): List<ChatMessage> = synchronized(lock) { replies[rootId].orEmpty() }

    fun threadSummary(rootId: String): ChatThreadSummary? = synchronized(lock) {
        val list = replies[rootId].orEmpty().filter { it.id !in hiddenMessageSet }
        val last = list.lastOrNull() ?: return@synchronized null
        val faces = ArrayList<String>()
        for (message in list.asReversed()) {
            if (message.senderName in faces) continue
            faces.add(message.senderName)
            if (faces.size == 3) break
        }
        ChatThreadSummary(rootId, list.size, last.senderName, last.timestamp, faces)
    }

    fun isThreadReply(id: String): Boolean = synchronized(lock) { replyRoot.containsKey(id) }

    fun isHidden(id: String): Boolean = synchronized(lock) { id in hiddenMessageSet }

    fun hideMessage(id: String) = restoreHiddenMessages(listOf(id))

    fun snapshotHiddenMessages(): List<String> = synchronized(lock) { hiddenMessageSet.toList() }

    fun restoreHiddenMessages(ids: List<String>) = batch {
        for (id in ids) if (id.isNotEmpty() && hiddenMessageSet.add(id)) changed()
        val excess = hiddenMessageSet.size - ChatLimits.HIDDEN_MESSAGES
        if (excess > 0) hiddenMessageSet.removeAll(hiddenMessageSet.take(excess).toSet())
    }

    fun rootOfReply(id: String): String? = synchronized(lock) { replyRoot[id] }

    fun replyAuthorOf(id: String): String? = replyAuthor(id).second

    fun hasReacted(messageId: String, value: String): Boolean {
        val me = selfId
        return me.isNotEmpty() && synchronized(lock) { reactionState[messageId]?.get(value)?.containsKey(me) == true }
    }

    fun text(message: ChatMessage): String = synchronized(lock) { editedText[message.id] ?: message.text }

    fun isEdited(id: String): Boolean = synchronized(lock) { editedText.containsKey(id) }

    fun reactions(messageId: String): List<ReactionChip> = synchronized(lock) {
        val state = reactionState[messageId] ?: return@synchronized emptyList()
        val me = selfId
        reactionOrder[messageId].orEmpty().mapNotNull { value ->
            val reactors = state[value]
            if (reactors.isNullOrEmpty()) return@mapNotNull null
            val names = reactors.keys.sorted().map { reactors[it] ?: "" }.filter { it.isNotEmpty() }
            ReactionChip(value, reactors.size, names, reactors.containsKey(me))
        }
    }

    fun attachment(messageId: String): ChatAttachment? = synchronized(lock) { files[messageId] }

    fun typingNames(conversation: String): List<String> = synchronized(lock) {
        val at = now()
        val peers = typingState[conversation] ?: return@synchronized emptyList()
        val names = ArrayList<String>()
        for (key in peers.keys.sorted()) {
            val peer = peers[key] ?: continue
            if (peer.untilMs <= at) continue
            if (peer.name !in names) names.add(peer.name)
        }
        names
    }

    fun isPrivate(channel: String): Boolean = synchronized(lock) { channel in privateSet }

    fun apply(frame: Map<String, Any?>) {
        val type = jsonText(frame["type"])
        val senderId = jsonText(frame["senderId"])
        val conversation = ChatFrameText.conversation(frame)
        if (conversation == "#team") return
        when (type) {
            "chat", "dm" -> batch {
                rememberFile(frame)
                if (frame["private"] == true && conversation.startsWith("#") && conversation !in hiddenSet) {
                    if (privateSet.add(conversation)) changed()
                }
            }
            "thread-reply" -> batch { applyReply(frame, senderId) }
            "reaction" -> batch { applyReaction(frame, senderId) }
            "edit" -> applyEdit(frame, senderId, conversation)
            "delete" -> applyDelete(frame, senderId)
            "typing" -> batch { applyTyping(frame, senderId, conversation) }
            "invite" -> batch { applyInvite(frame) }
            "channel" -> {
                val name = normalizedChannel(jsonText(frame["name"]))
                val joined = synchronized(lock) { name.isNotEmpty() && name != "#team" && name !in hiddenSet }
                if (joined) onChannelJoined?.invoke(name)
            }
            "history" -> applyHistory(frame)
        }
    }

    private fun Batch.applyReply(frame: Map<String, Any?>, senderId: String) {
        val rootId = jsonText(frame["rootId"]).trim().take(ChatLimits.MAX_ROOT_ID_CHARS)
        val channel = jsonText(frame["channel"])
        val id = jsonText(frame["id"])
        if (rootId.isEmpty() || id.isEmpty() || channel in hiddenSet) return
        if (!channel.startsWith("#") && !DirectMessageFrames.isDirect(channel)) return
        if (replyRoot.containsKey(id)) return
        val ts = frame["ts"]
        val millis = if (ts is Number) ts.toLong() else now()
        if (retention?.isExpired(millis, channel, now()) == true) return
        val message = ChatMessage(
            id = id,
            conversation = channel,
            senderId = senderId,
            senderName = ChatFrameText.bounded(frame["sender"], ChatLimits.MAX_DISPLAY_NAME_CHARS),
            text = ChatFrameText.bounded(frame["text"], ChatLimits.MAX_TEXT_CHARS),
            timestamp = millis,
            self = senderId.isNotEmpty() && senderId == selfId,
            fileName = ChatFrameText.bounded(frame["fileName"], ChatLimits.MAX_FILE_NAME_CHARS),
        )
        val list = insertSorted(replies[rootId].orEmpty(), message)
        if (list.size > MAX_REPLIES_PER_THREAD) {
            val dropped = list.removeAt(0)
            replyRoot.remove(dropped.id)
        }
        replies[rootId] = list
        replyRoot[id] = rootId
        rootChannel[rootId] = channel
        if (frame["private"] == true) privateSet.add(channel)
        rememberFile(frame)
        changed()
    }

    private fun Batch.applyReaction(frame: Map<String, Any?>, senderId: String) {
        val target = jsonText(frame["target"])
        val raw = jsonText(frame["emoji"])
        val reactorId = frame["reactorId"]?.let(::jsonText) ?: senderId
        if (target.isEmpty() || raw.isEmpty() || raw.codePointCount(0, raw.length) > 16 || reactorId.isEmpty()) return
        if (senderId.isNotEmpty() && reactorId != senderId) return
        val value = ReactionCatalog.valueForEmoji(raw)
        val name = (frame["reactorName"] ?: frame["sender"])?.let(::jsonText).orEmpty().take(ChatLimits.MAX_DISPLAY_NAME_CHARS)
        val state = reactionState[target] ?: LinkedHashMap()
        val order = ArrayList(reactionOrder[target].orEmpty())
        val reactors = state[value] ?: LinkedHashMap()
        if (frame["added"] == true) {
            if (reactors.containsKey(reactorId)) return
            if (!state.containsKey(value) && state.size >= MAX_REACTIONS_PER_MESSAGE) return
            if (reactors.size >= MAX_REACTORS_PER_REACTION) return
            reactors[reactorId] = name
            if (value !in order) order.add(value)
        } else {
            if (reactors.remove(reactorId) == null) return
        }
        if (reactors.isEmpty()) state.remove(value) else state[value] = reactors
        val kept = order.filter { state.containsKey(it) }
        if (state.isEmpty()) reactionState.remove(target) else reactionState[target] = state
        if (kept.isEmpty()) reactionOrder.remove(target) else reactionOrder[target] = kept
        changed()
    }

    private fun replyAuthor(target: String): Pair<Boolean, String?> = synchronized(lock) {
        val root = replyRoot[target] ?: return@synchronized false to null
        true to replies[root]?.firstOrNull { it.id == target }?.senderId
    }

    private fun author(target: String, conversation: String): String? {
        val (isReply, author) = replyAuthor(target)
        if (isReply) return author
        return authorOf?.invoke(conversation, target)
    }

    private fun applyEdit(frame: Map<String, Any?>, senderId: String, conversation: String) {
        val target = jsonText(frame["target"])
        val text = ChatFrameText.bounded(frame["text"], ChatLimits.MAX_TEXT_CHARS).trim()
        if (target.isEmpty() || text.isEmpty() || senderId.isEmpty()) return
        val author = author(target, conversation) ?: return
        if (author != senderId) return
        batch {
            editedText[target] = text
            changed()
        }
    }

    private fun applyDelete(frame: Map<String, Any?>, senderId: String) {
        val target = jsonText(frame["target"])
        if (target.isEmpty()) return
        val (isReply, replyAuthor) = replyAuthor(target)
        if (!isReply) {
            val author = authorOf?.invoke(ChatFrameText.conversation(frame), target)
            if (author != null && author != senderId) return
        } else if (replyAuthor != senderId) {
            return
        }
        batch {
            val root = replyRoot[target]
            if (root != null) {
                val list = replies[root].orEmpty().filter { it.id != target }
                if (list.isEmpty()) replies.remove(root) else replies[root] = list
                replyRoot.remove(target)
            }
            forget(target)
            changed()
        }
    }

    private fun forget(id: String) {
        editedText.remove(id)
        reactionState.remove(id)
        reactionOrder.remove(id)
        val file = files.remove(id)
        if (file != null) {
            fileBytes -= file.data.size
            fileOrder.remove(id)
        }
    }

    private fun Batch.applyTyping(frame: Map<String, Any?>, senderId: String, conversation: String) {
        if (senderId.isEmpty() || senderId == selfId || conversation.isEmpty()) return
        val target = if (conversation.startsWith("@")) "@$senderId" else conversation
        val peers = typingState[target] ?: LinkedHashMap()
        if (frame["active"] == true) {
            val name = jsonText(frame["sender"]).take(ChatLimits.MAX_DISPLAY_NAME_CHARS)
            peers[senderId] = ChatTypingPeer(name.ifEmpty { "Someone" }, now() + ChatLimits.TYPING_TIMEOUT_MS)
        } else if (peers.remove(senderId) == null) {
            return
        }
        if (peers.isEmpty()) typingState.remove(target) else typingState[target] = peers
        changed()
    }

    private fun Batch.applyInvite(frame: Map<String, Any?>) {
        val me = selfId
        if (jsonText(frame["to"]) != me || me.isEmpty()) return
        val channel = normalizedChannel(jsonText(frame["channel"]).take(120))
        if (channel.isEmpty() || channel == "#team" || channel in hiddenSet) return
        privateSet.add(channel)
        after.add { onChannelJoined?.invoke(channel) }
        val sender = frame["sender"]?.let(::jsonText) ?: "Someone"
        notice = "$sender invited you to private room $channel."
        changed()
    }

    private fun applyHistory(frame: Map<String, Any?>) {
        val to = jsonText(frame["to"])
        if (to.isNotEmpty() && to != selfId) return
        val target = sink ?: return
        val accepted = synchronized(lock) {
            jsonMaps(frame["entries"]).take(ChatLimits.HISTORY_ENTRIES_PER_CHANNEL).filter { entry ->
                val type = jsonText(entry["type"])
                if (type != "chat" && type != "thread-reply") return@filter false
                val channel = jsonText(entry["channel"])
                if (!channel.startsWith("#") || channel == "#team" || channel in hiddenSet) return@filter false
                !(entry["private"] == true && channel !in privateSet)
            }
        }
        for (entry in accepted) target.applyFrame(entry)
    }

    private fun Batch.rememberFile(frame: Map<String, Any?>) {
        val id = jsonText(frame["id"])
        val name = ChatFrameText.bounded(frame["fileName"], ChatLimits.MAX_FILE_NAME_CHARS)
        if (id.isEmpty() || name.isEmpty() || files.containsKey(id)) return
        val encoded = jsonText(frame["file"])
        if (encoded.isEmpty() || encoded.length > (ChatLimits.MAX_FILE_BYTES / 3 + 1) * 4) return
        val data = try {
            Base64.getDecoder().decode(encoded)
        } catch (_: IllegalArgumentException) {
            return
        }
        if (data.isEmpty() || data.size > ChatLimits.MAX_FILE_BYTES) return
        storeLocked(ChatAttachment(id, name, ChatFrameText.bounded(frame["fileMime"], ChatLimits.MAX_MIME_CHARS), data))
    }

    private fun Batch.storeLocked(attachment: ChatAttachment) {
        if (files.containsKey(attachment.messageId)) return
        files[attachment.messageId] = attachment
        fileOrder.add(attachment.messageId)
        fileBytes += attachment.data.size
        while (fileBytes > ChatLimits.ATTACHMENT_BUDGET_BYTES && fileOrder.size > 1) {
            val oldest = fileOrder.removeAt(0)
            val dropped = files.remove(oldest)
            if (dropped != null) fileBytes -= dropped.data.size
        }
        changed()
    }

    fun store(attachment: ChatAttachment) = batch { storeLocked(attachment) }

    fun createPrivateChannel(raw: String): String? {
        if (!validChannelName(raw)) return null
        val name = normalizedChannel(raw).lowercase()
        return batch {
            hiddenSet.remove(name)
            privateSet.add(name)
            after.add { onChannelJoined?.invoke(name) }
            changed()
            name
        }
    }

    fun addPublicChannel(raw: String): String? {
        val target = sink ?: return null
        if (!validChannelName(raw)) return null
        val name = normalizedChannel(raw).lowercase()
        batch {
            hiddenSet.remove(name)
            changed()
        }
        onChannelJoined?.invoke(name)
        val frame = target.makeFrame("channel")
        frame["name"] = name
        target.applyFrame(frame)
        target.sendFrame(frame)
        return name
    }

    fun removeChannel(channel: String) {
        if (channel == "#general") return
        batch {
            hiddenSet.add(channel)
            privateSet.remove(channel)
            for ((root, owner) in HashMap(rootChannel)) {
                if (owner != channel) continue
                for (reply in replies[root].orEmpty()) replyRoot.remove(reply.id)
                replies.remove(root)
                rootChannel.remove(root)
            }
            after.add { onChannelRemoved?.invoke(channel) }
            changed()
        }
    }

    fun restoreChannels(privateChannels: List<String>, hiddenChannels: List<String>) = batch {
        hiddenSet.addAll(hiddenChannels)
        for (channel in privateChannels) {
            if (channel in hiddenSet) continue
            privateSet.add(channel)
            after.add { onChannelJoined?.invoke(channel) }
        }
        changed()
    }

    private fun routed(frame: LinkedHashMap<String, Any?>, conversation: String, persist: Boolean, private: Boolean) {
        frame["conversation"] = conversation
        if (conversation.startsWith("#")) {
            frame["channel"] = conversation
            if (private) frame["private"] = true
            if (persist) frame["persist"] = true
        } else if (conversation.startsWith("@")) {
            frame["to"] = conversation.drop(1)
        }
    }

    private fun takeBack(target: ChatFrameSink, id: String, conversation: String) {
        val undo = target.makeFrame("delete")
        undo["conversation"] = conversation
        undo["target"] = id
        target.applyFrame(undo)
    }

    fun sendPrivateChat(channel: String, text: String): Boolean {
        val body = text.trim().take(ChatLimits.MAX_TEXT_CHARS)
        val target = sink ?: return false
        if (body.isEmpty() || !channel.startsWith("#")) return false
        val frame = target.makeFrame("chat")
        frame["text"] = body
        routed(frame, channel, persist = true, private = true)
        frame["private"] = true
        target.applyFrame(frame)
        if (target.sendFrame(frame)) return true
        takeBack(target, jsonText(frame["id"]), channel)
        return false
    }

    fun sendText(conversation: String, text: String): Boolean {
        if (isPrivate(conversation)) return sendPrivateChat(conversation, text)
        val target = sink ?: return false
        return target.sendMessage(text.take(ChatLimits.MAX_TEXT_CHARS), conversation)
    }

    fun sendBotReply(channel: String, rootId: String, text: String): Boolean {
        val body = text.trim().take(ChatLimits.MAX_TEXT_CHARS)
        val target = sink ?: return false
        if (body.isEmpty() || !channel.startsWith("#") || isPrivate(channel)) return false
        val root = rootId.take(ChatLimits.MAX_ROOT_ID_CHARS)
        val frame = linkedMapOf<String, Any?>(
            "type" to if (root.isEmpty()) "chat" else "thread-reply",
            "id" to UUID.randomUUID().toString().lowercase(),
            "senderId" to "forkbot",
            "sender" to "forkbot",
            "accountKind" to "user",
            "ts" to now(),
            "channel" to channel,
            "text" to body,
        )
        if (root.isNotEmpty()) frame["rootId"] = root
        target.applyFrame(frame)
        return target.sendFrame(frame)
    }

    fun sendThreadReply(channel: String, rootId: String, text: String): Boolean {
        val body = text.trim().take(ChatLimits.MAX_TEXT_CHARS)
        val root = rootId.trim().take(ChatLimits.MAX_ROOT_ID_CHARS)
        val target = sink ?: return false
        if (body.isEmpty() || root.isEmpty() || !channel.startsWith("#")) return false
        val frame = target.makeFrame("thread-reply")
        frame["rootId"] = root
        frame["text"] = body
        routed(frame, channel, persist = true, private = isPrivate(channel))
        target.applyFrame(frame)
        if (target.sendFrame(frame)) return true
        takeBack(target, jsonText(frame["id"]), channel)
        return false
    }

    fun toggleReaction(conversation: String, messageId: String, value: String): Boolean {
        val target = sink ?: return false
        if (messageId.isEmpty() || value.isEmpty()) return false
        val me = target.selfId
        val added = synchronized(lock) { reactionState[messageId]?.get(value)?.containsKey(me) != true }
        val frame = target.makeFrame("reaction")
        frame["target"] = messageId
        frame["emoji"] = value
        frame["reactorId"] = me
        frame["reactorName"] = jsonText(frame["sender"])
        frame["added"] = added
        routed(frame, conversation, persist = true, private = isPrivate(conversation))
        target.applyFrame(frame)
        if (target.sendFrame(frame)) return true
        val undo = target.makeFrame("reaction")
        for ((key, v) in frame) if (key != "id" && key != "ts") undo[key] = v
        undo["added"] = !added
        target.applyFrame(undo)
        return false
    }

    fun edit(conversation: String, messageId: String, text: String): Boolean {
        val body = text.trim().take(ChatLimits.MAX_TEXT_CHARS)
        val target = sink ?: return false
        if (body.isEmpty() || messageId.isEmpty()) return false
        val author = author(messageId, conversation) ?: return false
        if (author != target.selfId) return false
        val frame = target.makeFrame("edit")
        frame["target"] = messageId
        frame["text"] = body
        routed(frame, conversation, persist = true, private = isPrivate(conversation))
        val previous = synchronized(lock) { editedText[messageId] }
        target.applyFrame(frame)
        if (target.sendFrame(frame)) return true
        batch {
            if (previous == null) editedText.remove(messageId) else editedText[messageId] = previous
            changed()
        }
        return false
    }

    fun deleteReply(reply: ChatMessage): Boolean {
        val target = sink ?: return false
        if (reply.senderId != target.selfId || !isThreadReply(reply.id)) return false
        val frame = target.makeFrame("delete")
        frame["target"] = reply.id
        routed(frame, reply.conversation, persist = true, private = isPrivate(reply.conversation))
        target.applyFrame(frame)
        return target.sendFrame(frame)
    }

    fun sendTyping(conversation: String, active: Boolean): Boolean {
        val target = sink ?: return false
        if (!(conversation.startsWith("#") || conversation.startsWith("@"))) return false
        val at = now()
        val proceed = synchronized(lock) {
            if (active) {
                val last = lastTypingSent[conversation]
                if (last != null && at - last < ChatLimits.TYPING_RESEND_MS) return@synchronized false
                lastTypingSent[conversation] = at
                true
            } else {
                lastTypingSent.remove(conversation) != null
            }
        }
        if (!proceed) return false
        val frame = target.makeFrame("typing")
        frame["active"] = active
        routed(frame, conversation, persist = false, private = isPrivate(conversation))
        frame.remove("channel")
        return target.sendFrame(frame)
    }

    fun invite(peerId: String, channel: String): Boolean {
        val target = sink ?: return false
        if (peerId.isEmpty() || peerId == target.selfId || !isPrivate(channel) || channel == "#team") return false
        val frame = target.makeFrame("invite")
        frame["to"] = peerId
        frame["channel"] = channel
        return target.sendFrame(frame)
    }

    fun sendFile(conversation: String, fileName: String, mime: String, data: ByteArray): Boolean {
        val name = fileName.trim().take(ChatLimits.MAX_FILE_NAME_CHARS)
        val target = sink ?: return false
        if (name.isEmpty() || data.isEmpty() || data.size > ChatLimits.MAX_SEND_FILE_BYTES) return false
        val frame = target.makeFrame(if (conversation.startsWith("@")) "dm" else "chat")
        frame["fileName"] = name
        frame["fileMime"] = mime.take(ChatLimits.MAX_MIME_CHARS)
        frame["file"] = Base64.getEncoder().encodeToString(data)
        routed(frame, conversation, persist = true, private = isPrivate(conversation))
        if (frame["channel"] == null) frame.remove("persist")
        val id = jsonText(frame["id"])
        store(ChatAttachment(id, name, mime, data))
        target.applyFrame(frame)
        if (target.sendFrame(frame)) return true
        takeBack(target, id, conversation)
        batch {
            forget(id)
            changed()
        }
        return false
    }

    fun snapshotThreads(): List<ChatMessage> = synchronized(lock) { replies.values.flatten() }

    fun pruneExpired(rule: ChatRetention, nowMs: Long = now()): Boolean = batch {
        for ((root, list) in HashMap(replies)) {
            val (gone, kept) = list.partition { rule.isExpired(it.timestamp, it.conversation, nowMs) }
            if (gone.isEmpty()) continue
            for (reply in gone) replyRoot.remove(reply.id)
            if (kept.isEmpty()) {
                replies.remove(root)
                rootChannel.remove(root)
            } else {
                replies[root] = kept
            }
            changed()
        }
        changed
    }

    fun snapshotReplyRoots(): Map<String, String> = synchronized(lock) { HashMap(replyRoot) }

    fun snapshotReactions(): Map<String, Map<String, Map<String, String>>> = synchronized(lock) {
        reactionState.mapValues { (_, state) -> state.mapValues { LinkedHashMap(it.value) } }
    }

    fun snapshotReactionOrder(): Map<String, List<String>> = synchronized(lock) { HashMap(reactionOrder) }

    fun snapshotEdits(): Map<String, String> = synchronized(lock) { HashMap(editedText) }

    fun restore(
        replies: List<ChatMessage>,
        roots: Map<String, String>,
        reactions: Map<String, Map<String, Map<String, String>>>,
        order: Map<String, List<String>>,
        edits: Map<String, String>,
    ) = batch {
        for (reply in replies) {
            val root = roots[reply.id] ?: continue
            if (replyRoot.containsKey(reply.id) || reply.conversation in hiddenSet) continue
            this@ChatExtras.replies[root] = insertSorted(this@ChatExtras.replies[root].orEmpty(), reply)
            replyRoot[reply.id] = root
            rootChannel[root] = reply.conversation
        }
        for ((id, state) in reactions) {
            if (reactionState.containsKey(id)) continue
            val copy = LinkedHashMap<String, LinkedHashMap<String, String>>()
            for ((value, reactors) in state) copy[value] = LinkedHashMap(reactors)
            reactionState[id] = copy
            reactionOrder[id] = (order[id] ?: state.keys.sorted()).filter { copy.containsKey(it) }
        }
        for ((id, text) in edits) if (!editedText.containsKey(id)) editedText[id] = text
        changed()
    }
}
