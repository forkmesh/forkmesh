package com.forkmesh.nativeapp.djstudio

import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.json.JsonMap
import com.forkmesh.nativeapp.json.asJsonMap
import com.forkmesh.nativeapp.json.jsonMaps
import java.net.URI
import java.net.URLDecoder
import kotlin.math.PI
import kotlin.math.abs
import kotlin.math.cos
import kotlin.math.exp
import kotlin.math.floor
import kotlin.math.max
import kotlin.math.min
import kotlin.math.pow
import kotlin.math.sin
import kotlin.math.sqrt

enum class DjDeckSide(val key: String) {
    A("a"), B("b");

    val other: DjDeckSide get() = if (this == A) B else A
    val label: String get() = key.uppercase()

    companion object {
        fun of(key: String): DjDeckSide? = entries.firstOrNull { it.key == key }
    }
}

data class DjWaveBar(val level: Double, val hue: Double)

sealed interface DjSampleStep {
    data object Idle : DjSampleStep
    data class Recording(val start: Double) : DjSampleStep
    data class Armed(val slot: Int) : DjSampleStep
}

enum class DjLoadTarget(val key: String) {
    A("a"), B("b"), QUEUE("queue");

    val side: DjDeckSide?
        get() = when (this) {
            A -> DjDeckSide.A
            B -> DjDeckSide.B
            QUEUE -> null
        }

    val label: String get() = side?.let { "Deck ${it.label}" } ?: "Queue"

    companion object {
        fun of(side: DjDeckSide): DjLoadTarget = if (side == DjDeckSide.A) A else B
    }
}

data class DjSearchResult(val id: String, val title: String, val author: String, val seconds: Double = 0.0) {
    val thumbnail: String get() = DjStudio.thumbnailUrl(id, "mqdefault")
}

enum class DjDrumPad(val key: String) {
    KICK("kick"), SNARE("snare"), HAT("hat"), CLAP("clap"), TOM("tom"), PERC("perc"), RISER("riser"), STAB("stab");

    val label: String get() = key.replaceFirstChar { it.uppercase() }
}

object DjStudio {
    const val SECONDS_PER_TURN = 1.8
    const val PITCH_RANGE = 0.16
    const val BEND_AMOUNT = 0.04
    const val LOOP_SECONDS = 8.0
    const val SECTION_SECONDS = 8.0
    const val SECTION_COUNT = 4
    const val AUTOMIX_SECONDS = 8.0
    const val RATE_LOW = 0.25
    const val RATE_HIGH = 2.0
    const val STORAGE_KEY = "fm.videoMixDjStudio.v1"
    const val HISTORY_LIMIT = 40
    const val LIBRARY_LIMIT = 500
    const val QUEUE_LIMIT = 100
    const val TOUCH_BRAKE = 0.08
    const val TOUCH_BRAKE_SECONDS = 0.35
    const val SCRATCH_THRESHOLD = 14.0
    const val SHORTEST_SAMPLE = 0.25

    private val linkPattern = Regex(
        "(?:youtu\\.be/|youtube(?:-nocookie)?\\.com/(?:watch\\?(?:[^#\\s]*&)?v=|(?:embed|shorts|live|v|e)/))([A-Za-z0-9_-]{11})",
        RegexOption.IGNORE_CASE,
    )
    private val bareHost = Regex("^(?:www\\.|m\\.|music\\.)?(?:youtube(?:-nocookie)?\\.com|youtu\\.be)/", RegexOption.IGNORE_CASE)
    private val hostPrefix = Regex("^(?:www\\.|m\\.|music\\.)")
    private val fragmentId = Regex("[?&]v=[A-Za-z0-9_-]{11}")

    fun isVideoId(value: String): Boolean =
        value.length == 11 && value.all { it in 'a'..'z' || it in 'A'..'Z' || it in '0'..'9' || it == '_' || it == '-' }

