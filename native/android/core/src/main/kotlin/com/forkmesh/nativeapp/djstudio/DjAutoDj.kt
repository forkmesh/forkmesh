package com.forkmesh.nativeapp.djstudio

import com.forkmesh.nativeapp.json.Json
import kotlin.math.log10
import kotlin.math.max
import kotlin.math.min

data class DjAutoDjCandidate(
    val id: String,
    val title: String,
    val author: String = "",
    val seconds: Double = 0.0,
    val views: Double = 0.0,
    val likes: Double? = null,
    val rank: Int = 0,
) {
    val result: DjSearchResult get() = DjSearchResult(id, title, author, seconds)
}

data class DjLikes(val likes: Double, val views: Double)

object DjAutoDj {
    const val SHORTEST = 60.0
    const val LONGEST = 12 * 60.0
    const val CHECKED = 6
    const val LIKE_SHARE = 0.01
    const val SESSION_LIMIT = 200

    private val noise = setOf(
        "official", "video", "audio", "music", "lyrics", "lyric", "visualizer", "visualiser", "hd", "hq", "4k", "mv", "m v",
        "remastered", "remaster", "explicit", "clean", "version", "full", "feat", "ft", "featuring", "topic",
    )

    private val notSongs = listOf(
        "got talent", "agt", "bgt", "x factor", "xfactor", "the voice", "american idol", "pop idol", "idols", "golden buzzer",
        "audition", "auditions", "judges", "simon cowell", "reaction", "reactions", "reacts", "reacting", "first time hearing",
        "first time listening", "vocal coach", "cover by", "covered by", "cover of", "acoustic cover", "piano cover", "guitar cover",
        "drum cover", "bass cover", "karaoke", "backing track", "sing along", "tutorial", "lesson", "how to play", "how to sing",
        "remix", "mashup", "nightcore", "sped up", "slowed", "reverb", "8d", "bass boosted", "parody", "medley", "interview",
        "trailer", "episode", "compilation", "highlights", "rehearsal", "behind the scenes", "documentary", "podcast",
        "live at", "live from", "live performance", "full performance", "concert", "tiny desk", "sings", "performs", "tribute",
    )

    private val notOriginal = listOf("live", "cover", "covers", "acoustic", "unplugged", "instrumental", "karaoke", "remix", "session", "sessions", "demo")

    private val brackets = listOf("(" to ")", "[" to "]", "{" to "}")

    private val viewCount = Regex("[0-9]+(\\.[0-9]+)?\\s*[kmb]?")

    fun candidates(text: String): List<DjAutoDjCandidate> {
        val root = runCatching { Json.parse(text) }.getOrNull() ?: return emptyList()
        val rows = ((root as? Map<*, *>)?.get("recommendedVideos") as? List<*> ?: root as? List<*> ?: emptyList<Any?>()).mapNotNull { it as? Map<*, *> }
        val seen = mutableSetOf<String>()
        val out = mutableListOf<DjAutoDjCandidate>()
        for (row in rows) {
            val type = row["type"] as? String
            if (type != null && type != "video") continue
            val link = (row["url"] as? String)?.let { if (it.startsWith("/")) "https://www.youtube.com$it" else it }
            val id = DjStudio.videoId(row["videoId"] as? String ?: link ?: "")
            if (id.isEmpty() || !seen.add(id)) continue
            val title = (row["title"] as? String)?.takeIf { it.isNotEmpty() } ?: "Untitled video"
            val views = (row["viewCount"] as? Number)?.toDouble() ?: count(row["viewCountText"] as? String ?: "")
            out += DjAutoDjCandidate(
                id, title, row["author"] as? String ?: "", (row["lengthSeconds"] as? Number)?.toDouble() ?: 0.0, views, rank = seen.size - 1,
            )
        }
        return out
    }

    fun count(text: String): Double {
        val lowered = text.lowercase().replace(",", "")
        val part = viewCount.find(lowered)?.value?.replace(" ", "") ?: return 0.0
        val scale = when {
            part.endsWith("k") -> 1e3
            part.endsWith("m") -> 1e6
            part.endsWith("b") -> 1e9
            else -> 1.0
        }
        return (part.trim('k', 'm', 'b').toDoubleOrNull() ?: 0.0) * scale
    }

    fun likesUrl(instance: String, id: String): String? =
        if (DjStudio.isVideoId(id)) "${instance.trimEnd('/')}/api/v1/videos/$id?fields=likeCount,viewCount" else null

    fun likes(text: String): DjLikes? {
        val root = runCatching { Json.parse(text) }.getOrNull() as? Map<*, *> ?: return null
        val likes = (root["likeCount"] as? Number)?.toDouble() ?: return null
        if (likes < 0) return null
        return DjLikes(likes, max(0.0, (root["viewCount"] as? Number)?.toDouble() ?: 0.0))
    }

    fun songKey(title: String): String {
        var name = title
        for ((open, close) in brackets) {
            while (true) {
                val start = name.indexOf(open)
                if (start < 0) break
                val end = name.indexOf(close, start + open.length)
                if (end < 0) break
                name = name.removeRange(start, end + close.length)
            }
        }
        for (dash in listOf(" - ", " – ", " — ", " | ")) {
            val split = name.indexOf(dash)
            if (split >= 0) {
                val tail = name.substring(split + dash.length)
                if (key(tail).isNotEmpty()) name = tail
                break
            }
        }
        return key(name)
    }

