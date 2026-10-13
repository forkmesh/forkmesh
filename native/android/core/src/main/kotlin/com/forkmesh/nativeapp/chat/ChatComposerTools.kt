package com.forkmesh.nativeapp.chat

import com.forkmesh.nativeapp.api.PrefStore
import com.forkmesh.nativeapp.model.ChatMessage
import java.net.URI
import java.time.Instant
import java.time.ZoneOffset
import java.time.format.DateTimeFormatter

data class ChatMentionToken(val query: String, val start: Int, val end: Int)

data class ChatSlashCommand(val command: String, val usage: String, val summary: String)

sealed class ChatSlash {
    data class Issue(val argument: String) : ChatSlash()
    data class Topic(val topic: String) : ChatSlash()
}

data class ChatLinkInsertion(val text: String, val selectionStart: Int, val selectionLength: Int)

object ChatComposerTools {
    val emojiPicker: List<String> = listOf(
        "😀", "😁", "😂", "🤣", "😊", "😍", "😎",
        "😉", "🙂", "😢", "😭", "😡", "😱", "🤔",
        "😴", "🤯", "👍", "👎", "👏", "🙏", "💪",
        "🙌", "👋", "🤝", "🔥", "✨", "🎉", "💯",
        "❤️", "💔", "⭐", "✅", "❌", "👀", "🚀",
        "🐛", "💡", "📌", "☕", "🍕", "🎂", "🥳",
    )

    fun mentionToken(text: String, cursor: Int? = null): ChatMentionToken? {
        val end = (cursor ?: text.length).coerceIn(0, text.length)
        var start = end - 1
        while (start >= 0 && !text[start].isWhitespace() && text[start] != '@') start--
        if (start < 0 || text[start] != '@') return null
        if (start > 0 && !text[start - 1].isWhitespace()) return null
        return ChatMentionToken(text.substring(start + 1, end), start, end)
    }

    fun mentionCandidates(names: List<String>, excluding: String, query: String, limit: Int = 8): List<String> {
        val seen = HashSet<String>()
        val out = ArrayList<String>()
        val own = excluding.trim().lowercase()
        val folded = query.lowercase()
        for (raw in names) {
            val name = raw.trim()
            val key = name.lowercase()
            if (name.isEmpty() || key == own || !seen.add(key)) continue
            if (folded.isEmpty() || key.startsWith(folded)) out.add(name)
        }
        out.sortBy { it.lowercase() }
        return out.take(limit)
    }

    fun insertMention(text: String, name: String, cursor: Int? = null): String {
        if (name.isEmpty()) return text
        val token = mentionToken(text, cursor) ?: return text
        return text.substring(0, token.start) + "@$name " + text.substring(token.end)
    }

    fun slashCommands(text: String, canSetTopic: Boolean): List<ChatSlashCommand> {
        if (!text.startsWith("/") || !text.drop(1).all { it.isLetter() }) return emptyList()
        val all = ArrayList<ChatSlashCommand>()
        all.add(ChatSlashCommand("/issue", "/issue [owner/repo] <title>", "Create a repository issue"))
        if (canSetTopic) all.add(ChatSlashCommand("/topic", "/topic <text>", "Change this managed channel topic"))
        val prefix = text.lowercase()
        return all.filter { it.command.startsWith(prefix) }
    }

    fun parseSlash(text: String): ChatSlash? {
        val trimmed = text.trim()
        if (!trimmed.startsWith("/")) return null
        val body = trimmed.drop(1)
        val command = body.takeWhile { !it.isWhitespace() }.lowercase()
        val argument = body.drop(command.length).trim()
        return when (command) {
            "issue" -> ChatSlash.Issue(argument)
            "topic" -> ChatSlash.Topic(argument)
            else -> null
        }
    }

    fun sanitizedHref(raw: String): String {
        val trimmed = raw.trim()
        if (trimmed.isEmpty() || trimmed.length > 2048 || trimmed.any { it.isWhitespace() }) return ""
        val uri = try {
            URI(trimmed)
        } catch (_: Exception) {
            return ""
        }
        val scheme = uri.scheme?.lowercase() ?: return ""
        if (scheme != "https" && scheme != "http") return ""
        if (uri.host.isNullOrEmpty()) return ""
        return trimmed
    }