    fun videoId(value: String): String {
        var raw = value.trim()
        if (raw.startsWith("<")) raw = raw.drop(1)
        if (raw.endsWith(">")) raw = raw.dropLast(1)
        if (isVideoId(raw)) return raw
        var text = raw
        if (bareHost.containsMatchIn(text)) text = "https://$text"
        val uri = runCatching { URI(text) }.getOrNull()
        val found = uri?.host?.lowercase()
        if (uri != null && found != null) {
            val host = found.replace(hostPrefix, "")
            val items = query(uri.rawQuery)
            val item = { name: String -> items.firstOrNull { it.first == name }?.second }
            val path = uri.rawPath.orEmpty()
            val segments = path.split('/').filter { it.isNotEmpty() }
            if (host == "youtu.be") {
                val id = segments.firstOrNull() ?: item("v") ?: ""
                return if (isVideoId(id)) id else ""
            }
            if (host == "youtube.com" || host == "youtube-nocookie.com") {
                val candidates = mutableListOf(item("v"), item("vi"), item("video_id"))
                val inner = item("u")
                if (path == "/attribution_link" && inner != null) {
                    val nested = if (inner.startsWith("/")) inner else "/$inner"
                    val nestedUri = runCatching { URI("https://www.youtube.com$nested") }.getOrNull()
                    candidates.add(query(nestedUri?.rawQuery).firstOrNull { it.first == "v" }?.second)
                }
                val index = segments.indexOfFirst { it in listOf("embed", "shorts", "live", "v", "e", "watch") }
                if (index >= 0 && index + 1 < segments.size) candidates.add(segments[index + 1])
                val fragment = uri.rawFragment
                if (fragment != null) {
                    fragmentId.find(fragment)?.let { candidates.add(it.value.takeLast(11)) }
                }
                return candidates.filterNotNull().firstOrNull(::isVideoId) ?: ""
            }
        }
        return linkPattern.find(raw)?.groupValues?.get(1) ?: ""
    }

    private fun query(raw: String?): List<Pair<String, String>> {
        if (raw.isNullOrEmpty()) return emptyList()
        return raw.split('&').filter { it.isNotEmpty() }.map { part ->
            val equals = part.indexOf('=')
            val name = if (equals < 0) part else part.substring(0, equals)
            val value = if (equals < 0) "" else part.substring(equals + 1)
            decode(name) to decode(value)
        }
    }

    private fun decode(text: String): String = runCatching { URLDecoder.decode(text, "UTF-8") }.getOrDefault(text)

    fun sharedVideoIds(urls: List<String>, text: String): List<String> {
        val typed = text.trim()
        var links = urls
        if (links.isEmpty() && typed.isNotEmpty() && !isVideoId(typed) && typed.none { it.isWhitespace() }) links = listOf(typed)
        val ids = mutableListOf<String>()
        for (link in links) {
            val id = videoId(link)
            if (id.isEmpty()) return emptyList()
            if (id !in ids) ids.add(id)
        }
        return ids
    }

    fun watchUrl(id: String): String = "https://www.youtube.com/watch?v=${encode(id)}"

    fun thumbnailUrl(id: String, quality: String = "hqdefault"): String = "https://i.ytimg.com/vi/$id/$quality.jpg"

    fun gains(crossfader: Double): Pair<Double, Double> {
        val position = (min(1.0, max(-1.0, crossfader)) + 1) / 2
        val a = cos(position * PI / 2)
        val b = sin(position * PI / 2)
        return (if (abs(a) < 1e-9) 0.0 else a) to (if (abs(b) < 1e-9) 0.0 else b)
    }

    fun gain(side: DjDeckSide, crossfader: Double): Double {
        val both = gains(crossfader)
        return if (side == DjDeckSide.A) both.first else both.second
    }

    fun clock(seconds: Double): String {
        val total = if (seconds.isFinite()) max(0, seconds.toLong()) else 0L
        val hours = total / 3600
        val minutes = total % 3600 / 60
        val secs = total % 60
        if (hours > 0) return "%d:%02d:%02d".format(hours, minutes, secs)
        return "%02d:%02d".format(minutes, secs)
    }

    fun clampRate(rate: Double): Double {
        if (!rate.isFinite()) return 1.0
        return min(1 + PITCH_RANGE, max(1 - PITCH_RANGE, rate))
    }

