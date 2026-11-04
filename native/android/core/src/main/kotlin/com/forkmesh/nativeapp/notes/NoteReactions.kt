package com.forkmesh.nativeapp.notes

import com.forkmesh.nativeapp.docs.DocsBlock

data class NoteReaction(
    val id: String,
    val author: String = "",
    val emoji: String,
    val quote: String = "",
    val prefix: String = "",
    val suffix: String = "",
    val start: Int = 0,
    val end: Int = 0,
    val createdAt: Long = 0L,
) {
    val isWhole: Boolean get() = quote.isEmpty()
    val anchor: NoteQuote? get() = if (isWhole) null else NoteQuote(quote, prefix, suffix, start, end)

    companion object {
        fun from(json: JsonObject): NoteReaction? {
            val id = jsonString(json["id"]).trim()
            val emoji = jsonString(json["emoji"]).trim()
            if (id.isEmpty() || emoji.isEmpty()) return null
            return NoteReaction(
                id = id,
                author = jsonString(json["author"]).trim(),
                emoji = emoji,
                quote = jsonString(json["quote"]),
                prefix = jsonString(json["prefix"]),
                suffix = jsonString(json["suffix"]),
                start = jsonInt(json["start"]).coerceIn(0L, Int.MAX_VALUE.toLong()).toInt(),
                end = jsonInt(json["end"]).coerceIn(0L, Int.MAX_VALUE.toLong()).toInt(),
                createdAt = jsonInt(json["createdAt"]),
            )
        }

        fun list(value: Any?): List<NoteReaction> = jsonObjects(value).mapNotNull(::from)
    }
}

data class NoteReactionChoice(val emoji: String, val label: String)

data class NoteReactionChip(val emoji: String, val people: List<String>, val mine: List<String>) {
    val count: Int get() = people.size
    val reacted: Boolean get() = mine.isNotEmpty()
    val label: String get() = "${people.joinToString(", ")} reacted with $emoji"
}

data class NoteReactionGroup(val range: IntRange?, val anchor: NoteQuote?, val chips: List<NoteReactionChip>) {
    val id: String get() = range?.let { "${it.first}-${it.last + 1}" } ?: "whole"
    val isWhole: Boolean get() = range == null
    val quote: String get() = anchor?.quote.orEmpty()
    val summary: String get() = chips.joinToString("  ") { "${it.emoji} ${it.count}" }
    val label: String get() = chips.joinToString(". ") { it.label }

    fun chip(emoji: String): NoteReactionChip? = chips.firstOrNull { it.emoji == emoji }
}

sealed class NoteReactionAction {
    data class Add(val emoji: String, val anchor: NoteQuote?) : NoteReactionAction()
    data class Remove(val ids: List<String>) : NoteReactionAction()
}

object NoteReactions {
    const val TITLE = "Reactions"
    const val WHOLE_NOTE = "On the whole note"
    const val REACT = "React"
    const val ADD_LABEL = "Add a reaction"
    private const val PROBE_LENGTH = 24

    val quickSet: List<NoteReactionChoice> = listOf(
        NoteReactionChoice("👍", "Thumbs up"),
        NoteReactionChoice("❤️", "Heart"),
        NoteReactionChoice("😂", "Laughing"),
        NoteReactionChoice("🎉", "Celebrate"),
        NoteReactionChoice("😮", "Surprised"),
        NoteReactionChoice("👀", "Looking"),
    )

    fun body(emoji: String, anchor: NoteQuote?): JsonObject {
        val body = linkedMapOf<String, Any?>("emoji" to emoji)
        anchor?.let { body.putAll(it.body()) }
        return body
    }

    fun groups(reactions: List<NoteReaction>, markdown: String, me: String): List<NoteReactionGroup> {
        val self = me.trim().lowercase()
        val whole = mutableListOf<NoteReaction>()
        val buckets = linkedMapOf<IntRange, MutableList<NoteReaction>>()
        for (reaction in reactions) {
            if (reaction.id.isEmpty() || reaction.emoji.isEmpty()) continue
            if (reaction.isWhole) {
                whole.add(reaction)
            } else {
                val range = NoteAnchoring.locate(markdown, reaction.quote, reaction.prefix, reaction.suffix, reaction.start) ?: continue
                buckets.getOrPut(range) { mutableListOf() }.add(reaction)
            }
        }
        val ordered = buckets.keys.sortedWith(compareBy<IntRange> { it.first }.thenBy { it.last })
        return buildList {
            if (whole.isNotEmpty()) add(group(null, whole, self))
            for (range in ordered) add(group(range, buckets.getValue(range), self))
        }
    }

