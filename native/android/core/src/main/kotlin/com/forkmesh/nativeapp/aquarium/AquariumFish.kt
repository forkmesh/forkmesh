package com.forkmesh.nativeapp.aquarium

import kotlin.math.hypot
import kotlin.math.max
import kotlin.math.min

data class AquariumSwimmer(val id: String, val name: String, val model: String, val waiting: Boolean = false)

enum class AquariumSpecies(val height: Double) {
    GOLDFISH(0.56), CLOWNFISH(0.5), ANGELFISH(0.95), PUFFER(0.8), TETRA(0.4), BETTA(0.62)
}

data class AquariumColor(val red: Double, val green: Double, val blue: Double) {
    fun shifted(amount: Double, seed: Long): AquariumColor {
        val a = amount * ((seed and 0xff).toDouble() / 255 - 0.5)
        val b = amount * (((seed ushr 8) and 0xff).toDouble() / 255 - 0.5)
        val c = amount * (((seed ushr 16) and 0xff).toDouble() / 255 - 0.5)
        return of(red + a, green + b, blue + c)
    }

    companion object {
        fun of(red: Double, green: Double, blue: Double): AquariumColor =
            AquariumColor(min(max(red, 0.0), 1.0), min(max(green, 0.0), 1.0), min(max(blue, 0.0), 1.0))

        val GOLD = AquariumColor(1.0, 0.82, 0.3)
        val WHITE = AquariumColor(1.0, 1.0, 1.0)
    }
}

data class AquariumLook(
    val species: AquariumSpecies,
    val body: AquariumColor,
    val fin: AquariumColor,
    val accent: AquariumColor,
    val size: Double,
) {
    companion object {
        fun hash(text: String): Long {
            var value = 0xcbf29ce484222325uL.toLong()
            for (byte in text.toByteArray(Charsets.UTF_8)) {
                value = value xor (byte.toLong() and 0xff)
                value *= 0x100000001b3L
            }
            return value
        }

        fun of(name: String, model: String): AquariumLook {
            val seed = hash("$name|$model")
            val all = AquariumSpecies.entries
            val species = all[java.lang.Long.remainderUnsigned(seed, all.size.toLong()).toInt()]
            val base = palette(species)
            val size = 0.85 + ((seed ushr 40) % 1000).toDouble() / 1000 * 0.35
            return AquariumLook(
                species,
                base.body.shifted(0.16, seed ushr 8),
                base.fin.shifted(0.12, seed ushr 24),
                base.accent,
                size,
            )
        }

        fun palette(species: AquariumSpecies): AquariumLook = when (species) {
            AquariumSpecies.GOLDFISH -> AquariumLook(species, AquariumColor(1.0, 0.55, 0.15), AquariumColor(1.0, 0.76, 0.32), AquariumColor(1.0, 0.9, 0.6), 1.0)
            AquariumSpecies.CLOWNFISH -> AquariumLook(species, AquariumColor(1.0, 0.45, 0.1), AquariumColor(0.95, 0.35, 0.08), AquariumColor.WHITE, 1.0)
            AquariumSpecies.ANGELFISH -> AquariumLook(species, AquariumColor(0.86, 0.88, 0.93), AquariumColor(0.98, 0.85, 0.35), AquariumColor(0.18, 0.18, 0.24), 1.0)
            AquariumSpecies.PUFFER -> AquariumLook(species, AquariumColor(0.88, 0.8, 0.52), AquariumColor(0.75, 0.68, 0.42), AquariumColor(0.42, 0.3, 0.16), 1.0)
            AquariumSpecies.TETRA -> AquariumLook(species, AquariumColor(0.62, 0.72, 0.85), AquariumColor(0.9, 0.18, 0.22), AquariumColor(0.18, 0.62, 1.0), 1.0)
            AquariumSpecies.BETTA -> AquariumLook(species, AquariumColor(0.55, 0.25, 0.85), AquariumColor(0.86, 0.22, 0.56), AquariumColor(0.35, 0.55, 1.0), 1.0)
        }
    }
}

enum class AquariumTrick(val withSpider: Boolean = false) {
    PEEK, BOOP, READ, UNDERLINE, POLISH, BUBBLE_RING, MEASURE, HIGHLIGHT, QUOTE, COUNT, STARTLE, PERCH, TRACE,
    HELLO(true), NOSE_BOOP(true), TAG(true), ORBIT(true), BUBBLE_CROWN(true), HIGH_FIVE(true), RACE(true),
    SERENADE(true), LEG_COUNT(true), PHOTO(true), KISS(true);

