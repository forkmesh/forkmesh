package com.forkmesh.nativeapp.aquarium

import kotlin.math.abs
import kotlin.math.max
import kotlin.math.min

enum class GenieLandmarkKind {
    BUTTON, LINK, TAB, FIELD, TOGGLE, SLIDER, IMAGE, HEADING, TEXT, PROGRESS;

    val keepsClear: Boolean
        get() = when (this) {
            BUTTON, LINK, TAB, FIELD, TOGGLE, SLIDER, IMAGE -> true
            HEADING, TEXT, PROGRESS -> false
        }
}

data class GenieLandmark(val kind: GenieLandmarkKind, val rect: AquariumRect, val text: String = "") {
    val keepsClear: Boolean get() = kind.keepsClear

    val center: AquariumPoint get() = AquariumPoint(rect.midX, rect.midY)
}

object GenieLandmarks {
    const val CAP = 300
    const val REFRESH = 10.0
    const val POLL = 2.0
    const val SMALLEST = 6.0
    const val LARGEST = 0.35
    const val LONGEST = 80
    const val SHORTEST = 18

    private val breaks = Regex("[\n\r\u000B\u000C\u0085  \t]")

    private fun blankEdge(char: Char): Boolean = char == ' ' || char == '\t' || char == ' '

    fun clean(raw: String): String {
        val flat = raw.split(breaks).filter { it.isNotEmpty() }.joinToString(" ")
        val trimmed = flat.trim(::blankEdge)
        return if (trimmed.length > LONGEST) trimmed.take(LONGEST) else trimmed
    }

    fun short(text: String): String {
        val clean = clean(text)
        if (clean.length <= SHORTEST) return clean
        return clean.take(SHORTEST - 1).trim(::blankEdge) + "…"
    }

    fun tidy(raw: List<GenieLandmark>, screen: AquariumRect, cap: Int = CAP): List<GenieLandmark> {
        val area = screen.width * screen.height
        if (area <= 0) return emptyList()
        val kept = ArrayList<GenieLandmark>()
        for (mark in raw) {
            val clipped = mark.rect.intersection(screen) ?: continue
            if (clipped.width < SMALLEST || clipped.height < SMALLEST) continue
            val share = clipped.width * clipped.height / area
            if (share > (if (mark.keepsClear) LARGEST else 0.5)) continue
            val next = GenieLandmark(mark.kind, clipped, clean(mark.text))
            if (kept.any { it.kind == next.kind && same(it.rect, next.rect) }) continue
            kept.add(next)
        }
        val blockers = kept.filter { it.keepsClear }
        val others = kept.filter { !it.keepsClear }
        return (blockers + others).take(max(cap, 0))
    }

    fun blockers(marks: List<GenieLandmark>): List<AquariumRect> = marks.filter { it.keepsClear }.map { it.rect }

    fun overlaps(box: AquariumRect, rects: List<AquariumRect>, slack: Double = 1.0): Boolean =
        rects.any { rect ->
            val cut = rect.intersection(box)
            cut != null && cut.width > slack && cut.height > slack
        }

    fun same(a: AquariumRect, b: AquariumRect): Boolean =
        abs(a.minX - b.minX) <= 2 && abs(a.minY - b.minY) <= 2 && abs(a.maxX - b.maxX) <= 2 && abs(a.maxY - b.maxY) <= 2
}

class GenieBag<Item>(val items: List<Item>) {
    private var left: MutableList<Item> = ArrayList()
    var last: Item? = null
        private set

    fun next(random: () -> Double, fits: (Item) -> Boolean = { true }): Item? {
        if (left.isEmpty()) refill(random)
        take(fits)?.let { return it }
        if (items.none(fits)) return null
        refill(random)
        return take(fits)
    }

    private fun take(fits: (Item) -> Boolean): Item? {
        val index = left.indexOfFirst(fits)
        if (index < 0) return null
        val item = left.removeAt(index)
        last = item
        return item
    }

    private fun refill(random: () -> Double) {
        val deck = items.toMutableList()
        if (deck.size > 1) {
            for (index in deck.size - 1 downTo 1) {
                val other = min((random() * (index + 1)).toInt(), index)
                val held = deck[index]
                deck[index] = deck[other]
                deck[other] = held
            }
            val previous = last
            if (previous != null && deck.first() == previous) deck.add(deck.removeAt(0))
        }
        left = deck
    }
}