    internal fun key(text: String): String =
        DjSmart.text(text).split(" ").filter { it.isNotEmpty() && it !in noise }.joinToString(" ")

    internal fun qualifiers(title: String): List<String> {
        val parts = mutableListOf<String>()
        for ((open, close) in brackets) {
            var rest = title
            while (true) {
                val start = rest.indexOf(open)
                if (start < 0) break
                val end = rest.indexOf(close, start + open.length)
                if (end < 0) break
                parts += rest.substring(start + open.length, end)
                rest = rest.substring(end + close.length)
            }
        }
        val piped = title.split(" | ")
        parts += piped.drop(1)
        var dashed = piped[0]
        for (dash in listOf(" – ", " — ")) dashed = dashed.replace(dash, " - ")
        parts += dashed.split(" - ").drop(2)
        return parts.map(DjSmart::text)
    }

    fun isSong(candidate: DjAutoDjCandidate): Boolean {
        val text = DjSmart.text(candidate.title + " " + candidate.author)
        if (notSongs.any { text.contains(" $it ") }) return false
        val last = DjSmart.text(candidate.title).split(" ").lastOrNull { it.isNotEmpty() }
        if (last != null && last in listOf("cover", "covers", "karaoke", "instrumental")) return false
        return qualifiers(candidate.title).none { part -> notOriginal.any { part.contains(" $it ") } }
    }

    fun isOriginal(candidate: DjAutoDjCandidate): Boolean {
        val author = DjSmart.text(candidate.author)
        if (author.endsWith(" topic ") || author.contains("vevo")) return true
        val title = DjSmart.text(candidate.title)
        if (listOf(" official ", " remastered ", " remaster ", " original ").any { title.contains(it) }) return true
        var name = candidate.title
        for (dash in listOf(" – ", " — ", " | ")) name = name.replace(dash, " - ")
        val parts = name.split(" - ")
        val artist = key(candidate.author)
        return parts.size > 1 && artist.isNotEmpty() && key(parts[0]) == artist
    }

    fun eligible(found: List<DjAutoDjCandidate>, seed: DjTrack, avoid: Set<String>, heard: List<String>): List<DjAutoDjCandidate> {
        val keys = (listOf(seed.title) + heard).map(::songKey).filter { it.isNotEmpty() }.toMutableSet()
        val songs = found.filter { candidate ->
            candidate.id != seed.id && candidate.id !in avoid && candidate.seconds >= SHORTEST && candidate.seconds <= LONGEST && isSong(candidate)
        }
        return (songs.filter(::isOriginal) + songs.filter { !isOriginal(it) }).filter { candidate ->
            val key = songKey(candidate.title)
            key.isEmpty() || keys.add(key)
        }
    }

    fun score(candidate: DjAutoDjCandidate, among: Int): Double {
        val likes = candidate.likes ?: (candidate.views * LIKE_SHARE)
        val related = if (among > 1) 1 - candidate.rank.toDouble() / (among - 1) else 1.0
        val ratio = if (candidate.likes != null && candidate.views > 0) min(0.1, likes / candidate.views) * 10 else 0.0
        return log10(1 + max(0.0, likes)) + ratio + related * 2
    }

    fun ranked(found: List<DjAutoDjCandidate>): List<DjAutoDjCandidate> {
        val span = (found.maxOfOrNull { it.rank } ?: 0) + 1
        return found.withIndex().sortedWith { left, right ->
            val l = isOriginal(left.value)
            val r = isOriginal(right.value)
            if (l != r) return@sortedWith if (l) -1 else 1
            val a = score(left.value, span)
            val b = score(right.value, span)
            if (a != b) b.compareTo(a) else left.index.compareTo(right.index)
        }.map { it.value }
    }

    fun next(ranked: List<DjAutoDjCandidate>, loaded: Set<String>, skip: Set<String>, played: List<String>): DjAutoDjCandidate? =
        ranked.firstOrNull { it.id !in loaded && it.id !in skip && it.id !in played }

    fun remember(id: String, played: List<String>): List<String> = (listOf(id) + played.filter { it != id }).take(SESSION_LIMIT)
}

data class DjRepeatSpan(val start: Double, val end: Double)

sealed interface DjRepeatStep {
    data object Wait : DjRepeatStep
    data object Wrap : DjRepeatStep
    data class After(val seconds: Double) : DjRepeatStep
}

object DjRepeat {
    const val QUIET = 0.07
    const val TAIL = 0.25
    const val SHORTEST = 2.0
    const val AHEAD = 0.6

    fun span(bars: List<DjWaveBar>, duration: Double): DjRepeatSpan? {
        if (!duration.isFinite() || duration <= TAIL + SHORTEST) return null
        val whole = DjRepeatSpan(0.0, duration - TAIL)
        val first = bars.indexOfFirst { it.level > QUIET }
        val last = bars.indexOfLast { it.level > QUIET }
        if (first < 0 || last < 0) return whole
        val step = duration / bars.size
        val start = first * step
        val end = min(whole.end, (last + 1) * step)
        if (end - start < SHORTEST) return whole
        return DjRepeatSpan(start, end)
    }

    fun step(position: Double, span: DjRepeatSpan, rate: Double): DjRepeatStep {
        if (!position.isFinite() || !rate.isFinite() || rate <= 0.01) return DjRepeatStep.Wait
        val left = (span.end - position) / rate
        if (left <= 0) return DjRepeatStep.Wrap
        return if (left < AHEAD) DjRepeatStep.After(left) else DjRepeatStep.Wait
    }
}