    val title: String
        get() = when (this) {
            PEEK -> "peeks at"
            BOOP -> "boops"
            READ -> "reads"
            UNDERLINE -> "underlines"
            POLISH -> "polishes"
            BUBBLE_RING -> "blows a bubble ring round"
            MEASURE -> "measures"
            HIGHLIGHT -> "highlights"
            QUOTE -> "quotes"
            COUNT -> "counts the letters of"
            STARTLE -> "gets startled by"
            PERCH -> "naps beside"
            TRACE -> "traces"
            HELLO -> "says hello to the spider"
            NOSE_BOOP -> "boops the spider's nose"
            TAG -> "plays tag with the spider"
            ORBIT -> "swims rings round the spider"
            BUBBLE_CROWN -> "gives the spider a bubble crown"
            HIGH_FIVE -> "high-fives the spider"
            RACE -> "races the spider"
            SERENADE -> "sings to the spider"
            LEG_COUNT -> "counts the spider's legs"
            PHOTO -> "takes the spider's photo"
            KISS -> "kisses the spider"
        }

    val fits: Set<GenieLandmarkKind>
        get() = when (this) {
            PEEK -> setOf(GenieLandmarkKind.BUTTON, GenieLandmarkKind.TAB, GenieLandmarkKind.TOGGLE, GenieLandmarkKind.IMAGE, GenieLandmarkKind.LINK)
            BOOP -> setOf(GenieLandmarkKind.BUTTON, GenieLandmarkKind.TAB, GenieLandmarkKind.LINK)
            READ -> setOf(GenieLandmarkKind.HEADING, GenieLandmarkKind.TEXT)
            UNDERLINE -> setOf(GenieLandmarkKind.TEXT, GenieLandmarkKind.HEADING)
            POLISH -> setOf(GenieLandmarkKind.BUTTON)
            BUBBLE_RING -> setOf(GenieLandmarkKind.BUTTON, GenieLandmarkKind.TOGGLE, GenieLandmarkKind.IMAGE)
            MEASURE -> GenieLandmarkKind.entries.toSet()
            HIGHLIGHT -> setOf(GenieLandmarkKind.TEXT, GenieLandmarkKind.HEADING)
            QUOTE -> setOf(GenieLandmarkKind.TEXT, GenieLandmarkKind.HEADING, GenieLandmarkKind.BUTTON)
            COUNT -> setOf(GenieLandmarkKind.TEXT, GenieLandmarkKind.HEADING, GenieLandmarkKind.BUTTON)
            STARTLE -> setOf(GenieLandmarkKind.BUTTON, GenieLandmarkKind.LINK)
            PERCH -> setOf(GenieLandmarkKind.BUTTON, GenieLandmarkKind.HEADING)
            TRACE -> setOf(GenieLandmarkKind.BUTTON, GenieLandmarkKind.IMAGE, GenieLandmarkKind.FIELD)
            else -> emptySet()
        }

    fun duration(text: String): Double {
        val letters = max(letters(text).size, 1)
        return when (this) {
            PEEK -> 2.4
            BOOP -> 2.2
            READ -> min(0.8 + min(letters, 24) * 0.14, 4.2)
            UNDERLINE -> 2.0
            POLISH -> 2.4
            BUBBLE_RING -> 2.4
            MEASURE -> 2.6
            HIGHLIGHT -> 2.2
            QUOTE -> 2.6
            COUNT -> min(0.9 + letters * 0.18, 5.4)
            STARTLE -> 1.9
            PERCH -> 3.0
            TRACE -> 2.6
            HELLO -> 2.2
            NOSE_BOOP -> 1.8
            TAG -> 2.6
            ORBIT -> 3.2
            BUBBLE_CROWN -> 2.6
            HIGH_FIVE -> 1.8
            RACE -> 3.6
            SERENADE -> 2.6
            LEG_COUNT -> 3.6
            PHOTO -> 2.6
            KISS -> 2.0
        }
    }

