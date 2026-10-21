package com.forkmesh.nativeapp.chat

import com.forkmesh.nativeapp.navigation.ProfileLinks

object ChatProfileLink {
    fun senderProfile(row: ChatRow): String? {
        val message = row.message
        if (message.self || row.visitor) return null
        if (message.senderId == "forkbot") return null
        if (message.senderId.startsWith("app:")) return null
        return message.senderName.trim().takeIf { it.isNotEmpty() }
    }

    fun mentionLink(pieces: List<ChatMentionPiece>, index: Int): String? {
        val piece = pieces.getOrNull(index) ?: return null
        if (piece.kind != ChatMentionKind.Person) return null
        if (pieces.getOrNull(index + 1)?.text?.startsWith("[bot]", ignoreCase = true) == true) return null
        return ProfileLinks.link(piece.text)
    }
}

enum class ChatMentionKind { Text, Person, You, Broadcast }

data class ChatMentionPiece(val text: String, val kind: ChatMentionKind = ChatMentionKind.Text)

object ChatSmartMentions {
    private val broadcasts = setOf("here", "channel", "everyone")

    private fun isNameCharacter(c: Char): Boolean = c.isLetterOrDigit() || c == '_' || c == '-' || c == '.'

    private fun opensMention(text: String, index: Int): Boolean {
        if (text[index] != '@') return false
        if (index == 0) return true
        val before = text[index - 1]
        return before.isWhitespace() || before == '(' || before == '[' || before == '"' || before == '\''
    }

    fun split(text: String, own: List<String>): List<ChatMentionPiece> {
        val mine = own.map { it.trim().lowercase() }.filter { it.isNotEmpty() }.toSet()
        val pieces = ArrayList<ChatMentionPiece>()
        val buffer = StringBuilder()
        var index = 0
        var inCode = false
        while (index < text.length) {
            if (text[index] == '`') inCode = !inCode
            if (!inCode && opensMention(text, index)) {
                var end = index + 1
                while (end < text.length && isNameCharacter(text[end])) end++
                var name = text.substring(index + 1, end)
                while (name.endsWith(".") || name.endsWith("-")) {
                    name = name.dropLast(1)
                    end--
                }
                if (name.isNotEmpty()) {
                    val lower = name.lowercase()
                    val kind = when {
                        lower in broadcasts -> ChatMentionKind.Broadcast
                        lower in mine -> ChatMentionKind.You
                        else -> ChatMentionKind.Person
                    }
                    if (buffer.isNotEmpty()) {
                        pieces.add(ChatMentionPiece(buffer.toString()))
                        buffer.setLength(0)
                    }
                    pieces.add(ChatMentionPiece("@$name", kind))
                    index = end
                    continue
                }
            }
            buffer.append(text[index])
            index++
        }
        if (buffer.isNotEmpty()) pieces.add(ChatMentionPiece(buffer.toString()))
        return pieces
    }
}
