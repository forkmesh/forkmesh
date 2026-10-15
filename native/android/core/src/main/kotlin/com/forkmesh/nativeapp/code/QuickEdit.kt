package com.forkmesh.nativeapp.code

import com.forkmesh.nativeapp.localrepos.PatchBuildResult
import com.forkmesh.nativeapp.localrepos.PatchBuilder
import com.forkmesh.nativeapp.localrepos.gitBlobId
import java.nio.ByteBuffer
import java.nio.charset.CharacterCodingException
import java.nio.charset.CodingErrorAction
import java.time.Instant
import java.time.ZoneOffset

enum class QuickEditBlocker { Binary, NotUtf8, TooLarge }

object QuickEdit {
    const val maxBytes: Int = 512 * 1024

    fun blockerTitle(blocker: QuickEditBlocker): String = when (blocker) {
        QuickEditBlocker.Binary -> "Binary file"
        QuickEditBlocker.NotUtf8 -> "Not a text file"
        QuickEditBlocker.TooLarge -> "File too large"
    }

    fun blockerMessage(blocker: QuickEditBlocker): String = when (blocker) {
        QuickEditBlocker.Binary ->
            "This file is not text, so it cannot be edited on this device. Clone the repository to change it."
        QuickEditBlocker.NotUtf8 ->
            "This file is not valid UTF-8. Editing it here would rewrite the bytes that could not be read, so it has to be changed from a clone."
        QuickEditBlocker.TooLarge ->
            "Files over ${maxBytes / 1024} KB are edited from a clone, not in the quick editor."
    }

    fun blockerFor(bytes: ByteArray, maxBytes: Int = QuickEdit.maxBytes): QuickEditBlocker? {
        if (bytes.size > maxBytes) return QuickEditBlocker.TooLarge
        if (bytes.any { it.toInt() == 0 }) return QuickEditBlocker.Binary
        if (decodeStrict(bytes) == null) return QuickEditBlocker.NotUtf8
        return null
    }

    fun decodeStrict(bytes: ByteArray): String? = try {
        Charsets.UTF_8.newDecoder()
            .onMalformedInput(CodingErrorAction.REPORT)
            .onUnmappableCharacter(CodingErrorAction.REPORT)
            .decode(ByteBuffer.wrap(bytes))
            .toString()
    } catch (_: CharacterCodingException) {
        null
    }

    fun loadFailureBlocker(error: Throwable): QuickEditBlocker? =
        if (error.toString().contains("too large")) QuickEditBlocker.TooLarge else null

    fun patch(path: String, bytes: ByteArray, text: String): PatchBuildResult = PatchBuilder.forFile(
        path = path,
        beforeBytes = bytes,
        afterText = text,
        oldId = gitBlobId(bytes),
        newId = gitBlobId(text.toByteArray(Charsets.UTF_8)),
    )

    fun head(account: String, path: String, nowMillis: Long = System.currentTimeMillis()): String {
        val at = Instant.ofEpochMilli(nowMillis).atOffset(ZoneOffset.UTC)
        fun two(value: Int) = value.toString().padStart(2, '0')
        val stamp = "${two(at.monthValue)}${two(at.dayOfMonth)}-${two(at.hour)}${two(at.minute)}"
        val slug = path.split('/').last().lowercase()
            .replace(Regex("[^a-z0-9]+"), "-")
            .replace(Regex("^-+|-+$"), "")
        val branch = "mobile-edit/${slug.ifEmpty { "file" }}-$stamp"
        return if (account.isEmpty()) branch else "$account:$branch"
    }

    fun suggestTitle(path: String): String = "Update ${path.split('/').last()}"

    fun base(ref: String, defaultBranch: String): String = ref.ifEmpty { defaultBranch }

    fun submitFailure(error: String): String = when {
        error.contains("inbox_full") ->
            "This repository is not accepting pull requests right now — its inbox is full until the owner drains it."
        error.contains("author_quota") ->
            "You have too many pull requests pending on this repository. Wait for the owner to work through them."
        error.contains("pull_too_large") -> "This change is too large to submit from mobile."
        else -> "Could not submit the change: $error"
    }

    const val submittedMessage: String = "Submitted to the inbox - pending the repo owner applying it."

    fun nothingChanged(path: String): String = "Nothing changed in $path."

    fun composeFooter(path: String, base: String): String = "$path · into $base"
}
