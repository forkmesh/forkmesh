package com.forkmesh.nativeapp.chat

import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.json.asJsonMap
import com.forkmesh.nativeapp.json.jsonLong
import com.forkmesh.nativeapp.json.jsonMaps
import com.forkmesh.nativeapp.json.jsonText
import com.forkmesh.nativeapp.model.ChatMessage
import com.forkmesh.nativeapp.model.ChatRetention
import java.io.File

data class ChatHistorySnapshot(
    val messages: List<ChatMessage> = emptyList(),
    val replies: List<ChatMessage> = emptyList(),
    val replyRoots: Map<String, String> = emptyMap(),
    val reactions: Map<String, Map<String, Map<String, String>>> = emptyMap(),
    val reactionOrder: Map<String, List<String>> = emptyMap(),
    val edits: Map<String, String> = emptyMap(),
    val privateChannels: List<String> = emptyList(),
    val hiddenChannels: List<String> = emptyList(),
    val hiddenMessages: List<String> = emptyList(),
)

object ChatHistoryCodec {
    const val VERSION = 1

    fun bounded(
        conversations: Map<String, List<ChatMessage>>,
        retention: ChatRetention? = null,
        nowMs: Long = System.currentTimeMillis(),
    ): List<ChatMessage> {
        val out = ArrayList<ChatMessage>()
        for (key in conversations.keys.sorted()) {
            if (key == "#team" || key == MESH_CONVERSATION || !(key.startsWith("#") || key.startsWith("@"))) continue
            val list = conversations[key].orEmpty()
            val kept = ArrayList<ChatMessage>()
            var chars = 0
            for (message in list.asReversed()) {
                if (retention != null && retention.isExpired(message.timestamp, key, nowMs)) break
                if (kept.size >= ChatLimits.HISTORY_ENTRIES_PER_CHANNEL) break
                val cost = message.text.length + 64
                if (chars + cost > ChatLimits.HISTORY_CHARS_PER_CHANNEL && kept.isNotEmpty()) break
                chars += cost
                kept.add(message)
            }
            out.addAll(kept.asReversed())
        }
        return out
    }

    private fun encodeMessage(message: ChatMessage): Map<String, Any?> {
        val out = linkedMapOf<String, Any?>(
            "id" to message.id,
            "c" to message.conversation,
            "sid" to message.senderId,
            "s" to message.senderName,
            "t" to message.text,
            "ts" to message.timestamp,
        )
        if (message.self) out["self"] = true
        if (message.fileName.isNotEmpty()) out["f"] = message.fileName
        return out
    }

    private fun decodeMessage(map: Map<String, Any?>): ChatMessage? {
        val id = jsonText(map["id"])
        val conversation = jsonText(map["c"])
        if (id.isEmpty() || conversation.isEmpty()) return null
        return ChatMessage(
            id = id,
            conversation = conversation,
            senderId = jsonText(map["sid"]),
            senderName = jsonText(map["s"]).take(ChatLimits.MAX_DISPLAY_NAME_CHARS),
            text = jsonText(map["t"]).take(ChatLimits.MAX_TEXT_CHARS),
            timestamp = jsonLong(map["ts"]),
            self = map["self"] == true,
            fileName = jsonText(map["f"]).take(ChatLimits.MAX_FILE_NAME_CHARS),
        )
    }

    fun encode(snapshot: ChatHistorySnapshot): String {
        val reactions = LinkedHashMap<String, Any?>()
        for ((id, state) in snapshot.reactions) {
            val perValue = LinkedHashMap<String, Any?>()
            for ((value, reactors) in state) perValue[value] = LinkedHashMap<String, Any?>(reactors)
            reactions[id] = perValue
        }
        val root = linkedMapOf<String, Any?>(
            "v" to VERSION,
            "messages" to snapshot.messages.map(::encodeMessage),
            "replies" to snapshot.replies.map(::encodeMessage),
            "roots" to LinkedHashMap<String, Any?>(snapshot.replyRoots),
            "reactions" to reactions,
            "order" to LinkedHashMap<String, Any?>(snapshot.reactionOrder),
            "edits" to LinkedHashMap<String, Any?>(snapshot.edits),
            "private" to snapshot.privateChannels,
            "hidden" to snapshot.hiddenChannels,
            "hiddenMessages" to snapshot.hiddenMessages,
        )
        return Json.stringify(root)
    }

    fun decode(text: String): ChatHistorySnapshot? {
        val root = try {
            Json.parseObject(text)
        } catch (_: Exception) {
            return null
        }
        if (jsonLong(root["v"], -1) != VERSION.toLong()) return null
        val reactions = LinkedHashMap<String, Map<String, Map<String, String>>>()
        for ((id, value) in root["reactions"].asJsonMap()) {
            val state = LinkedHashMap<String, Map<String, String>>()
            for ((emoji, reactors) in value.asJsonMap()) {
                val names = LinkedHashMap<String, String>()
                for ((reactor, name) in reactors.asJsonMap()) names[reactor] = jsonText(name)
                if (names.isNotEmpty()) state[emoji] = names
            }
            if (state.isNotEmpty()) reactions[id] = state
        }
        return ChatHistorySnapshot(
            messages = jsonMaps(root["messages"]).mapNotNull(::decodeMessage),
            replies = jsonMaps(root["replies"]).mapNotNull(::decodeMessage),
            replyRoots = root["roots"].asJsonMap().mapValues { jsonText(it.value) },
            reactions = reactions,
            reactionOrder = root["order"].asJsonMap().mapValues { (_, v) -> (v as? List<*>).orEmpty().map(::jsonText) },
            edits = root["edits"].asJsonMap().mapValues { jsonText(it.value) },
            privateChannels = (root["private"] as? List<*>).orEmpty().map(::jsonText),
            hiddenChannels = (root["hidden"] as? List<*>).orEmpty().map(::jsonText),
            hiddenMessages = (root["hiddenMessages"] as? List<*>).orEmpty().map(::jsonText),
        )
    }

    fun scopeName(serverUrl: String, room: String): String {
        var hash = -0x340d631b7bdddcdbL
        for (byte in "$serverUrl|$room".toByteArray(Charsets.UTF_8)) {
            hash = hash xor (byte.toLong() and 0xff)
            hash *= 0x100000001b3L
        }
        return java.lang.Long.toUnsignedString(hash, 16)
    }
}

class ChatHistoryStore(val directory: File) {
    fun file(scope: String): File = File(directory, "relay-$scope.json")

    fun load(scope: String): ChatHistorySnapshot? {
        val target = file(scope)
        if (!target.isFile) return null
        return try {
            ChatHistoryCodec.decode(target.readText(Charsets.UTF_8))
        } catch (_: Exception) {
            null
        }
    }

    @Synchronized
    fun save(snapshot: ChatHistorySnapshot, scope: String) {
        directory.mkdirs()
        val target = file(scope)
        val temp = File(directory, "${target.name}.tmp")
        temp.writeText(ChatHistoryCodec.encode(snapshot), Charsets.UTF_8)
        if (!temp.renameTo(target)) {
            target.delete()
            temp.renameTo(target)
        }
    }

    fun erase(scope: String) {
        file(scope).delete()
    }
}