    private fun group(range: IntRange?, reactions: List<NoteReaction>, me: String): NoteReactionGroup {
        val people = linkedMapOf<String, MutableList<String>>()
        val mine = linkedMapOf<String, MutableList<String>>()
        for (reaction in reactions) {
            val names = people.getOrPut(reaction.emoji) { mutableListOf() }
            val ids = mine.getOrPut(reaction.emoji) { mutableListOf() }
            val name = reaction.author.ifEmpty { "Someone" }
            if (names.none { it.equals(name, ignoreCase = true) }) names.add(name)
            if (me.isNotEmpty() && reaction.author.lowercase() == me) ids.add(reaction.id)
        }
        val chips = people.map { (emoji, names) -> NoteReactionChip(emoji, names, mine[emoji].orEmpty()) }
        return NoteReactionGroup(range, reactions.firstOrNull()?.anchor, chips)
    }

    fun action(chip: NoteReactionChip, group: NoteReactionGroup): NoteReactionAction =
        if (chip.reacted) NoteReactionAction.Remove(chip.mine) else NoteReactionAction.Add(chip.emoji, group.anchor)

    fun adding(emoji: String, group: NoteReactionGroup): NoteReactionAction? =
        if (group.chip(emoji)?.reacted == true) null else NoteReactionAction.Add(emoji, group.anchor)

    fun probe(block: DocsBlock): String? {
        val text = when (block) {
            is DocsBlock.Heading -> block.text
            is DocsBlock.Paragraph -> block.text
            is DocsBlock.ListItem -> block.text
            is DocsBlock.Quote -> return block.blocks.firstNotNullOfOrNull(::probe)
            is DocsBlock.Code -> block.text
            is DocsBlock.Table -> block.header.firstOrNull().orEmpty()
            is DocsBlock.Anchor, DocsBlock.Rule -> return null
        }
        return text.trim().ifEmpty { null }
    }

    fun blockStarts(probes: List<String?>, markdown: String): List<Int?> {
        var cursor = 0
        return probes.map { probe ->
            if (probe == null) return@map null
            val units = probe.take(PROBE_LENGTH)
            val word = units.takeWhile { !isSpace(it) }
            for (candidate in listOf(units, word)) {
                if (candidate.isEmpty()) continue
                val found = find(candidate, markdown, cursor) ?: continue
                cursor = found.second
                return@map found.first
            }
            null
        }
    }

    fun placed(groups: List<NoteReactionGroup>, starts: List<Int?>): List<List<NoteReactionGroup>> {
        val out = List(starts.size) { mutableListOf<NoteReactionGroup>() }
        val known = starts.mapIndexedNotNull { index, start -> start?.let { index to it } }
        val first = known.firstOrNull() ?: return out
        for (group in groups) {
            val range = group.range ?: continue
            val index = known.lastOrNull { it.second <= range.first }?.first ?: first.first
            out[index].add(group)
        }
        return out
    }

    private fun isSpace(c: Char): Boolean = c == ' ' || c == '\t' || c == '\n' || c == '\r'

    private fun find(probe: String, text: String, from: Int): Pair<Int, Int>? {
        var at = from.coerceAtLeast(0)
        while (at < text.length) {
            if (text[at] == probe[0]) match(probe, text, at)?.let { return at to it }
            at++
        }
        return null
    }

    private fun match(probe: String, text: String, at: Int): Int? {
        var i = 0
        var j = at
        while (i < probe.length) {
            if (isSpace(probe[i])) {
                if (j >= text.length || !isSpace(text[j])) return null
                while (i < probe.length && isSpace(probe[i])) i++
                while (j < text.length && isSpace(text[j])) j++
                continue
            }
            if (j >= text.length || text[j] != probe[i]) return null
            i++
            j++
        }
        return j
    }
}
