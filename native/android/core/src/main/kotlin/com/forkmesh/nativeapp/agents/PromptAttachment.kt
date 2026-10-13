package com.forkmesh.nativeapp.agents

import com.forkmesh.nativeapp.crypto.B64

class PromptAttachment(
    val id: String,
    val name: String,
    val bytes: ByteArray,
    val mediaType: String = "image/jpeg",
    val width: Int = 0,
    val height: Int = 0,
) {
    val size: Int get() = bytes.size

    val base64Data: String get() = B64.encode(bytes)

    fun toWire(): Map<String, Any?> = linkedMapOf("name" to name, "mediaType" to mediaType, "data" to base64Data)

    override fun equals(other: Any?): Boolean =
        other is PromptAttachment && other.id == id && other.mediaType == mediaType && other.bytes.contentEquals(bytes)

    override fun hashCode(): Int = id.hashCode() * 31 + bytes.size

    override fun toString(): String = "PromptAttachment($id, $name, $size bytes)"
}

class PromptAttachmentException(override val message: String) : RuntimeException(message) {
    override fun toString(): String = message
}

object PromptAttachmentLimits {
    const val MAX_COUNT = 5
    const val MAX_BYTES = 5 * 1024 * 1024
    const val MAX_TOTAL_BYTES = 10 * 1024 * 1024
    const val MAX_DIMENSION = 2048
    const val JPEG_QUALITY = 80

    fun check(attachments: List<PromptAttachment>) {
        if (attachments.size > MAX_COUNT) throw AgentLaunchException("A prompt can carry $MAX_COUNT photos at most.")
        for (item in attachments) {
            if (item.size > MAX_BYTES) throw AgentLaunchException("${item.name} is larger than 5 MB.")
        }
        if (attachments.sumOf { it.size } > MAX_TOTAL_BYTES) {
            throw AgentLaunchException("Those photos add up to more than 10 MB. Send fewer at once.")
        }
    }
}

fun attachmentPromptBlock(paths: List<String>): String {
    if (paths.isEmpty()) return ""
    val out = StringBuilder("\n\nAttached images (open them with your file-reading tool):")
    for (path in paths) out.append("\n- ").append(path)
    return out.toString()
}

fun promptWithAttachmentPaths(prompt: String, paths: List<String>): String = prompt + attachmentPromptBlock(paths)

fun attachmentRelativePath(attachment: PromptAttachment): String {
    val safe = attachment.id.replace(Regex("[^A-Za-z0-9_-]"), "")
    val ext = when (attachment.mediaType) {
        "image/png" -> "png"
        "image/webp" -> "webp"
        "image/gif" -> "gif"
        else -> "jpg"
    }
    return ".forkmesh/attachments/${safe.ifEmpty { "image" }}.$ext"
}

fun promptOrPhotoNote(prompt: String, attachments: List<PromptAttachment>): String {
    val text = prompt.trim()
    if (text.isNotEmpty() || attachments.isEmpty()) return text
    return "See the attached photos."
}
