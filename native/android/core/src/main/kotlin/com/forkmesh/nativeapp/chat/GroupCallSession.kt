package com.forkmesh.nativeapp.chat

import com.forkmesh.nativeapp.json.asJsonMap
import com.forkmesh.nativeapp.json.jsonText

data class GroupCallView(
    val conversation: String = "",
    val media: DirectCallKind = DirectCallKind.AUDIO,
    val state: String = "",
    val others: Int = 0,
    val muted: Boolean = false,
    val cameraOff: Boolean = false,
    val error: String = "",
    val engine: String = "",
    val verify: GroupCallVerify? = null,
) {
    val active: Boolean get() = conversation.isNotEmpty()

    val fmm: Boolean get() = engine == ENGINE_FMM

    val title: String get() = "#" + conversation.removePrefix(OfficeFrames.PREFIX).removePrefix("#")

    val stateLine: String get() = when (state) {
        "live" -> "Connected · end-to-end encrypted · ${others + 1} in call"
        "reconnecting" -> "Reconnecting…"
        else -> "Connecting…"
    }

    companion object {
        const val ENGINE_FMM = "fmm"
        const val NOTE = "End-to-end encrypted. ForkMesh's call server forwards everyone's audio and video without being able to see or hear them."
    }
}

sealed class GroupCallPageEvent {
    data object Ready : GroupCallPageEvent()
    data class Send(val conversation: String, val type: String, val body: Map<String, Any?>) : GroupCallPageEvent()
    data class Ticket(val id: Int, val conversation: String, val body: Map<String, Any?>) : GroupCallPageEvent()
    data class State(
        val state: String,
        val others: Int,
        val muted: Boolean,
        val cameraOff: Boolean,
        val active: Boolean,
        val error: String,
        val engine: String = "",
        val verify: GroupCallVerify? = null,
    ) : GroupCallPageEvent()
    data class Result(val id: Int, val ok: Boolean, val value: Any?, val error: String) : GroupCallPageEvent()

    companion object {
        fun parse(message: Map<String, Any?>): GroupCallPageEvent? = when (jsonText(message["event"])) {
            "ready" -> Ready
            "send" -> {
                val type = jsonText(message["type"])
                val conversation = jsonText(message["channel"])
                if (type in GroupCallFrames.LIVE_TYPES && conversation.isNotEmpty()) Send(conversation, type, message["body"].asJsonMap()) else null
            }
            "ticket" -> (message["id"] as? Number)?.toInt()?.let { Ticket(it, jsonText(message["channel"]), message["body"].asJsonMap()) }
            "state" -> State(
                state = jsonText(message["state"]),
                others = ((message["others"] as? Number)?.toInt() ?: 0).coerceIn(0, GroupCallFrames.MAX_PEOPLE - 1),
                muted = message["muted"] == true,
                cameraOff = message["cameraOff"] == true,
                active = message["active"] == true,
                error = jsonText(message["error"]).take(200),
                engine = jsonText(message["engine"]).takeIf { it == "fmm" || it == "webrtc" }.orEmpty(),
                verify = if (jsonText(message["engine"]) == "fmm") GroupCallVerify.parse(message["verify"]) else null,
            )
            "result" -> (message["id"] as? Number)?.toInt()?.let {
                Result(it, message["ok"] == true, message["value"], jsonText(message["error"]))
            }
            else -> null
        }
    }
}

class GroupCallFrameBuffer(
    private val now: () -> Long = System::currentTimeMillis,
    private val limit: Int = 64,
) {
    private class Kept(val plain: Map<String, Any?>, val at: Long)

    private val lock = Any()
    private val frames = HashMap<String, ArrayDeque<Kept>>()

    fun add(plain: Map<String, Any?>, conversation: String) {
        if (jsonText(plain["type"]) !in GroupCallFrames.LIVE_TYPES) return
        synchronized(lock) {
            val list = frames.getOrPut(conversation) { ArrayDeque() }
            list.addLast(Kept(plain, now()))
            while (list.size > limit) list.removeFirst()
        }
    }

    fun recent(conversation: String): List<Map<String, Any?>> = synchronized(lock) {
        val cutoff = now() - GroupCallFrames.PRESENCE_TTL_MS
        val list = frames[conversation] ?: return emptyList()
        list.removeAll { it.at < cutoff }
        list.map { it.plain }
    }

    fun clear() = synchronized(lock) { frames.clear() }
}