    fun syncedRate(ownBpm: Double, otherBpm: Double, otherRate: Double): Double {
        if (ownBpm > 0 && otherBpm > 0) return clampRate(otherBpm * otherRate / ownBpm)
        return clampRate(otherRate)
    }

    fun scratchSeconds(fromRadians: Double): Double = fromRadians / (2 * PI) * SECONDS_PER_TURN

    fun scratchSpeed(previous: Double, seconds: Double, elapsed: Double): Double {
        if (!seconds.isFinite() || !elapsed.isFinite() || elapsed <= 0) return previous
        val instant = min(8.0, max(-8.0, seconds / max(0.008, elapsed)))
        return previous * 0.4 + instant * 0.6
    }

    fun scratchLevel(speed: Double): Double {
        if (!speed.isFinite()) return 0.0
        return min(1.0, abs(speed) / 2.5) * (if (speed < 0) 1.0 else 0.5)
    }

    fun isScratch(dx: Double, dy: Double, threshold: Double = SCRATCH_THRESHOLD): Boolean {
        if (!dx.isFinite() || !dy.isFinite()) return false
        return sqrt(dx * dx + dy * dy) >= threshold
    }

    fun sampleSection(deck: DjDeckSide, start: Double, end: Double): DjSection? {
        if (!start.isFinite() || !end.isFinite() || end - start < SHORTEST_SAMPLE) return null
        return DjSection.of(deck, start, end - start)
    }

    fun trackedPosition(predicted: Double, reported: Double, continuous: Boolean): Double {
        if (!reported.isFinite()) return predicted
        if (!continuous || !predicted.isFinite()) return reported
        val error = reported - predicted
        return if (abs(error) > 0.3) reported else predicted + error * 0.35
    }

    fun amplitude(bars: List<DjWaveBar>, time: Double, duration: Double): DjWaveBar {
        if (bars.isEmpty() || duration <= 0 || !time.isFinite() || time < 0 || time > duration) return DjWaveBar(0.0, 0.0)
        val place = time / duration * bars.size - 0.5
        val lower = min(bars.size - 1, max(0, floor(place).toInt()))
        val upper = min(bars.size - 1, lower + 1)
        val blend = min(1.0, max(0.0, place - lower))
        return DjWaveBar(
            bars[lower].level * (1 - blend) + bars[upper].level * blend,
            (if (blend < 0.5) bars[lower] else bars[upper]).hue,
        )
    }

    fun remember(history: List<DjHistoryItem>, id: String, title: String, author: String, limit: Int = HISTORY_LIMIT): List<DjHistoryItem> {
        if (!isVideoId(id) || limit <= 0) return history
        val known = history.firstOrNull { it.id == id }
        val item = DjHistoryItem.of(id, title.ifEmpty { known?.title ?: "" }, author.ifEmpty { known?.author ?: "" })
        return (listOf(item) + history.filter { it.id != id }).take(limit)
    }

    fun played(library: List<DjPlayedSong>, id: String, title: String, author: String, at: Double, limit: Int = LIBRARY_LIMIT): List<DjPlayedSong> {
        if (!isVideoId(id) || limit <= 0) return library
        val known = library.firstOrNull { it.id == id }
        val song = DjPlayedSong.of(
            id, title.ifEmpty { known?.title ?: "" }, author.ifEmpty { known?.author ?: "" },
            (known?.plays ?: 0) + 1, if (at.isFinite()) at else known?.last ?: 0.0,
        )
        return (listOf(song) + library.filter { it.id != id }).take(limit)
    }

    fun retitled(library: List<DjPlayedSong>, id: String, title: String, author: String): List<DjPlayedSong> = library.map { song ->
        if (song.id != id) song else song.copy(
            title = if (title.isNotEmpty()) title.take(300) else song.title,
            author = if (author.isNotEmpty()) author.take(200) else song.author,
        )
    }

    fun playsLabel(plays: Int): String = if (plays <= 0) "" else if (plays == 1) "1 play" else "$plays plays"

    fun mostPlayed(library: List<DjPlayedSong>, limit: Int = 25): List<DjPlayedSong> =
        library.withIndex().filter { it.value.plays > 1 }
            .sortedWith(compareByDescending<IndexedValue<DjPlayedSong>> { it.value.plays }.thenBy { it.index })
            .map { it.value }.take(limit)

