package com.forkmesh.nativeapp.market

import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.json.JsonMap
import com.forkmesh.nativeapp.json.asJsonMap
import com.forkmesh.nativeapp.json.str
import java.util.Base64

sealed class ShareStep {
    data class Begin(val id: String, val name: String, val mime: String, val size: Long, val title: String) : ShareStep()
    class Append(val bytes: ByteArray) : ShareStep()
    data class Finish(val name: String, val mime: String, val title: String) : ShareStep()
    data class Reject(val reason: String) : ShareStep()
    data object Ignore : ShareStep()
}

class ShareTransfer(private val maxBytes: Long = MAX_BYTES) {
    private var current: ShareStep.Begin? = null
    private var received = 0L

    val active: Boolean get() = current != null

    fun accept(text: String): ShareStep {
        if (text.length > MAX_MESSAGE_CHARS) return abort("too_large")
        val json = runCatching { Json.parse(text) }.getOrNull().asJsonMap().takeIf { it.isNotEmpty() } ?: return ShareStep.Reject("invalid")
        return when (json.str("event")) {
            "begin" -> begin(json)
            "chunk" -> chunk(json)
            "end" -> end(json)
            else -> ShareStep.Ignore
        }
    }

    fun cancel() {
        current = null
        received = 0
    }

    private fun abort(reason: String): ShareStep {
        cancel()
        return ShareStep.Reject(reason)
    }

    private fun begin(json: JsonMap): ShareStep {
        cancel()
        val id = json.str("id")
        if (!ID.matches(id)) return ShareStep.Reject("invalid_id")
        val mime = mimeType(json.str("mime")) ?: return ShareStep.Reject("unsupported_type")
        val size = when (val raw = json["size"]) {
            is Number -> raw.toDouble().takeIf { it == Math.floor(it) && it > 0 }?.toLong()
            else -> null
        } ?: return ShareStep.Reject("invalid_size")
        if (size > maxBytes) return ShareStep.Reject("too_large")
        val title = json.str("title").trim().take(MAX_TITLE)
        return ShareStep.Begin(id, fileName(json.str("name")), mime, size, title).also { current = it }
    }

    private fun chunk(json: JsonMap): ShareStep {
        val transfer = current ?: return ShareStep.Ignore
        if (json.str("id") != transfer.id) return ShareStep.Ignore
        val bytes = runCatching { Base64.getDecoder().decode(json.str("data")) }.getOrNull() ?: return abort("invalid_data")
        if (received + bytes.size > transfer.size) return abort("too_large")
        received += bytes.size
        return ShareStep.Append(bytes)
    }

    private fun end(json: JsonMap): ShareStep {
        val transfer = current ?: return ShareStep.Ignore
        if (json.str("id") != transfer.id) return ShareStep.Ignore
        if (received != transfer.size) return abort("incomplete")
        cancel()
        return ShareStep.Finish(transfer.name, transfer.mime, transfer.title)
    }

    companion object {
        const val MAX_BYTES = 512L * 1024 * 1024
        const val CHUNK_BYTES = 384 * 1024
        const val MAX_MESSAGE_CHARS = 1024 * 1024
        const val FALLBACK_NAME = "face-changer.mp4"
        const val MAX_NAME = 80
        const val MAX_TITLE = 120
        private val ID = Regex("[A-Za-z0-9_-]{1,64}")
        private val MIME = Regex("(video|image|audio)/[a-z0-9.+-]{1,64}")

        fun fileName(raw: String): String {
            val base = raw.substringAfterLast('/').substringAfterLast('\\')
            val clean = base.filter { it in 'A'..'Z' || it in 'a'..'z' || it in '0'..'9' || it == '.' || it == '_' || it == '-' }
                .trimStart('.')
                .take(MAX_NAME)
            return clean.ifEmpty { FALLBACK_NAME }
        }

        fun mimeType(raw: String): String? = raw.substringBefore(';').trim().lowercase().takeIf { MIME.matches(it) }
    }
}
