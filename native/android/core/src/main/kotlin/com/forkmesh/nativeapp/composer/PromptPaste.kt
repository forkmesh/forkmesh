package com.forkmesh.nativeapp.composer

import com.forkmesh.nativeapp.agents.PromptAttachmentLimits
import java.util.Base64

class PasteboardItem(val representations: Map<String, ByteArray>, val embeddedImages: List<ByteArray> = emptyList())

class PastedImage(val name: String, val data: ByteArray)

class PromptPaste(val images: List<PastedImage>, val text: String)

class AdmittedPaste(val accepted: List<PastedImage>, val problem: String?)

object PromptPasteRules {
    val imageTypes = listOf(
        "image/png", "image/jpeg", "image/heic", "image/heif", "image/tiff", "image/gif", "image/webp", "image/bmp",
    )
    const val HTML_TYPE = "text/html"
    val textTypes = listOf("text/plain")

    fun isImageType(mimeType: String): Boolean = mimeType.lowercase().startsWith("image/")

    fun wants(mimeType: String): Boolean {
        val type = mimeType.lowercase()
        return isImageType(type) || type == HTML_TYPE || type in textTypes
    }

    fun paste(items: List<PasteboardItem>): PromptPaste {
        val images = ArrayList<PastedImage>()
        val texts = ArrayList<String>()
        for (item in items) {
            val imageType = imageTypes.firstOrNull { type -> item.representations[type]?.let(::looksLikeImage) == true }
                ?: item.representations.keys.firstOrNull { isImageType(it) && looksLikeImage(item.representations.getValue(it)) }
            if (imageType != null) {
                images.add(PastedImage("pasted-image-${images.size + 1}", item.representations.getValue(imageType)))
                continue
            }
            var embedded = item.embeddedImages.filter(::looksLikeImage)
            if (embedded.isEmpty()) {
                val html = item.representations[HTML_TYPE]
                if (html != null) embedded = dataUriImages(String(html, Charsets.UTF_8))
            }
            for (data in embedded) images.add(PastedImage("pasted-image-${images.size + 1}", data))
            val text = plainText(item)
            if (!text.isNullOrEmpty()) texts.add(text)
        }
        return PromptPaste(images, texts.joinToString("\n"))
    }

    private fun plainText(item: PasteboardItem): String? {
        val type = textTypes.firstOrNull { item.representations[it] != null } ?: return null
        return String(item.representations.getValue(type), Charsets.UTF_8).replace("￼", "")
    }

    fun dataUriImages(html: String): List<ByteArray> {
        val out = ArrayList<ByteArray>()
        val lower = html.lowercase()
        var from = 0
        while (true) {
            val start = lower.indexOf("data:image/", from)
            if (start < 0) break
            val after = start + "data:image/".length
            val marker = lower.indexOf(";base64,", after)
            if (marker < 0 || marker - after > 32) {
                from = after
                continue
            }
            val bodyStart = marker + ";base64,".length
            var end = bodyStart
            while (end < html.length) {
                val c = html[end]
                if (c.isLetterOrDigit() || c == '+' || c == '/' || c == '=' || c.isWhitespace()) end++ else break
            }
            val encoded = html.substring(bodyStart, end).filter { !it.isWhitespace() }
            val data = try {
                Base64.getDecoder().decode(encoded)
            } catch (error: IllegalArgumentException) {
                null
            }
            if (data != null && looksLikeImage(data)) out.add(data)
            from = end
        }
        return out
    }

    fun looksLikeImage(data: ByteArray): Boolean {
        val b = data.copyOfRange(0, minOf(16, data.size))
        fun starts(prefix: ByteArray, at: Int = 0): Boolean =
            b.size >= at + prefix.size && prefix.indices.all { b[at + it] == prefix[it] }
        fun bytes(vararg values: Int) = ByteArray(values.size) { values[it].toByte() }
        if (starts(bytes(0x89, 0x50, 0x4E, 0x47)) || starts(bytes(0xFF, 0xD8, 0xFF)) || starts("GIF8".toByteArray())) return true
        if (starts(bytes(0x49, 0x49, 0x2A, 0x00)) || starts(bytes(0x4D, 0x4D, 0x00, 0x2A)) || starts("BM".toByteArray())) return true
        if (starts("RIFF".toByteArray()) && starts("WEBP".toByteArray(), 8)) return true
        if (starts("ftyp".toByteArray(), 4)) {
            val brand = if (b.size >= 12) String(b, 8, 4, Charsets.US_ASCII) else ""
            return brand in listOf("heic", "heix", "hevc", "heim", "heis", "mif1", "msf1", "avif")
        }
        return false
    }

    fun admit(images: List<PastedImage>, existing: Int): AdmittedPaste {
        val room = maxOf(0, PromptAttachmentLimits.MAX_COUNT - existing)
        if (images.size <= room) return AdmittedPaste(images, null)
        val left = images.size - room
        val problem = "A prompt can carry ${PromptAttachmentLimits.MAX_COUNT} photos, so " +
            (if (left == 1) "1 pasted image was" else "$left pasted images were") + " left out."
        return AdmittedPaste(images.take(room), problem)
    }
}
