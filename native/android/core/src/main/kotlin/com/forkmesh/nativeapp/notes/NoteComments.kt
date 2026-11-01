package com.forkmesh.nativeapp.notes

import com.forkmesh.nativeapp.format.relativeTime
import com.forkmesh.nativeapp.model.OrgApiException

data class NoteComment(
    val id: String,
    val parentId: String = "",
    val author: String = "",
    val body: String = "",
    val quote: String = "",
    val prefix: String = "",
    val suffix: String = "",
    val start: Int = 0,
    val end: Int = 0,
    val mentions: List<String> = emptyList(),
    val createdAt: Long = 0L,
    val updatedAt: Long = 0L,
    val editedAt: Long = 0L,
    val resolvedAt: Long = 0L,
    val resolvedBy: String = "",
) {
    val isThread: Boolean get() = parentId.isEmpty()
    val isResolved: Boolean get() = resolvedAt > 0
    val isInline: Boolean get() = quote.isNotEmpty()

    fun isBy(name: String): Boolean = name.isNotBlank() && author.equals(name.trim(), ignoreCase = true)

    fun canEdit(me: String): Boolean = isBy(me)

    fun canDelete(me: String, isOwner: Boolean): Boolean = isOwner || isBy(me)

    fun meta(now: Long = System.currentTimeMillis()): String =
        listOf(relativeTime(createdAt, now), if (editedAt > 0) "edited" else "").filter { it.isNotEmpty() }.joinToString(" · ")

    companion object {
        fun from(json: JsonObject): NoteComment? {
            val id = jsonString(json["id"]).trim()
            if (id.isEmpty()) return null
            val mentions = (json["mentions"] as? List<*>)?.mapNotNull { (it as? String)?.trim()?.takeIf(String::isNotEmpty) } ?: emptyList()
            return NoteComment(
                id = id,
                parentId = jsonString(json["parentId"]).trim(),
                author = jsonString(json["author"]).trim(),
                body = jsonString(json["body"]),
                quote = jsonString(json["quote"]),
                prefix = jsonString(json["prefix"]),
                suffix = jsonString(json["suffix"]),
                start = jsonInt(json["start"]).coerceIn(0L, Int.MAX_VALUE.toLong()).toInt(),
                end = jsonInt(json["end"]).coerceIn(0L, Int.MAX_VALUE.toLong()).toInt(),
                mentions = mentions,
                createdAt = jsonInt(json["createdAt"]),
                updatedAt = jsonInt(json["updatedAt"]),
                editedAt = jsonInt(json["editedAt"]),
                resolvedAt = jsonInt(json["resolvedAt"]),
                resolvedBy = jsonString(json["resolvedBy"]).trim(),
            )
        }

        fun list(value: Any?): List<NoteComment> = jsonObjects(value).mapNotNull(::from)
    }
}

data class NoteCommentsPayload(
    val comments: List<NoteComment> = emptyList(),
    val people: List<String> = emptyList(),
    val canResolve: Boolean = false,
    val comment: NoteComment? = null,
) {
    companion object {
        fun from(json: JsonObject): NoteCommentsPayload = NoteCommentsPayload(
            comments = NoteComment.list(json["comments"]),
            people = (json["people"] as? List<*>)?.mapNotNull { (it as? String)?.trim()?.takeIf(String::isNotEmpty) }?.distinctBy { it.lowercase() } ?: emptyList(),
            canResolve = json["canResolve"] == true,
            comment = (json["comment"] as? Map<*, *>)?.let { NoteComment.from(jsonObject(it)) },
        )
    }
}

data class NoteQuote(val quote: String, val prefix: String, val suffix: String, val start: Int, val end: Int) {
    fun body(): JsonObject = linkedMapOf("quote" to quote, "prefix" to prefix, "suffix" to suffix, "start" to start, "end" to end)
}

object NoteAnchoring {
    const val CONTEXT = 32
    const val MAX_QUOTE = 1000

    fun quote(text: String, start: Int, end: Int): NoteQuote? {
        val from = minOf(start, end).coerceIn(0, text.length)
        val to = maxOf(start, end).coerceIn(from, text.length)
        if (to <= from || text.substring(from, to).isBlank()) return null
        val stop = minOf(to, from + MAX_QUOTE)
        return NoteQuote(
            quote = text.substring(from, stop),
            prefix = text.substring(maxOf(0, from - CONTEXT), from),
            suffix = text.substring(stop, minOf(text.length, stop + CONTEXT)),
            start = from,
            end = stop,
        )
    }

    fun locate(text: String, quote: String, prefix: String, suffix: String, start: Int): IntRange? {
        if (quote.isEmpty()) return null
        var best = -1
        var bestScore = -1
        var bestDistance = Long.MAX_VALUE
        var at = text.indexOf(quote)
        while (at >= 0) {
            var score = 0
            if (at >= prefix.length && text.startsWith(prefix, at - prefix.length)) score += 2
            if (text.startsWith(suffix, at + quote.length)) score += 2
            val distance = kotlin.math.abs(at.toLong() - start.toLong())
            if (score > bestScore || (score == bestScore && distance < bestDistance)) {
                best = at
                bestScore = score
                bestDistance = distance
            }
            at = text.indexOf(quote, at + 1)
        }
        return if (best < 0) null else best until best + quote.length
    }

    fun locate(text: String, comment: NoteComment): IntRange? =
        locate(text, comment.quote, comment.prefix, comment.suffix, comment.start)
}

data class NoteThread(val comment: NoteComment, val replies: List<NoteComment>, val anchor: IntRange?) {
    val id: String get() = comment.id
    val isDetached: Boolean get() = comment.isInline && anchor == null
    val isWholeNote: Boolean get() = !comment.isInline
    val isResolved: Boolean get() = comment.isResolved
    val participants: List<String> get() = (listOf(comment.author) + replies.map { it.author }).filter { it.isNotEmpty() }.distinctBy { it.lowercase() }