    fun fits(mark: GenieLandmark): Boolean {
        if (mark.kind !in fits) return false
        val letters = letters(mark.text).size
        return when (this) {
            READ, UNDERLINE, HIGHLIGHT, QUOTE -> letters > 0
            COUNT -> letters in 1..24
            MEASURE -> mark.rect.width >= 60 && mark.rect.width <= 900
            BUBBLE_RING -> mark.kind != GenieLandmarkKind.BUTTON || mark.rect.width <= 90
            else -> true
        }
    }

    companion object {
        val element: List<AquariumTrick> = entries.filter { !it.withSpider }
        val spider: List<AquariumTrick> = entries.filter { it.withSpider }

        fun letters(text: String): List<Char> = text.filter { it.isLetter() || it.isDigit() }.toList()
    }
}

enum class AquariumPhase { ENTERING, WANDERING, GOING, ACTING, LEAVING }

sealed class AquariumBit {
    object Bubble : AquariumBit()
    object Sparkle : AquariumBit()
    object Heart : AquariumBit()
    object Star : AquariumBit()
    object Note : AquariumBit()
    object Zzz : AquariumBit()
    object Flash : AquariumBit()
    object Photo : AquariumBit()
    object Speed : AquariumBit()
    object Splash : AquariumBit()
    data class Glyph(val text: String) : AquariumBit()
}

class AquariumParticle(
    var bit: AquariumBit,
    var point: AquariumPoint,
    var velocity: AquariumSize = AquariumSize.ZERO,
    life: Double,
    var size: Double,
) {
    var age: Double = 0.0
    var life: Double = max(life, 0.05)
    var center: AquariumPoint? = null
    var radius: Double = 0.0
    var angle: Double = 0.0
    var spin: Double = 0.0

    val progress: Double get() = min(max(age / life, 0.0), 1.0)
}

sealed class AquariumMarkKind {
    object Ring : AquariumMarkKind()
    object Underline : AquariumMarkKind()
    object Highlight : AquariumMarkKind()
    object Trace : AquariumMarkKind()
    data class Tape(val label: String) : AquariumMarkKind()
}

class AquariumMark(val kind: AquariumMarkKind, val rect: AquariumRect, life: Double, val tint: AquariumColor) {
    var age: Double = 0.0
    val life: Double = max(life, 0.05)

    val progress: Double get() = min(max(age / this.life, 0.0), 1.0)
}

class AquariumAct(val trick: AquariumTrick, var target: AquariumRect, val text: String, var spot: AquariumPoint) {
    var start: Double = 0.0
    val duration: Double = trick.duration(text)
    val fired: MutableSet<Int> = HashSet()
    var away: AquariumPoint? = null
    var angle: Double = 0.0

    fun beat(index: Int, at: Double, age: Double): Boolean {
        if (age < at || index in fired) return false
        fired.add(index)
        return true
    }
}

class AquariumFish(val id: String, var name: String, model: String, var waiting: Boolean, at: AquariumPoint) {
    var look: AquariumLook = AquariumLook.of(name, model)
    var point: AquariumPoint = at
    var velocity: AquariumSize = AquariumSize.ZERO
    var goal: AquariumPoint = at
    var phase: AquariumPhase = AquariumPhase.ENTERING
    var act: AquariumAct? = null
    var restUntil: Double = 0.0
    var deadline: Double = 0.0
    var speech: String? = null
    var speechUntil: Double = 0.0
    var speechBox: AquariumRect? = null
    var opacity: Double = 1.0
    var appear: Double = 0.0
    var facingLeft: Boolean = false
    var swim: Double = 0.0
    var nextCall: Double = 0.0
    var nudge: AquariumSize = AquariumSize.ZERO

    val length: Double get() = BASE_LENGTH * look.size

    val height: Double get() = length * look.species.height

    val label: String
        get() {
            val clean = GenieLandmarks.clean(name)
            if (clean.length <= LONGEST_LABEL) return clean
            return clean.take(LONGEST_LABEL - 1).trim { it == ' ' || it == '\t' } + "…"
        }

    val drawn: AquariumPoint get() = AquariumPoint(point.x + nudge.width, point.y + nudge.height)

    fun body(center: AquariumPoint): AquariumRect {
        val width = length * 1.2 + 6
        val above = height / 2 + 4 + (if (waiting) 16 else 0)
        val below = height / 2 + 2 + LABEL_HEIGHT
        return AquariumRect(center.x - width / 2, center.y - above, width, above + below)
    }

    val box: AquariumRect get() = body(drawn)