    fun turnDegrees(seconds: Double): Double = (seconds / SECONDS_PER_TURN * 360) % 360

    fun searchInstances(text: String, limit: Int = 3): List<String> {
        val rows = runCatching { Json.parse(text) }.getOrNull() as? List<*> ?: return emptyList()
        return rows.mapNotNull { row ->
            val pair = row as? List<*> ?: return@mapNotNull null
            if (pair.size < 2) return@mapNotNull null
            val details = pair[1].asJsonMap()
            val uri = details["uri"] as? String ?: return@mapNotNull null
            if (details["api"] != true || !uri.startsWith("https://")) return@mapNotNull null
            uri.removeSuffix("/")
        }.take(limit)
    }

    fun encode(text: String): String {
        val out = StringBuilder()
        for (byte in text.toByteArray(Charsets.UTF_8)) {
            val c = byte.toInt() and 0xff
            val ch = c.toChar()
            if (ch in 'A'..'Z' || ch in 'a'..'z' || ch in '0'..'9' || ch == '-' || ch == '.' || ch == '_' || ch == '~') out.append(ch)
            else out.append('%').append("%02X".format(c))
        }
        return out.toString()
    }

    fun searchUrl(instance: String, query: String): String = "$instance/api/v1/search?q=${encode(query)}&type=video"

    fun similarUrl(instance: String, id: String): String? =
        if (isVideoId(id)) "$instance/api/v1/videos/$id?fields=recommendedVideos" else null

    fun similarResults(text: String): List<DjSearchResult> {
        val value = runCatching { Json.parse(text) }.getOrNull() as? Map<*, *> ?: return emptyList()
        return results(jsonMaps(value["recommendedVideos"]))
    }

    fun searchResults(text: String): List<DjSearchResult> {
        val value = runCatching { Json.parse(text) }.getOrNull() as? List<*> ?: return emptyList()
        return results(jsonMaps(value))
    }

    fun pageSearchUrl(query: String): String = "https://www.youtube.com/results?search_query=${encode(query)}&hl=en"

    fun pageResults(html: String, limit: Int = 20): List<DjSearchResult> {
        val start = Regex("(?:var\\s+ytInitialData|window\\[\"ytInitialData\"\\])\\s*=\\s*").find(html) ?: return emptyList()
        val from = start.range.last + 1
        val end = jsonEnd(html, from)
        if (end <= from) return emptyList()
        val root = runCatching { Json.parse(html.substring(from, end)) }.getOrNull() ?: return emptyList()
        val found = mutableListOf<DjSearchResult>()
        val seen = mutableSetOf<String>()
        fun text(value: Any?): String {
            val map = value as? Map<*, *> ?: return ""
            (map["simpleText"] as? String)?.let { return it }
            return (map["runs"] as? List<*>).orEmpty().joinToString("") { ((it as? Map<*, *>)?.get("text") as? String).orEmpty() }
        }
        fun walk(value: Any?) {
            if (found.size >= limit) return
            when (value) {
                is Map<*, *> -> {
                    for (key in listOf("videoRenderer", "compactVideoRenderer")) {
                        val row = value[key] as? Map<*, *> ?: continue
                        val id = row["videoId"] as? String ?: continue
                        if (!isVideoId(id) || !seen.add(id)) continue
                        val owner = text(row["ownerText"]).ifEmpty { text(row["longBylineText"]) }.ifEmpty { text(row["shortBylineText"]) }
                        found.add(DjSearchResult(id, text(row["title"]).ifEmpty { "Untitled video" }, owner, length(text(row["lengthText"]))))
                    }
                    for (item in value.values) walk(item)
                }
                is List<*> -> for (item in value) walk(item)
            }
        }
        walk(root)
        return found
    }

    fun length(text: String): Double {
        val parts = text.trim().split(':').map { it.toIntOrNull() ?: return 0.0 }
        return parts.fold(0.0) { total, part -> total * 60 + part }
    }

