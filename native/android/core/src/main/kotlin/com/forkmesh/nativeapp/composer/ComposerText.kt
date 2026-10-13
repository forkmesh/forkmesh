package com.forkmesh.nativeapp.composer

import com.forkmesh.nativeapp.agents.AgentQueueApi
import com.forkmesh.nativeapp.api.PrefStore

data class SlashCommand(val name: String, val description: String)

object SlashCommands {
    val MODEL = SlashCommand("model", "Choose the model")
    val PHOTO = SlashCommand("photo", "Attach photos from your library")
    val CAMERA = SlashCommand("camera", "Take a photo to attach")
    val SCREENSHOT = SlashCommand("screenshot", "Capture this screen and mark it up")
    val DICTATE = SlashCommand("dictate", "Speak instead of typing")
    val FILE = SlashCommand("file", "Upload a text file into the prompt")

    fun builtIn(canPickModel: Boolean, canAttach: Boolean, canDictate: Boolean, canUpload: Boolean = false): List<SlashCommand> = buildList {
        if (canPickModel) add(MODEL)
        if (canAttach) {
            add(PHOTO)
            add(CAMERA)
            add(SCREENSHOT)
        }
        if (canUpload) add(FILE)
        if (canDictate) add(DICTATE)
    }

    fun matching(text: String, commands: List<SlashCommand>): List<SlashCommand> {
        if (!text.startsWith("/") || text.any { it.isWhitespace() }) return emptyList()
        val query = text.substring(1).lowercase()
        return commands.filter { it.name.startsWith(query) }
    }
}

object DictationText {
    fun merge(base: String, words: String): String {
        if (words.isEmpty()) return base
        val joiner = if (base.isEmpty() || base.last().isWhitespace()) "" else " "
        return base + joiner + words
    }
}

object ComposerRules {
    fun hasContent(text: String, attachmentCount: Int): Boolean = text.isNotBlank() || attachmentCount > 0

    fun showsStop(running: Boolean, text: String, attachmentCount: Int, canStop: Boolean): Boolean =
        running && canStop && !hasContent(text, attachmentCount)

    fun canSend(text: String, attachmentCount: Int, busy: Boolean, sendable: Boolean): Boolean =
        sendable && !busy && hasContent(text, attachmentCount)

    fun clamp(text: String, maxLength: Int = AgentQueueApi.MAX_AGENT_PROMPT_CHARS): String =
        if (text.length <= maxLength) text else text.substring(0, maxLength)

    const val FIX_LEAD = "Please fix"

    fun fixPrompt(text: String): String = text.trim().let { if (it.isEmpty()) FIX_LEAD else "$FIX_LEAD: $it" }
}

class PromptDraftStore(private val prefs: PrefStore) {
    fun load(surface: String): String = prefs.getString(key(surface)) ?: ""

    fun save(surface: String, text: String) {
        if (surface.isEmpty()) return
        if (text.isBlank()) {
            prefs.remove(key(surface))
            return
        }
        prefs.putString(key(surface), ComposerRules.clamp(text))
    }

    fun clear(surface: String) {
        if (surface.isNotEmpty()) prefs.remove(key(surface))
    }

    companion object {
        const val PREFIX = "composer/draft/"
        const val SHELL = "shell"
        const val NEW_AGENT = "new-agent"

        fun key(surface: String): String = PREFIX + surface.trim().lowercase()

        fun agentSurface(sessionKey: String): String = "agent/$sessionKey"
    }
}