    fun canResolve(canResolve: Boolean, me: String): Boolean = canResolve || comment.isBy(me)
}

data class NoteThreadList(val open: List<NoteThread>, val resolved: List<NoteThread>) {
    val openCount: Int get() = open.size
    val resolvedLabel: String get() = "Resolved (${resolved.size})"
    val all: List<NoteThread> get() = open + resolved

    fun find(commentId: String): NoteThread? =
        all.firstOrNull { thread -> thread.id == commentId || thread.replies.any { it.id == commentId } }
}

object NoteThreads {
    const val RELOAD_MS = 300L

    fun build(comments: List<NoteComment>, markdown: String): NoteThreadList {
        val ids = comments.filter { it.isThread }.map { it.id }.toSet()
        val replies = comments.filter { !it.isThread && it.parentId in ids }
            .sortedWith(compareBy<NoteComment> { it.createdAt }.thenBy { it.id })
            .groupBy { it.parentId }
        val threads = comments.filter { it.isThread }.map { comment ->
            NoteThread(comment, replies[comment.id].orEmpty(), if (comment.isInline) NoteAnchoring.locate(markdown, comment) else null)
        }
        val order = compareBy<NoteThread> { rank(it) }
            .thenBy { it.anchor?.first ?: 0 }
            .thenBy { it.comment.createdAt }
            .thenBy { it.id }
        return NoteThreadList(threads.filter { !it.isResolved }.sortedWith(order), threads.filter { it.isResolved }.sortedWith(order))
    }

    private fun rank(thread: NoteThread): Int = when {
        thread.isWholeNote -> 0
        thread.isDetached -> 2
        else -> 1
    }

    fun openCount(comments: List<NoteComment>): Int = comments.count { it.isThread && !it.isResolved }

    fun buttonLabel(openCount: Int): String = if (openCount > 0) "Comments ($openCount)" else "Comments"

    fun quoteLabel(thread: NoteThread): String = when {
        thread.isWholeNote -> "On the whole note"
        thread.isDetached -> "Removed from the note"
        else -> ""
    }
}

data class NoteMentionQuery(val start: Int, val end: Int, val query: String)

object NoteMentions {
    private val token = Regex("(?<![\\w@])@([a-zA-Z](?:[a-zA-Z0-9-]{0,61}[a-zA-Z0-9])?)")
    private val partial = Regex("(?<![\\w@])@([a-zA-Z0-9-]{0,63})$")
    const val MAX_SUGGESTIONS = 6

    fun ranges(body: String, mentions: List<String>): List<IntRange> {
        if (mentions.isEmpty()) return emptyList()
        val kept = mentions.map { it.lowercase() }.toSet()
        return token.findAll(body).filter { it.groupValues[1].lowercase() in kept }.map { it.range }.toList()
    }

    fun active(text: String, cursor: Int): NoteMentionQuery? {
        val at = cursor.coerceIn(0, text.length)
        val match = partial.find(text.substring(0, at)) ?: return null
        return NoteMentionQuery(match.range.first, at, match.groupValues[1])
    }

    fun suggestions(people: List<String>, query: String, me: String = ""): List<String> {
        val q = query.lowercase()
        val candidates = people.filter { it.isNotBlank() && !it.equals(me, ignoreCase = true) }.distinctBy { it.lowercase() }
        val starts = candidates.filter { it.lowercase().startsWith(q) }
        val contains = candidates.filter { q.isNotEmpty() && !it.lowercase().startsWith(q) && it.lowercase().contains(q) }
        return (starts + contains).take(MAX_SUGGESTIONS)
    }

    fun insert(text: String, mention: NoteMentionQuery, name: String): NoteEdit {
        val from = mention.start.coerceIn(0, text.length)
        val to = mention.end.coerceIn(from, text.length)
        val after = text.substring(to)
        val inserted = "@$name" + if (after.startsWith(" ")) "" else " "
        val cursor = from + inserted.length + if (after.startsWith(" ")) 1 else 0
        return NoteEdit(text.substring(0, from) + inserted + after, cursor, cursor)
    }
}

object NoteCommentText {
    const val MAX_BODY = 4000

    fun canSend(body: String): Boolean = body.isNotBlank() && body.trim().length <= MAX_BODY

    fun deleteTitle(comment: NoteComment): String = if (comment.isThread) "Delete thread?" else "Delete comment?"

    fun deletePrompt(comment: NoteComment, replies: Int, me: String): String {
        val whose = if (comment.isBy(me) || comment.author.isEmpty()) "" else "${comment.author}'s "
        if (!comment.isThread) return if (whose.isEmpty()) "This comment will be removed for everyone." else "${whose}comment will be removed for everyone."
        val subject = if (whose.isEmpty()) "This thread" else "${whose}thread"
        return when (replies) {
            0 -> "$subject will be removed for everyone."
            1 -> "$subject and its reply will be removed for everyone."
            else -> "$subject and its $replies replies will be removed for everyone."
        }
    }

    fun error(error: Throwable, fallback: String = "The comment was not saved."): String {
        if (error is OrgApiException) {
            when (error.code) {
                "invalid_comment" -> return "Write a comment of up to $MAX_BODY characters."
                "comment_not_found" -> return "That comment was deleted."
                "too_many_comments" -> return "This note has reached its limit of 1000 comments."
                "comment_author_required" -> return "Only the person who wrote a comment can change it."
                "resolve_not_allowed" -> return "Only the note's owner, its editors or the thread's author can resolve it."
            }
        }
        return apiErrorText(error, fallback)
    }
}
