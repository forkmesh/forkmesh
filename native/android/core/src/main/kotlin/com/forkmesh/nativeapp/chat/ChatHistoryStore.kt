package com.forkmesh.nativeapp.chat

import com.forkmesh.nativeapp.api.AppEnv
import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.json.asJsonMap
import com.forkmesh.nativeapp.json.jsonLong
import com.forkmesh.nativeapp.json.jsonMaps
import com.forkmesh.nativeapp.json.jsonText
import com.forkmesh.nativeapp.model.ChatMessage
import com.forkmesh.nativeapp.model.ChatReceipt
import com.forkmesh.nativeapp.model.ChatReceiptTimes
import com.forkmesh.nativeapp.model.ChatRetention
import java.io.File
import java.net.URI
import java.nio.file.Files
import java.nio.file.StandardCopyOption

data class ChatHistorySnapshot(
    val messages: List<ChatMessage> = emptyList(),
    val replies: List<ChatMessage> = emptyList(),
    val replyRoots: Map<String, String> = emptyMap(),
    val reactions: Map<String, Map<String, Map<String, String>>> = emptyMap(),
    val reactionOrder: Map<String, List<String>> = emptyMap(),
    val stickers: Map<String, Map<String, Map<String, String>>> = emptyMap(),
    val edits: Map<String, String> = emptyMap(),
    val privateChannels: List<String> = emptyList(),
    val hiddenChannels: List<String> = emptyList(),
    val hiddenMessages: List<String> = emptyList(),
    val receipts: Map<String, ChatReceipt> = emptyMap(),
    val receiptTimes: Map<String, ChatReceiptTimes> = emptyMap(),
)

sealed class ChatHistoryLoad {
    data class Loaded(val snapshot: ChatHistorySnapshot) : ChatHistoryLoad()
    data object Missing : ChatHistoryLoad()
    data object Unreadable : ChatHistoryLoad()
}

object ChatHistoryCodec {
    const val VERSION = 1

    val RELAY_PATHS = listOf("/api/repo/mainnode/forkmesh/rooms/general/ws", AppEnv.RELAY_PATH)

    val PROD_ALIAS_HOSTS = setOf("forkmesh.com", "www.forkmesh.com", "api.forkmesh.com", "next.forkmesh.com", "app.next.forkmesh.com")

    fun receiptWire(receipt: ChatReceipt): String = receipt.name.lowercase()

    fun receiptFromWire(value: String): ChatReceipt? = ChatReceipt.entries.firstOrNull { receiptWire(it) == value }

    fun bounded(
        conversations: Map<String, List<ChatMessage>>,
        retention: ChatRetention? = null,
        nowMs: Long = System.currentTimeMillis(),
    ): List<ChatMessage> {
        val out = ArrayList<ChatMessage>()
        for (key in conversations.keys.sorted()) {
            if (key == "#team" || key == MESH_CONVERSATION) continue
            if (!(key.startsWith("#") || key.startsWith("@") || DirectMessageFrames.isDirect(key))) continue
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
        if (message.serverVisible) out["sv"] = true
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
            serverVisible = map["sv"] == true,
        )
    }

    private fun encodeReactors(source: Map<String, Map<String, Map<String, String>>>): Map<String, Any?> {
        val out = LinkedHashMap<String, Any?>()
        for ((id, state) in source) {
            val perValue = LinkedHashMap<String, Any?>()
            for ((value, reactors) in state) perValue[value] = LinkedHashMap<String, Any?>(reactors)
            out[id] = perValue
        }
        return out
    }

    private fun decodeReactors(source: Any?): Map<String, Map<String, Map<String, String>>> {
        val out = LinkedHashMap<String, Map<String, Map<String, String>>>()
        for ((id, value) in source.asJsonMap()) {
            val state = LinkedHashMap<String, Map<String, String>>()
            for ((key, reactors) in value.asJsonMap()) {
                val names = LinkedHashMap<String, String>()
                for ((reactor, name) in reactors.asJsonMap()) names[reactor] = jsonText(name)
                if (names.isNotEmpty()) state[key] = names
            }
            if (state.isNotEmpty()) out[id] = state
        }
        return out
    }