    private fun jsonEnd(text: String, from: Int): Int {
        var depth = 0
        var quoted = false
        var escaped = false
        for (index in from until text.length) {
            val c = text[index]
            if (quoted) {
                if (escaped) escaped = false else if (c == '\\') escaped = true else if (c == '"') quoted = false
                continue
            }
            when (c) {
                '"' -> quoted = true
                '{', '[' -> depth += 1
                '}', ']' -> {
                    depth -= 1
                    if (depth == 0) return index + 1
                }
            }
        }
        return -1
    }

    fun results(rows: List<JsonMap>): List<DjSearchResult> {
        val seen = mutableSetOf<String>()
        return rows.mapNotNull { row ->
            val link = (row["url"] as? String)?.let { if (it.startsWith("/")) "https://www.youtube.com$it" else it }
            val id = videoId((row["videoId"] as? String) ?: link ?: "")
            if (id.isEmpty() || !seen.add(id)) return@mapNotNull null
            val title = (row["title"] as? String)?.takeIf { it.isNotEmpty() } ?: "Untitled video"
            val seconds = (row["lengthSeconds"] as? Number)?.toDouble() ?: 0.0
            DjSearchResult(id, title, row["author"] as? String ?: "", seconds)
        }
    }
}

class DjTapTempo {
    private val tapList = mutableListOf<Double>()
    val taps: List<Double> get() = tapList.toList()

    fun tap(time: Double): Double? {
        val last = tapList.lastOrNull()
        if (last != null && (time - last > 2 || time <= last)) tapList.clear()
        tapList.add(time)
        while (tapList.size > 8) tapList.removeAt(0)
        if (tapList.size < 3) return null
        val bpm = 60 / ((tapList.last() - tapList.first()) / (tapList.size - 1))
        if (!bpm.isFinite() || bpm < 40 || bpm > 250) return null
        return Math.round(bpm * 10) / 10.0
    }
}

class DjSpin {
    companion object {
        const val SPIN_UP = 0.09
        const val SPIN_DOWN = 0.18
        const val FOLLOW = 0.2
        const val JUMP = 0.6
        const val WAITING = 0.35
        const val GAP = 0.1
        const val HITCH = 0.5
    }

    var position = 0.0
        private set
    var speed = 0.0
        private set
    var turn = 0.0
        private set
    var motor = 0.0
        private set
    private var stamp: Double? = null

    fun hold(target: Double, now: Double) {
        if (!target.isFinite()) return
        if (abs(target - position) <= JUMP) turn += target - position
        position = target
        speed = 0.0
        motor = 0.0
        if (now.isFinite()) stamp = now
    }

    fun step(target: Double, speed: Double, motor: Double? = null, now: Double): Double {
        if (!target.isFinite()) return position
        val goal = if (speed.isFinite()) speed else 0.0
        val drive = motor?.let { if (it.isFinite()) it else 0.0 } ?: goal
        val last = stamp
        if (!now.isFinite() || last == null || abs(target - position) > JUMP) {
            position = target
            this.speed = goal
            this.motor = drive
            if (now.isFinite()) stamp = now
            return position
        }
        if (now < last) return position - this.speed * min(last - now, GAP)
        stamp = now
        val elapsed = now - last
        if (elapsed > GAP) {
            if (elapsed <= HITCH) turn += this.motor * elapsed
            position = target
            return position
        }
        if (elapsed <= 0) return position
        this.speed += (goal - this.speed) * (1 - exp(-elapsed / (if (goal > this.speed) SPIN_UP else SPIN_DOWN)))
        this.motor += (drive - this.motor) * (1 - exp(-elapsed / (if (drive > this.motor) SPIN_UP else SPIN_DOWN)))
        turn += this.motor * elapsed
        position += this.speed * elapsed
        position += (target - position) * (1 - exp(-elapsed / FOLLOW))
        return position
    }
}

class DjRandom(seed: Long) {
    private var state = seed

    fun next(): Long {
        state += -0x61c8864680b583ebL
        var value = state
        value = (value xor (value ushr 30)) * -0x40a7b892e31b1a47L
        value = (value xor (value ushr 27)) * -0x6b2fb644ecceee15L
        return value xor (value ushr 31)
    }

    fun unit(): Double = (next() ushr 11).toDouble() / (1L shl 53).toDouble()