    fun linkInsertion(value: String, selectionStart: Int, selectionEnd: Int, clipboard: String = ""): ChatLinkInsertion {
        val start = selectionStart.coerceIn(0, value.length)
        val end = selectionEnd.coerceIn(start, value.length)
        val selected = value.substring(start, end)
        val haveTarget = sanitizedHref(clipboard).isNotEmpty()
        val label = selected.ifEmpty { "link text" }
        val target = if (haveTarget) clipboard.trim() else "https://"
        val text = value.substring(0, start) + "[" + label + "](" + target + ")" + value.substring(end)
        if (haveTarget && selected.isEmpty()) return ChatLinkInsertion(text, start + 1, label.length)
        if (haveTarget) return ChatLinkInsertion(text, start + 1 + label.length + 2 + target.length + 1, 0)
        return ChatLinkInsertion(text, start + 1 + label.length + 2, target.length)
    }

    fun mentionsForkbot(text: String): Boolean {
        val lower = text.lowercase()
        val needle = "forkbot"
        var index = lower.indexOf(needle)
        while (index >= 0) {
            var before = index - 1
            if (before >= 0 && lower[before] == '@') before--
            val leftOk = before < 0 || !(lower[before].isLetterOrDigit() || lower[before] == '_' || lower[before] == '-')
            val after = index + needle.length
            val rightOk = after >= lower.length || !(lower[after].isLetterOrDigit() || lower[after] == '_')
            if (leftOk && rightOk) return true
            index = lower.indexOf(needle, index + 1)
        }
        return false
    }

    fun forkbotContext(history: List<ChatMessage>, threadRootId: String = "", replies: List<ChatMessage> = emptyList()): List<Map<String, Any?>> {
        val source = if (threadRootId.isEmpty()) history else history.filter { it.id == threadRootId } + replies
        val context = ArrayList<Map<String, Any?>>()
        for (entry in source.dropLast(1).asReversed()) {
            if (context.size >= 12) break
            if (entry.senderId == "forkbot" || entry.text.isEmpty()) continue
            context.add(0, linkedMapOf("sender" to entry.senderName.take(32), "text" to entry.text.take(600)))
        }
        return context
    }

    fun forkbotPayload(message: String, sender: String, context: List<Map<String, Any?>>): Map<String, Any?> =
        linkedMapOf("message" to message, "sender" to sender, "room" to "general", "context" to context)
}

data class RepoSlug(val owner: String, val name: String) {
    val slug: String get() = "$owner/$name"

    companion object {
        fun parse(raw: String): RepoSlug? {
            val parts = raw.trim().split("/")
            if (parts.size != 2 || parts[0].isEmpty() || parts[1].isEmpty()) return null
            return RepoSlug(parts[0], parts[1])
        }
    }
}

data class ChatIssueDraft(val repo: RepoSlug, val title: String, val body: String) {
    sealed class Failure {
        data object Usage : Failure()
        data object NoRepositories : Failure()
        data class UnknownRepository(val slug: String) : Failure()
    }

    sealed class Result {
        data class Ok(val draft: ChatIssueDraft) : Result()
        data class Error(val failure: Failure) : Result()
    }

