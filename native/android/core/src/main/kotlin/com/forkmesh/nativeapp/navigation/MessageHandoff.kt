package com.forkmesh.nativeapp.navigation

object MessageHandoff {
    const val MAX_MESSAGE_LENGTH = 4000

    fun cleaned(message: String): String {
        val trimmed = message.trim()
        if (trimmed.length <= MAX_MESSAGE_LENGTH) return trimmed
        return trimmed.take(MAX_MESSAGE_LENGTH) + "…"
    }

    fun canHandOff(message: String): Boolean = cleaned(message).isNotEmpty()

    fun agentPrompt(message: String): String {
        val quoted = cleaned(message).split("\n").joinToString("\n") { "> $it" }
        return "The ForkMesh Android app showed this message:\n\n$quoted\n\nFind out what caused it and fix it."
    }

    fun composeTarget(message: String): ComposeTarget = ComposeTarget(text = agentPrompt(message))
}