    fun below(count: Int): Int = java.lang.Long.remainderUnsigned(next(), count.toLong()).toInt()
}

internal object DjSavedJson {
    fun clamp(value: Any?, fallback: Double, low: Double, high: Double): Double {
        val number = (value as? Number)?.toDouble() ?: return fallback
        if (!number.isFinite()) return fallback
        return min(high, max(low, number))
    }

    fun text(value: Any?): String = value as? String ?: ""
}

data class DjSavedDeck(
    val videoId: String = "",
    val title: String = "",
    val author: String = "",
    val cue: Double = 0.0,
    val level: Double = 1.0,
    val rate: Double = 1.0,
    val bpm: Double = 0.0,
    val loop: Boolean = false,
    val position: Double = 0.0,
    val playing: Boolean = false,
) {
    fun toJson(): Map<String, Any?> = linkedMapOf(
        "videoId" to videoId, "title" to title, "author" to author, "cue" to cue, "level" to level, "rate" to rate,
        "bpm" to bpm, "loop" to loop, "position" to position, "playing" to playing,
    )

    companion object {
        fun parse(value: Any?): DjSavedDeck {
            val row = value as? Map<*, *> ?: return DjSavedDeck()
            val id = DjSavedJson.text(row["videoId"])
            val videoId = if (DjStudio.isVideoId(id)) id else ""
            val cue = DjSavedJson.clamp(row["cue"], 0.0, 0.0, 864_000.0)
            return DjSavedDeck(
                videoId = videoId,
                title = DjSavedJson.text(row["title"]).take(300),
                author = DjSavedJson.text(row["author"]).take(200),
                cue = cue,
                level = DjSavedJson.clamp(row["level"], 1.0, 0.0, 1.0),
                rate = DjStudio.clampRate(DjSavedJson.clamp(row["rate"], 1.0, DjStudio.RATE_LOW, DjStudio.RATE_HIGH)),
                bpm = DjSavedJson.clamp(row["bpm"], 0.0, 0.0, 250.0),
                loop = row["loop"] == true,
                position = DjSavedJson.clamp(row["position"], cue, 0.0, 864_000.0),
                playing = videoId.isNotEmpty() && row["playing"] == true,
            )
        }
    }
}

data class DjHistoryItem(val id: String, val title: String, val author: String) {
    fun toJson(): Map<String, Any?> = linkedMapOf("id" to id, "title" to title, "author" to author)

    companion object {
        fun of(id: String, title: String, author: String) = DjHistoryItem(id, title.take(300), author.take(200))

        fun parse(value: Any?): DjHistoryItem? {
            val row = value as? Map<*, *> ?: return null
            val id = row["id"] as? String ?: return null
            if (!DjStudio.isVideoId(id)) return null
            return of(id, DjSavedJson.text(row["title"]), DjSavedJson.text(row["author"]))
        }
    }
}

data class DjPlayedSong(val id: String, val title: String, val author: String, val plays: Int = 1, val last: Double = 0.0) {
    fun toJson(): Map<String, Any?> = linkedMapOf("id" to id, "title" to title, "author" to author, "plays" to plays, "last" to last)

    companion object {
        fun of(id: String, title: String, author: String, plays: Int = 1, last: Double = 0.0) = DjPlayedSong(
            id, title.take(300), author.take(200), max(0, plays), if (last.isFinite()) max(0.0, last) else 0.0,
        )

        fun parse(value: Any?): DjPlayedSong? {
            val row = value as? Map<*, *> ?: return null
            val id = row["id"] as? String ?: return null
            if (!DjStudio.isVideoId(id)) return null
            val plays = (row["plays"] as? Number)?.toLong() ?: 1L
            return of(
                id, DjSavedJson.text(row["title"]), DjSavedJson.text(row["author"]), min(1_000_000L, plays).toInt(),
                (row["last"] as? Number)?.toDouble() ?: 0.0,
            )
        }
    }
}

data class DjSection(val deck: DjDeckSide, val start: Double, val duration: Double) {
    fun toJson(): Map<String, Any?> = linkedMapOf("deck" to deck.key, "start" to start, "duration" to duration)

