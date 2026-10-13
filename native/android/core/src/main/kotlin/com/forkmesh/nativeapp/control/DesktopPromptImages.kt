package com.forkmesh.nativeapp.control

import com.forkmesh.nativeapp.agents.PromptAttachment
import com.forkmesh.nativeapp.crypto.B64
import com.forkmesh.nativeapp.crypto.Sha256
import com.forkmesh.nativeapp.json.jsonText

object DesktopPromptImages {
    const val CHUNK_BYTES = 192 * 1024
    const val RELAY_CHARS_PER_SECOND = 100_000

    val sleep: (Long) -> Unit = { millis -> if (millis > 0) Thread.sleep(millis) }

    fun upload(images: List<PromptAttachment>, link: ControlLink, pause: (Long) -> Unit = sleep): List<String> {
        val ids = mutableListOf<String>()
        try {
            for (image in images) {
                val bytes = image.bytes
                val begin = link.request(
                    "agents.image.begin",
                    linkedMapOf("mediaType" to image.mediaType, "size" to bytes.size, "sha256" to Sha256.hex(bytes)),
                    20_000,
                )
                val id = jsonText(begin["image"])
                if (id.isEmpty()) throw LocalControlException("bad_image")
                ids += id
                var offset = 0
                while (offset < bytes.size) {
                    val end = minOf(bytes.size, offset + CHUNK_BYTES)
                    val encoded = B64.encode(bytes.copyOfRange(offset, end))
                    link.request("agents.image.chunk", linkedMapOf("image" to id, "data" to encoded), 60_000)
                    offset = end
                    if (link.transport == ControlTransport.Relay) {
                        pause(encoded.length * 1000L / RELAY_CHARS_PER_SECOND)
                    }
                }
            }
        } catch (error: Exception) {
            for (id in ids) runCatching { link.request("agents.image.cancel", mapOf("image" to id), 10_000) }
            if (error is LocalControlException && error.code == "unsupported") throw LocalControlException("images_unsupported")
            throw error
        }
        return ids
    }

    fun params(
        base: Map<String, Any?>,
        images: List<PromptAttachment>,
        link: ControlLink,
        pause: (Long) -> Unit = sleep,
    ): Map<String, Any?> {
        if (images.isEmpty()) return base
        return LinkedHashMap(base).apply { put("images", upload(images, link, pause)) }
    }
}
