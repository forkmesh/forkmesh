package com.forkmesh.nativeapp.chat

import com.forkmesh.nativeapp.json.jsonText
import com.forkmesh.nativeapp.model.ChatMessage
import com.forkmesh.nativeapp.model.Member
import com.forkmesh.nativeapp.model.MemberGroup
import java.util.concurrent.atomic.AtomicLong

class FakeChatRelay(override val selfId: String = "me-node", var online: Boolean = true) : ChatRelay {
    val sent = ArrayList<Map<String, Any?>>()
    val channelList = arrayListOf("#general", "#random")
    val history = LinkedHashMap<String, List<ChatMessage>>()
    val peers = ArrayList<Member>()
    private val seen = HashSet<String>()
    private val seq = AtomicLong()
    var conversation = "#general"
    var observer: ((Map<String, Any?>) -> Unit)? = null
    val unread = LinkedHashMap<String, Int>()
    private var readElsewhere: ((ChatMessage) -> Boolean)? = null
    var connects = 0

    override val channels: List<String> get() = ArrayList(channelList)
    override val currentConversation: String get() = conversation
    override val connected: Boolean get() = online

    override fun makeFrame(type: String): LinkedHashMap<String, Any?> = linkedMapOf(
        "type" to type,
        "id" to "$selfId-${seq.incrementAndGet()}",
        "senderId" to selfId,
        "ts" to System.currentTimeMillis() + seq.get(),
        "sender" to "Me",
    )

    override fun sendFrame(frame: Map<String, Any?>): Boolean {
        if (!online) return false
        sent.add(LinkedHashMap(frame))
        return true
    }

    override fun applyFrame(frame: Map<String, Any?>) = handle(frame)

    fun handle(m: Map<String, Any?>) {
        val type = jsonText(m["type"])
        val senderId = jsonText(m["senderId"])
        if (type != "typing") {
            val id = jsonText(m["id"])
            if (id.isNotEmpty() && !seen.add(id)) return
        }
        when (type) {
            "chat" -> {
                val target = if (m["channel"] == null) "#general" else jsonText(m["channel"])
                if (target !in channelList) channelList.add(target)
                append(toMessage(m, target))
            }
            "dm" -> if (jsonText(m["to"]) == selfId) append(toMessage(m, "@$senderId"))
            "delete" -> {
                val target = jsonText(m["conversation"])
                val id = jsonText(m["target"])
                val original = history[target]?.firstOrNull { it.id == id }
                if (original != null && original.senderId == senderId) history[target] = history[target].orEmpty().filter { it.id != id }
            }
        }
        observer?.invoke(m)
    }

    private fun toMessage(m: Map<String, Any?>, conversation: String): ChatMessage {
        val senderId = jsonText(m["senderId"])
        val ts = m["ts"]
        return ChatMessage(
            id = jsonText(m["id"]),
            conversation = conversation,
            senderId = senderId,
            senderName = jsonText(m["sender"]),
            text = jsonText(m["text"]),
            timestamp = if (ts is Number) ts.toLong() else 0L,
            self = senderId == selfId,
            fileName = jsonText(m["fileName"]),
        )
    }

    private fun append(message: ChatMessage) {
        history[message.conversation] = insertSorted(history[message.conversation].orEmpty(), message)
        if (!message.self && message.conversation != conversation && readElsewhere?.invoke(message) != true) {
            unread[message.conversation] = (unread[message.conversation] ?: 0) + 1
        }
    }

    override val unreadConversations: List<String> get() = ArrayList(unread.keys)

    override fun markConversationRead(conversation: String) {
        unread.remove(conversation)
    }

    override fun markAllRead() = unread.clear()

    override fun setReadElsewhere(check: (ChatMessage) -> Boolean) {
        readElsewhere = check
    }

    override fun sendMessage(text: String, to: String): Boolean {
        val trimmed = text.trim()
        if (trimmed.isEmpty() || !(to.startsWith("#") || to.startsWith("@"))) return false
        val frame = makeFrame(if (to.startsWith("@")) "dm" else "chat")
        frame["text"] = trimmed
        frame["conversation"] = to
        if (to.startsWith("#")) {
            frame["channel"] = to
        } else {
            frame["to"] = to.drop(1)
        }
        if (!online) return false
        append(toMessage(frame, to))
        seen.add(jsonText(frame["id"]))
        sent.add(frame)
        return true
    }

    override fun messages(conversation: String): List<ChatMessage> = history[conversation].orEmpty()

    override fun roster(): List<Member> = listOf(Member(id = selfId, name = "Me", self = true, online = true)) + peers

    override fun rosterGroups(): List<MemberGroup> = roster().map { MemberGroup(it.name, listOf(it), self = it.self) }

    override fun switchConversation(conversation: String) {
        this.conversation = conversation
        unread.remove(conversation)
    }

    override fun addChannel(channel: String) {
        if (channel.isNotEmpty() && channel !in channelList) channelList.add(channel)
    }

    override fun removeChannel(channel: String) {
        channelList.remove(channel)
        history.remove(channel)
    }

    override fun historySnapshot(): Map<String, List<ChatMessage>> = LinkedHashMap(history)

    override fun restoreHistory(messages: List<ChatMessage>) {
        for (message in messages) {
            if (!seen.add(message.id)) continue
            append(message)
            if (message.conversation.startsWith("#")) addChannel(message.conversation)
        }
    }

    override fun deleteMessage(message: ChatMessage): Boolean {
        history[message.conversation] = history[message.conversation].orEmpty().filter { it.id != message.id }
        return online
    }

    override fun connect() {
        connects++
    }

    fun sentOfType(type: String) = sent.filter { jsonText(it["type"]) == type }
}

class TestClock(var ms: Long = 1_000_000L) {
    fun now(): Long = ms
}

fun hubWith(relay: FakeChatRelay = FakeChatRelay(), clock: TestClock = TestClock()): Pair<ChatHub, FakeChatRelay> {
    val hub = ChatHub(relay, ChatExtras(clock::now))
    relay.observer = { hub.extras.apply(it) }
    return hub to relay
}

fun peerFrame(type: String, fields: Map<String, Any?>, id: String = java.util.UUID.randomUUID().toString(), sender: String = "peer-1"): Map<String, Any?> {
    val frame = linkedMapOf<String, Any?>("type" to type, "id" to id, "senderId" to sender, "sender" to "Pat", "ts" to System.currentTimeMillis())
    frame.putAll(fields)
    return frame
}

class FakeAccountApi : ChatAccountCalling {
    val calls = ArrayList<Triple<String, String, Map<String, Any?>?>>()
    val replies = HashMap<String, Map<String, Any?>>()
    val failures = HashMap<String, Exception>()

    override fun accountApiCall(method: String, path: String, body: Map<String, Any?>?): Map<String, Any?> {
        calls.add(Triple(method, path, body))
        val key = "$method ${path.substringBefore('?')}"
        failures[key]?.let { throw it }
        return replies[key] ?: emptyMap()
    }
}