    fun encode(snapshot: ChatHistorySnapshot): String {
        val receipts = LinkedHashMap<String, Any?>()
        for ((id, receipt) in snapshot.receipts) receipts[id] = receiptWire(receipt)
        val receiptTimes = LinkedHashMap<String, Any?>()
        for ((id, times) in snapshot.receiptTimes) {
            if (times.isEmpty) continue
            val entry = LinkedHashMap<String, Any?>()
            times.delivered?.let { entry["delivered"] = it }
            times.read?.let { entry["read"] = it }
            receiptTimes[id] = entry
        }
        val root = linkedMapOf<String, Any?>(
            "v" to VERSION,
            "messages" to snapshot.messages.map(::encodeMessage),
            "replies" to snapshot.replies.map(::encodeMessage),
            "roots" to LinkedHashMap<String, Any?>(snapshot.replyRoots),
            "reactions" to encodeReactors(snapshot.reactions),
            "stickers" to encodeReactors(snapshot.stickers),
            "order" to LinkedHashMap<String, Any?>(snapshot.reactionOrder),
            "edits" to LinkedHashMap<String, Any?>(snapshot.edits),
            "private" to snapshot.privateChannels,
            "hidden" to snapshot.hiddenChannels,
            "hiddenMessages" to snapshot.hiddenMessages,
            "receipts" to receipts,
            "receiptTimes" to receiptTimes,
        )
        return Json.stringify(root)
    }