    companion object {
        fun of(deck: DjDeckSide, start: Double, duration: Double) = DjSection(deck, max(0.0, start), min(30.0, max(1.0, duration)))

        fun parse(value: Any?): DjSection? {
            val row = value as? Map<*, *> ?: return null
            val deck = DjDeckSide.of(row["deck"] as? String ?: "") ?: return null
            if (row["start"] !is Number || row["duration"] !is Number) return null
            return of(
                deck,
                DjSavedJson.clamp(row["start"], 0.0, 0.0, 864_000.0),
                DjSavedJson.clamp(row["duration"], DjStudio.SECTION_SECONDS, 1.0, 30.0),
            )
        }
    }
}

data class DjSaved(
    val a: DjSavedDeck = DjSavedDeck(),
    val b: DjSavedDeck = DjSavedDeck(),
    val crossfader: Double = 0.0,
    val samplerLevel: Double = 0.8,
    val sections: List<DjSection?> = List(DjStudio.SECTION_COUNT) { null },
    val history: List<DjHistoryItem> = emptyList(),
    val queue: List<DjQueueItem> = emptyList(),
    val automix: Boolean = false,
    val library: List<DjPlayedSong> = emptyList(),
) {
    val playing: Boolean get() = a.playing || b.playing

    operator fun get(side: DjDeckSide): DjSavedDeck = if (side == DjDeckSide.A) a else b

    fun with(side: DjDeckSide, deck: DjSavedDeck): DjSaved = if (side == DjDeckSide.A) copy(a = deck) else copy(b = deck)

    fun encoded(): String = Json.stringify(
        linkedMapOf(
            "a" to a.toJson(), "b" to b.toJson(), "crossfader" to crossfader, "samplerLevel" to samplerLevel,
            "sections" to sections.map { it?.toJson() }, "history" to history.map { it.toJson() },
            "queue" to queue.map { it.toJson() }, "automix" to automix, "library" to library.map { it.toJson() },
        ),
    )

    companion object {
        fun decode(text: String?): DjSaved {
            if (text.isNullOrEmpty()) return DjSaved()
            val root = runCatching { Json.parse(text) }.getOrNull() as? Map<*, *> ?: return DjSaved()
            val stored = root["sections"] as? List<*> ?: emptyList<Any?>()
            val parsed = stored.map { if (it == null) null else DjSection.parse(it) }
            val valid = stored.indices.all { stored[it] == null || parsed[it] != null }
            val sections = (0 until DjStudio.SECTION_COUNT).map { if (valid && it < parsed.size) parsed[it] else null }
            val seen = mutableSetOf<String>()
            val history = (root["history"] as? List<*> ?: emptyList<Any?>()).mapNotNull(DjHistoryItem::parse)
                .filter { seen.add(it.id) }.take(DjStudio.HISTORY_LIMIT)
            val ids = mutableSetOf<String>()
            val queue = (root["queue"] as? List<*> ?: emptyList<Any?>()).mapNotNull(DjQueueItem::parse)
                .filter { ids.add(it.id) }.take(DjStudio.QUEUE_LIMIT)
            val library = if (root.containsKey("library")) {
                val known = mutableSetOf<String>()
                (root["library"] as? List<*> ?: emptyList<Any?>()).mapNotNull(DjPlayedSong::parse)
                    .filter { known.add(it.id) }.take(DjStudio.LIBRARY_LIMIT)
            } else {
                history.map { DjPlayedSong.of(it.id, it.title, it.author) }
            }
            return DjSaved(
                a = DjSavedDeck.parse(root["a"]),
                b = DjSavedDeck.parse(root["b"]),
                crossfader = DjSavedJson.clamp(root["crossfader"], 0.0, -1.0, 1.0),
                samplerLevel = DjSavedJson.clamp(root["samplerLevel"], 0.8, 0.0, 1.0),
                sections = sections,
                history = history,
                queue = queue,
                automix = root["automix"] == true,
                library = library,
            )
        }
    }
}

object DjDrumSynth {
    private enum class Wave { SINE, TRIANGLE, SAW }

