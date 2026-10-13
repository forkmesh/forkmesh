package com.forkmesh.nativeapp.model

import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.json.jsonLong
import com.forkmesh.nativeapp.json.jsonOf

data class ChatRetentionPreset(val ms: Long, val label: String)

data class ChatRetention(
    val defaultMs: Long = DEFAULT_MS,
    val chats: Map<String, Long> = emptyMap(),
    val updatedAt: Long = 0,
) {
    fun overrideMs(key: String): Long? = chats[key]?.let(::clamp)

    fun effectiveMs(key: String): Long = clamp(chats[key] ?: defaultMs)

    fun cutoff(key: String, nowMs: Long): Long = nowMs - effectiveMs(key)

    fun isExpired(timestampMs: Long, key: String, nowMs: Long): Boolean = timestampMs <= cutoff(key, nowMs)

    val defaultLabel: String get() = label(clamp(defaultMs))

    fun toJson(): LinkedHashMap<String, Any?> = jsonOf(
        "defaultMs" to clamp(defaultMs),
        "chats" to LinkedHashMap<String, Any?>(chats.mapValues { clamp(it.value) }),
        "updatedAt" to updatedAt,
    )

    fun encode(): String = Json.stringify(toJson())

    companion object {
        const val MIN_MS = 3_600_000L
        const val MAX_MS = 604_800_000L
        const val DEFAULT_MS = MAX_MS
        const val OFFICE_KEY_PREFIX = "channel:"

        private const val HOUR_MS = 3_600_000L
        private const val DAY_MS = 24 * HOUR_MS

        val PRESETS: List<ChatRetentionPreset> = listOf(
            ChatRetentionPreset(7 * DAY_MS, "7 days"),
            ChatRetentionPreset(3 * DAY_MS, "3 days"),
            ChatRetentionPreset(DAY_MS, "1 day"),
            ChatRetentionPreset(8 * HOUR_MS, "8 hours"),
            ChatRetentionPreset(HOUR_MS, "1 hour"),
        )

        fun clamp(ms: Long): Long = ms.coerceIn(MIN_MS, MAX_MS)

        fun label(ms: Long): String {
            val value = clamp(ms)
            PRESETS.firstOrNull { it.ms == value }?.let { return it.label }
            fun plural(count: Long, unit: String) = if (count == 1L) "1 $unit" else "$count ${unit}s"
            return when {
                value % DAY_MS == 0L -> plural(value / DAY_MS, "day")
                value % HOUR_MS == 0L -> plural(value / HOUR_MS, "hour")
                else -> plural(value / 60_000L, "minute")
            }
        }

        fun officeKey(roomId: String): String = OFFICE_KEY_PREFIX + roomId

        fun key(conversation: String, officeRoomId: String = ""): String =
            if (officeRoomId.isNotEmpty()) officeKey(officeRoomId) else conversation

        fun prefsKey(nodeName: String): String = "chat_retention/${nodeName.trim().lowercase()}"

        fun fromJson(value: Any?): ChatRetention {
            val json = value as? Map<*, *> ?: return ChatRetention()
            val default = json["defaultMs"]
            val chats = LinkedHashMap<String, Long>()
            for ((rawKey, rawValue) in (json["chats"] as? Map<*, *>).orEmpty()) {
                val key = rawKey?.toString().orEmpty()
                if (key.isEmpty() || rawValue !is Number) continue
                chats[key] = clamp(rawValue.toLong())
            }
            return ChatRetention(
                defaultMs = if (default is Number) clamp(default.toLong()) else DEFAULT_MS,
                chats = chats,
                updatedAt = jsonLong(json["updatedAt"]).coerceAtLeast(0),
            )
        }

        fun fromJsonOrNull(value: Any?): ChatRetention? = if (value is Map<*, *>) fromJson(value) else null

        fun decode(text: String?): ChatRetention? {
            if (text.isNullOrEmpty()) return null
            return try {
                fromJsonOrNull(Json.parse(text))
            } catch (_: Exception) {
                null
            }
        }

        fun defaultBody(ms: Long?): LinkedHashMap<String, Any?> = jsonOf("defaultMs" to ms)

        fun chatBody(key: String, ms: Long?): LinkedHashMap<String, Any?> = jsonOf("chat" to key, "retentionMs" to ms)
    }
}

class ChatRetentionException(val code: String, message: String) : Exception(message)

fun chatRetentionErrorMessage(code: String): String = when (code) {
    "invalid_chat_retention" -> "That disappearing-message time is not valid."
    "too_many_chat_retention_overrides" -> "Too many chats have their own time. Set some back to the default first."
    "unauthorized", "http_401" -> "Your session expired. Please sign in again."
    else -> "Could not save when messages disappear. Check your connection and try again."
}