    fun decode(text: String): ChatHistorySnapshot? {
        val root = try {
            Json.parseObject(text)
        } catch (_: Exception) {
            return null
        }
        if (jsonLong(root["v"], -1) < VERSION) return null
        val receipts = LinkedHashMap<String, ChatReceipt>()
        for ((id, value) in root["receipts"].asJsonMap()) receiptFromWire(jsonText(value))?.let { receipts[id] = it }
        val receiptTimes = LinkedHashMap<String, ChatReceiptTimes>()
        for ((id, value) in root["receiptTimes"].asJsonMap()) {
            val entry = value.asJsonMap()
            val times = ChatReceiptTimes(delivered = (entry["delivered"] as? Number)?.toLong(), read = (entry["read"] as? Number)?.toLong())
            if (!times.isEmpty) receiptTimes[id] = times
        }
        return ChatHistorySnapshot(
            messages = jsonMaps(root["messages"]).mapNotNull(::decodeMessage),
            replies = jsonMaps(root["replies"]).mapNotNull(::decodeMessage),
            replyRoots = root["roots"].asJsonMap().mapValues { jsonText(it.value) },
            reactions = decodeReactors(root["reactions"]),
            stickers = decodeReactors(root["stickers"]),
            reactionOrder = root["order"].asJsonMap().mapValues { (_, v) -> (v as? List<*>).orEmpty().map(::jsonText) },
            edits = root["edits"].asJsonMap().mapValues { jsonText(it.value) },
            privateChannels = (root["private"] as? List<*>).orEmpty().map(::jsonText),
            hiddenChannels = (root["hidden"] as? List<*>).orEmpty().map(::jsonText),
            hiddenMessages = (root["hiddenMessages"] as? List<*>).orEmpty().map(::jsonText),
            receipts = receipts,
            receiptTimes = receiptTimes,
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

    fun projectScope(projectId: String, room: String): String = "project-" + scopeName(projectId, room)

    fun formerScopes(serverUrl: String, room: String): List<String> {
        val urls = arrayListOf(serverUrl)
        val uri = try {
            URI(serverUrl.trim())
        } catch (_: Exception) {
            null
        }
        val host = uri?.host?.lowercase()
        if (uri != null && host != null && uri.rawPath in RELAY_PATHS) {
            val prod = uri.port == -1 && (host == AppEnv.PROD_HOST || host in PROD_ALIAS_HOSTS)
            val hosts = if (prod) listOf(AppEnv.PROD_HOST) + PROD_ALIAS_HOSTS.sorted() else listOf(host)
            val schemes = if (prod) listOf("wss") else listOf(uri.scheme ?: "wss")
            for (scheme in schemes) {
                for (name in hosts) {
                    for (path in RELAY_PATHS) urls.add(rebuild(uri, scheme, name, path))
                }
            }
        }
        return urls.map { scopeName(it, room) }.distinct()
    }

    private fun rebuild(uri: URI, scheme: String, host: String, path: String): String = buildString {
        append(scheme).append("://")
        uri.rawUserInfo?.let { append(it).append('@') }
        append(host)
        if (uri.port != -1) append(':').append(uri.port)
        append(path)
        uri.rawQuery?.let { append('?').append(it) }
        uri.rawFragment?.let { append('#').append(it) }
    }

    fun merge(snapshots: List<ChatHistorySnapshot>): ChatHistorySnapshot {
        fun union(lists: List<List<ChatMessage>>): List<ChatMessage> {
            val ids = HashSet<String>()
            val kept = ArrayList<ChatMessage>()
            for (list in lists) for (message in list) if (ids.add(message.id)) kept.add(message)
            return kept.sortedBy { it.timestamp }
        }
        fun names(lists: List<List<String>>): List<String> = lists.flatten().distinct()
        fun <V> first(maps: List<Map<String, V>>): Map<String, V> {
            val out = LinkedHashMap<String, V>()
            for (map in maps) for ((key, value) in map) if (key !in out) out[key] = value
            return out
        }
        return ChatHistorySnapshot(
            messages = union(snapshots.map { it.messages }),
            replies = union(snapshots.map { it.replies }),
            replyRoots = first(snapshots.map { it.replyRoots }),
            reactions = first(snapshots.map { it.reactions }),
            reactionOrder = first(snapshots.map { it.reactionOrder }),
            edits = first(snapshots.map { it.edits }),
            privateChannels = names(snapshots.map { it.privateChannels }),
            hiddenChannels = names(snapshots.map { it.hiddenChannels }),
            hiddenMessages = names(snapshots.map { it.hiddenMessages }),
            receipts = first(snapshots.map { it.receipts }),
            receiptTimes = first(snapshots.map { it.receiptTimes }),
        )
    }
}

class ChatHistoryStore(val directory: File) {
    private val blocked = HashSet<String>()
    private val written = HashMap<String, ChatHistorySnapshot>()

    @Volatile
    var writes = 0
        private set

    fun file(scope: String): File = File(directory, "relay-$scope.json")

    fun load(scope: String): ChatHistorySnapshot? = (read(scope) as? ChatHistoryLoad.Loaded)?.snapshot

    @Synchronized
    fun read(scope: String, formerScopes: List<String> = emptyList()): ChatHistoryLoad {
        val target = file(scope)
        if (!target.exists()) {
            val former = formerScopes.filter { it != scope }.map(::file).filter { it.exists() }
            if (former.isEmpty()) {
                blocked.remove(target.path)
                return ChatHistoryLoad.Missing
            }
            return adopt(former, target)
        }
        val text = try {
            target.readText(Charsets.UTF_8)
        } catch (_: Exception) {
            blocked.add(target.path)
            return ChatHistoryLoad.Unreadable
        }
        val snapshot = ChatHistoryCodec.decode(text)
        if (snapshot == null) {
            if (setAside(target, "unreadable-${System.currentTimeMillis()}")) {
                blocked.remove(target.path)
                return ChatHistoryLoad.Missing
            }
            blocked.add(target.path)
            return ChatHistoryLoad.Unreadable
        }
        blocked.remove(target.path)
        written[target.path] = snapshot
        return ChatHistoryLoad.Loaded(snapshot)
    }

    private fun adopt(former: List<File>, target: File): ChatHistoryLoad {
        val found = ArrayList<Triple<File, Long, ChatHistorySnapshot>>()
        for (file in former) {
            val text = try {
                file.readText(Charsets.UTF_8)
            } catch (_: Exception) {
                blocked.add(target.path)
                return ChatHistoryLoad.Unreadable
            }
            val snapshot = ChatHistoryCodec.decode(text) ?: continue
            found.add(Triple(file, file.lastModified(), snapshot))
        }
        blocked.remove(target.path)
        if (found.isEmpty()) return ChatHistoryLoad.Missing
        val merged = ChatHistoryCodec.merge(found.sortedByDescending { it.second }.map { it.third })
        if (write(merged, target)) {
            for ((file, _, _) in found) setAside(file, "migrated")
        }
        return ChatHistoryLoad.Loaded(merged)
    }

    private fun setAside(file: File, suffix: String): Boolean = try {
        Files.move(file.toPath(), File(file.parentFile, "${file.name}.$suffix").toPath())
        true
    } catch (_: Exception) {
        false
    }

    @Synchronized
    fun save(snapshot: ChatHistorySnapshot, scope: String) {
        write(snapshot, file(scope))
    }

    private fun write(snapshot: ChatHistorySnapshot, target: File): Boolean {
        if (target.path in blocked) return false
        if (written[target.path] == snapshot) return true
        return try {
            directory.mkdirs()
            val temp = File(directory, "${target.name}.tmp")
            temp.writeText(ChatHistoryCodec.encode(snapshot), Charsets.UTF_8)
            Files.move(temp.toPath(), target.toPath(), StandardCopyOption.REPLACE_EXISTING, StandardCopyOption.ATOMIC_MOVE)
            written[target.path] = snapshot
            writes++
            true
        } catch (_: Exception) {
            written.remove(target.path)
            false
        }
    }

    @Synchronized
    fun erase(scope: String) {
        val target = file(scope)
        written.remove(target.path)
        target.delete()
    }
}