    companion object {
        private val isoFormat: DateTimeFormatter = DateTimeFormatter.ofPattern("yyyy-MM-dd'T'HH:mm:ss'Z'").withZone(ZoneOffset.UTC)

        private fun iso(ms: Long): String = isoFormat.format(Instant.ofEpochMilli(ms))

        private fun titleOf(text: String): String {
            val firstLine = text.split("\n").firstOrNull() ?: text
            var title = firstLine.split(Regex("\\s+")).filter { it.isNotEmpty() }.joinToString(" ")
            if (title.length > 200) title = title.take(199) + "…"
            return title
        }

        fun parse(argument: String, repos: List<RepoSlug>, current: RepoSlug?, channel: String, author: String, nowMs: Long): Result {
            var text = argument.trim()
            if (text.isEmpty()) return Result.Error(Failure.Usage)
            if (repos.isEmpty()) return Result.Error(Failure.NoRepositories)
            var repo = current?.let { c -> repos.firstOrNull { it.slug.lowercase() == c.slug.lowercase() } }
            val first = text.takeWhile { !it.isWhitespace() }
            if (isRepoToken(first)) {
                val named = repos.firstOrNull { it.slug.lowercase() == first.lowercase() }
                    ?: return Result.Error(Failure.UnknownRepository(first))
                repo = named
                text = text.drop(first.length).trim()
            }
            if (repo == null) repo = repos.firstOrNull { it.owner.lowercase() == "forkmesh" && it.name.lowercase() == "forkmesh" }
            val chosen = repo ?: repos.first()
            if (text.isEmpty()) return Result.Error(Failure.Usage)
            val who = author.trim().ifEmpty { "someone" }
            val place = if (channel.trim().isEmpty()) "" else channel.trim() + " "
            val body = text + "\n\n---\nFiled from a " + place + "chat message by " + who + " at " + iso(nowMs) + "."
            return Result.Ok(ChatIssueDraft(chosen, titleOf(text), body))
        }

        fun fromMessage(message: ChatMessage, text: String, repo: RepoSlug, channel: String): ChatIssueDraft {
            val title = titleOf(text).ifEmpty { "Chat message from ${message.senderName}" }
            val body = "> " + text.replace("\n", "\n> ") + "\n\n---\nFrom a " + channel + " chat message by " +
                message.senderName + " at " + iso(message.timestamp) + "."
            return ChatIssueDraft(repo, title, body)
        }

        fun isRepoToken(token: String): Boolean {
            val parts = token.split("/")
            if (parts.size != 2) return false
            val owner = parts[0]
            val name = parts[1]
            val head = owner.firstOrNull() ?: return false
            if (!(head in 'a'..'z') || owner.length > 63) return false
            if (!owner.all { it in 'a'..'z' || it.isDigit() || it == '-' }) return false
            if (name.isEmpty() || name.length > 100) return false
            return name.all { it.isLetterOrDigit() || it == '.' || it == '_' || it == '-' }
        }
    }
}

object ChatVisitorPresence {
    const val IDLE_WINDOW_MS = 600_000L

    fun isTransientVisitor(accountKind: String = "", name: String): Boolean {
        if (accountKind.trim().lowercase() == "guest") return true
        val lower = name.trim().lowercase()
        for (prefix in listOf("guest", "world guest", "world visitor")) {
            if (!lower.startsWith(prefix)) continue
            val rest = lower.drop(prefix.length)
            if (rest.isEmpty() || !(rest[0].isLetterOrDigit() || rest[0] == '_')) return true
        }
        return false
    }

    fun isIdle(lastSeenMs: Long?, nowMs: Long): Boolean {
        if (lastSeenMs == null) return false
        return nowMs - lastSeenMs > IDLE_WINDOW_MS
    }
}

class ChatWelcomeAnnouncer(private val prefs: PrefStore) {
    companion object {
        const val CHANNEL = "#welcome"
        const val TEXT = "👋 Just joined ForkMesh — hello!"
        const val FRESH_ACCOUNT_WINDOW_MS = 24L * 60 * 60 * 1000
    }

    private fun key(account: String) = "chat/welcomeAnnounced/${account.lowercase()}"

    fun shouldAnnounce(account: String, isGuest: Boolean, emailVerified: Boolean, createdAtMs: Long, nowMs: Long, alreadyInRoom: Boolean): Boolean {
        val name = account.trim()
        if (name.isEmpty() || isGuest || !emailVerified) return false
        if (prefs.getBool(key(name)) == true) return false
        val created = if (createdAtMs in 1 until 100_000_000_000L) createdAtMs * 1000 else createdAtMs
        if (alreadyInRoom || created <= 0 || nowMs - created > FRESH_ACCOUNT_WINDOW_MS) {
            prefs.putBool(key(name), true)
            return false
        }
        return true
    }

    fun markAnnounced(account: String) = prefs.putBool(key(account.trim()), true)
}