    companion object {
        const val BASE_LENGTH = 34.0
        const val LABEL_HEIGHT = 14.0
        const val LONGEST_LABEL = 16
    }
}

data class AquariumSpider(val point: AquariumPoint, val side: Double, val free: Boolean) {
    val rect: AquariumRect
        get() {
            val half = side * 0.4
            return AquariumRect(point.x - half, point.y - half, half * 2, half * 2)
        }
}

sealed class AquariumSpiderAsk {
    object Wave : AquariumSpiderAsk()
    object Cheer : AquariumSpiderAsk()
    object Giggle : AquariumSpiderAsk()
    object Surprise : AquariumSpiderAsk()
    object Eureka : AquariumSpiderAsk()
    object Dizzy : AquariumSpiderAsk()
    object Nod : AquariumSpiderAsk()
    data class Walk(val point: AquariumPoint) : AquariumSpiderAsk()
    data class Look(val point: AquariumPoint) : AquariumSpiderAsk()
}

sealed class AquariumEvent {
    data class Spider(val ask: AquariumSpiderAsk) : AquariumEvent()
    data class Log(val text: String) : AquariumEvent()
}

object AquariumClear {
    val GAPS = listOf(10.0, 22.0, 40.0)
    const val ROOM = 8.0

    fun fits(box: AquariumRect, water: AquariumRect): Boolean =
        box.minX >= water.minX && box.maxX <= water.maxX && box.minY >= water.minY && box.maxY <= water.maxY

    fun clear(box: AquariumRect, blockers: List<AquariumRect>, water: AquariumRect, room: Double = 0.0): Boolean =
        fits(box, water) && !GenieLandmarks.overlaps(box.insetBy(-room, -room), blockers)

    fun spot(
        beside: AquariumRect,
        body: (AquariumPoint) -> AquariumRect,
        blockers: List<AquariumRect>,
        water: AquariumRect,
        prefer: AquariumPoint,
    ): AquariumPoint? {
        val sample = body(AquariumPoint.ZERO)
        val left = -sample.minX
        val right = sample.maxX
        val up = -sample.minY
        val down = sample.maxY
        val options = ArrayList<AquariumPoint>()
        for (gap in GAPS) {
            val xs = listOf(beside.minX - gap - right, beside.maxX + gap + left)
            val ys = listOf(beside.minY - gap - down, beside.maxY + gap + up)
            for (x in xs) options.add(AquariumPoint(x, beside.midY))
            for (y in ys) {
                options.add(AquariumPoint(beside.midX, y))
                if (beside.width > sample.width * 2) {
                    options.add(AquariumPoint(beside.minX + right, y))
                    options.add(AquariumPoint(beside.maxX - left, y))
                }
            }
            for (x in xs) {
                for (y in ys) options.add(AquariumPoint(x, y))
            }
        }
        val avoid = blockers + beside
        return options.filter { clear(body(it), avoid, water, ROOM) }
            .minByOrNull { hypot(it.x - prefer.x, it.y - prefer.y) }
    }

    fun waypoint(
        area: AquariumRect,
        body: (AquariumPoint) -> AquariumRect,
        blockers: List<AquariumRect>,
        water: AquariumRect,
        random: () -> Double,
    ): AquariumPoint {
        var last = AquariumPoint(area.midX, area.midY)
        repeat(18) {
            val point = AquariumPoint(area.minX + area.width * random(), area.minY + area.height * random())
            last = point
            if (clear(body(point), blockers, water, ROOM)) return point
        }
        return last
    }

    fun speech(text: String, body: AquariumRect, blockers: List<AquariumRect>, screen: AquariumRect): AquariumRect? {
        val width = min(max(text.length * 6.6 + 20, 32.0), 200.0)
        val height = 24.0
        val options = listOf(
            AquariumRect(body.midX - width / 2, body.minY - 4 - height, width, height),
            AquariumRect(body.maxX + 4, body.midY - height / 2, width, height),
            AquariumRect(body.minX - 4 - width, body.midY - height / 2, width, height),
            AquariumRect(body.midX - width / 2, body.maxY + 4, width, height),
        )
        return options.firstOrNull { clear(it, blockers, screen) }
    }

    fun opacity(box: AquariumRect, blockers: List<AquariumRect>): Double =
        if (GenieLandmarks.overlaps(box, blockers)) AquariumTank.CROSSING else 1.0
}