    fun samples(pad: DjDrumPad, sampleRate: Double = 44_100.0): FloatArray {
        val out = FloatArray((sampleRate * length(pad)).toInt())
        val random = DjRandom(0x5eed0000L + pad.ordinal)
        when (pad) {
            DjDrumPad.KICK -> tone(out, sampleRate, Wave.SINE, 155.0, 43.0, 0.32, 0.8)
            DjDrumPad.TOM -> tone(out, sampleRate, Wave.SINE, 190.0, 80.0, 0.36, 0.65)
            DjDrumPad.PERC -> tone(out, sampleRate, Wave.TRIANGLE, 740.0, 160.0, 0.16, 0.36)
            DjDrumPad.RISER -> tone(out, sampleRate, Wave.SAW, 160.0, 1300.0, 0.75, 0.25)
            DjDrumPad.STAB -> for (frequency in listOf(196.0, 247.0, 294.0)) tone(out, sampleRate, Wave.SAW, frequency, frequency * 0.98, 0.22, 0.13)
            DjDrumPad.SNARE -> noise(out, sampleRate, random, 0.0, 0.18, 1800.0, 0.5)
            DjDrumPad.HAT -> noise(out, sampleRate, random, 0.0, 0.07, 6000.0, 0.22)
            DjDrumPad.CLAP -> {
                noise(out, sampleRate, random, 0.0, 0.3, 1000.0, 0.5)
                for (delay in listOf(0.045, 0.09)) noise(out, sampleRate, random, delay, 0.12, 1000.0, 0.25)
            }
        }
        for (index in out.indices) out[index] = min(1f, max(-1f, out[index]))
        return out
    }

    fun length(pad: DjDrumPad): Double = when (pad) {
        DjDrumPad.KICK -> 0.35
        DjDrumPad.TOM -> 0.39
        DjDrumPad.PERC -> 0.19
        DjDrumPad.RISER -> 0.78
        DjDrumPad.STAB -> 0.25
        DjDrumPad.SNARE -> 0.21
        DjDrumPad.HAT -> 0.1
        DjDrumPad.CLAP -> 0.33
    }

    private fun envelope(time: Double, duration: Double, peak: Double): Double {
        val attack = 0.004
        val floor = 0.0001
        if (time < attack) return floor * (peak / floor).pow(time / attack)
        if (time >= duration) return 0.0
        return peak * (floor / peak).pow((time - attack) / (duration - attack))
    }

    private fun tone(out: FloatArray, rate: Double, wave: Wave, start: Double, end: Double, duration: Double, peak: Double) {
        var phase = 0.0
        for (index in 0 until min(out.size, (duration * rate).toInt())) {
            val time = index / rate
            phase += start * (end / start).pow(time / duration) / rate
            val cycle = phase - floor(phase)
            val value = when (wave) {
                Wave.SINE -> sin(2 * PI * cycle)
                Wave.TRIANGLE -> 1 - 4 * abs(cycle - 0.5)
                Wave.SAW -> 2 * cycle - 1
            }
            out[index] += (value * envelope(time, duration, peak)).toFloat()
        }
    }

    private fun noise(out: FloatArray, rate: Double, random: DjRandom, delay: Double, duration: Double, cutoff: Double, peak: Double) {
        val resistance = 1 / (2 * PI * cutoff)
        val alpha = resistance / (resistance + 1 / rate)
        var previousIn = 0.0
        var previousOut = 0.0
        val first = (delay * rate).toInt()
        for (index in 0 until (duration * rate).toInt()) {
            if (first + index >= out.size) continue
            val input = random.unit() * 2 - 1
            previousOut = alpha * (previousOut + input - previousIn)
            previousIn = input
            out[first + index] += (previousOut * envelope(index / rate, duration, peak)).toFloat()
        }
    }
}

internal fun String.splitWhere(predicate: (Char) -> Boolean): List<String> {
    val out = mutableListOf<String>()
    val current = StringBuilder()
    for (c in this) {
        if (predicate(c)) {
            if (current.isNotEmpty()) out.add(current.toString())
            current.clear()
        } else {
            current.append(c)
        }
    }
    if (current.isNotEmpty()) out.add(current.toString())
    return out
}
