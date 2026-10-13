package com.forkmesh.nativeapp.agents

import java.nio.ByteBuffer
import java.nio.charset.CharacterCodingException
import java.nio.charset.CodingErrorAction

data class PromptTextFile(val id: String, val name: String, val text: String) {
    val chars: Int get() = text.length
}

object PromptTextFiles {
    const val HEADER = "Uploaded files (sent from the phone that started this run):"
    const val MAX_COUNT = 5
    const val MAX_BYTES = 32 * 1024

    private var counter = 0L

    @Synchronized
    fun newId(nowMillis: Long = System.currentTimeMillis()): String = nowMillis.toString(36) + (counter++).toString(36)

    private fun label(name: String): String = name.trim().ifEmpty { "That file" }

    fun tooLarge(name: String): PromptAttachmentException =
        PromptAttachmentException("${label(name)} is larger than 32 KB, more than a prompt can carry.")

    fun unreadable(name: String): PromptAttachmentException =
        PromptAttachmentException("${label(name)} could not be read from this device.")

    fun read(name: String, bytes: ByteArray, id: String = newId()): PromptTextFile {
        val label = label(name)
        if (bytes.isEmpty()) throw PromptAttachmentException("$label is empty, so there is nothing to send with the prompt.")
        if (bytes.size > MAX_BYTES) throw tooLarge(label)
        val text = try {
            Charsets.UTF_8.newDecoder()
                .onMalformedInput(CodingErrorAction.REPORT)
                .onUnmappableCharacter(CodingErrorAction.REPORT)
                .decode(ByteBuffer.wrap(bytes))
                .toString()
        } catch (error: CharacterCodingException) {
            throw PromptAttachmentException(notText(label))
        }
        if ('\u0000' in text) throw PromptAttachmentException(notText(label))
        return PromptTextFile(id, label, text.replace("\r\n", "\n").removePrefix("﻿"))
    }

    fun fold(prompt: String, files: List<PromptTextFile>): String {
        val typed = prompt.trim()
        if (files.isEmpty()) return typed
        val parts = ArrayList<String>()
        if (typed.isNotEmpty()) parts.add(typed)
        parts.add(HEADER)
        for (file in files) {
            parts.add("===== BEGIN FILE ${file.name} =====\n${file.text}\n===== END FILE ${file.name} =====")
        }
        return parts.joinToString("\n\n")
    }

    fun cost(prompt: String, files: List<PromptTextFile>): Int = fold(prompt, files).length

    fun problem(
        adding: PromptTextFile,
        to: List<PromptTextFile>,
        prompt: String,
        budget: Int = AgentQueueApi.MAX_AGENT_PROMPT_CHARS,
    ): String? {
        if (to.size >= MAX_COUNT) return fullMessage()
        val used = cost(prompt, to + adding)
        if (used > budget) return "${adding.name} does not fit: a prompt holds $budget characters and this would make $used."
        return null
    }

    fun overBudget(prompt: String, files: List<PromptTextFile>, budget: Int = AgentQueueApi.MAX_AGENT_PROMPT_CHARS): String? {
        if (files.isEmpty()) return null
        val used = cost(prompt, files)
        if (used <= budget) return null
        return "The prompt and its files come to $used characters; an agent takes $budget. Remove a file or shorten the prompt."
    }

    fun fullMessage(): String = "A prompt can carry $MAX_COUNT files. Remove one to add another."

    fun sizeLabel(file: PromptTextFile): String = if (file.chars == 1) "1 character" else "${file.chars} characters"

    private fun notText(label: String): String =
        "$label is not a text file. An upload travels inside the prompt, so source, logs, diffs and notes arrive intact; images go through Photo library or Camera."
}
