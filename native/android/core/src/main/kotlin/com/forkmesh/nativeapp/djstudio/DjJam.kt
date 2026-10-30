package com.forkmesh.nativeapp.djstudio

import com.forkmesh.nativeapp.json.JsonMap
import com.forkmesh.nativeapp.json.asJsonMap
import java.time.Instant
import java.time.ZoneId
import java.time.format.DateTimeFormatter
import java.time.format.FormatStyle
import java.util.Locale
import java.util.UUID
import kotlin.math.abs
import kotlin.math.floor
import kotlin.math.max
import kotlin.math.min
import kotlin.math.roundToLong
import kotlin.math.sqrt

data class DjQueueItem(
    val id: String,
    val videoId: String,
    val title: String,
    val author: String,
    val addedBy: String = "",
    val addedAt: Long = 0,
) {
    val thumbnail: String get() = DjStudio.thumbnailUrl(videoId, "mqdefault")

    fun toJson(): Map<String, Any?> =
        linkedMapOf("id" to id, "videoId" to videoId, "title" to title, "author" to author, "addedBy" to addedBy, "addedAt" to addedAt)

    fun addedStamp(nowMs: Long, zone: ZoneId = ZoneId.systemDefault(), locale: Locale = Locale.getDefault()): String? {
        if (addedAt <= 0) return null
        val added = Instant.ofEpochMilli(addedAt).atZone(zone)
        val time = DateTimeFormatter.ofLocalizedTime(FormatStyle.SHORT).withLocale(locale).format(added)
        if (added.toLocalDate() == Instant.ofEpochMilli(nowMs).atZone(zone).toLocalDate()) return time
        return DateTimeFormatter.ofPattern("MMM d", locale).format(added) + ", " + time
    }

    fun addedLine(zone: ZoneId = ZoneId.systemDefault(), locale: Locale = Locale.getDefault()): String? {
        val whenText = if (addedAt > 0) {
            val added = Instant.ofEpochMilli(addedAt).atZone(zone)
            DateTimeFormatter.ofLocalizedDate(FormatStyle.MEDIUM).withLocale(locale).format(added) + " at " +
                DateTimeFormatter.ofLocalizedTime(FormatStyle.SHORT).withLocale(locale).format(added)
        } else {
            null
        }
        return when {
            addedBy.isNotEmpty() && whenText != null -> "Added by $addedBy on $whenText"
            addedBy.isNotEmpty() -> "Added by $addedBy"
            whenText != null -> "Added on $whenText"
            else -> null
        }
    }

    companion object {
        fun of(id: String, videoId: String, title: String, author: String, addedBy: String = "", addedAt: Long = 0) =
            DjQueueItem(id.take(64), videoId, title.take(300), author.take(200), addedBy.take(64), max(0L, addedAt))

        private fun stamp(value: Any?): Long = (value as? Number)?.toLong() ?: 0L

        fun parse(value: Any?): DjQueueItem? {
            val row = value as? Map<*, *> ?: return null
            val videoId = row["videoId"] as? String ?: return null
            if (!DjStudio.isVideoId(videoId)) return null
            return of(
                row["id"] as? String ?: UUID.randomUUID().toString(), videoId, row["title"] as? String ?: "",
                row["author"] as? String ?: "", row["addedBy"] as? String ?: "", stamp(row["addedAt"]),
            )
        }

        fun fromServer(row: JsonMap): DjQueueItem? {
            val videoId = row["videoId"] as? String ?: ""
            val id = row["id"] as? String ?: ""
            if (!DjStudio.isVideoId(videoId) || id.isEmpty()) return null
            return of(id, videoId, row["title"] as? String ?: "", row["author"] as? String ?: "", row["addedBy"] as? String ?: "", stamp(row["addedAt"]))
        }
    }
}

data class DjJamMember(val name: String, val status: String) {
    val joined: Boolean get() = status == "owner" || status == "joined"
}

data class DjJamDeck(
    val videoId: String = "",
    val title: String = "",
    val author: String = "",
    val playing: Boolean = false,
    val position: Double = 0.0,
    val rate: Double = 1.0,
    val at: Long = 0,
    val by: String = "",
) {
    fun expectedPosition(serverNow: Long): Double {
        if (!playing || at <= 0 || serverNow <= at) return position
        return position + (serverNow - at) / 1000.0 * rate
    }

    val body: Map<String, Any?>
        get() = linkedMapOf(
            "videoId" to videoId, "title" to title, "author" to author, "playing" to playing,
            "position" to (position * 1000).roundToLong() / 1000.0, "rate" to rate,
        )

    companion object {
        fun parse(row: JsonMap): DjJamDeck {
            val id = row["videoId"] as? String ?: ""
            return DjJamDeck(
                videoId = if (DjStudio.isVideoId(id)) id else "",
                title = (row["title"] as? String ?: "").take(300),
                author = (row["author"] as? String ?: "").take(200),
                playing = row["playing"] == true,
                position = max(0.0, (row["position"] as? Number)?.toDouble() ?: 0.0),
                rate = DjStudio.clampRate((row["rate"] as? Number)?.toDouble() ?: 1.0),
                at = (row["at"] as? Number)?.toLong() ?: 0,
                by = row["by"] as? String ?: "",
            )
        }
    }
}

data class DjJamSummary(val id: String, val title: String, val owner: String, val myStatus: String, val members: Int) {
    val invited: Boolean get() = myStatus == "invited"

    companion object {
        fun list(reply: JsonMap): List<DjJamSummary> = (reply["jams"] as? List<*> ?: emptyList<Any?>()).mapNotNull { value ->
            val row = value as? Map<*, *> ?: return@mapNotNull null
            val id = row["id"] as? String ?: return@mapNotNull null
            if (id.isEmpty()) return@mapNotNull null
            DjJamSummary(
                id, row["title"] as? String ?: "", row["owner"] as? String ?: "", row["myStatus"] as? String ?: "",
                (row["members"] as? Number)?.toInt() ?: 0,
            )
        }
    }
}

data class DjMotionSample(val x: Double, val y: Double) {
    companion object {
        val STILL = DjMotionSample(0.0, 0.0)
    }
}

object DjMotion {
    const val LIMIT = 2.0
    const val SHARED = 12
    const val DANCING = 0.3

    fun clamp(value: Double): Double = if (!value.isFinite()) 0.0 else min(LIMIT, max(-LIMIT, value))

    private fun hundredths(value: Double): Double = (clamp(value) * 100).roundToLong() / 100.0

    fun downsample(samples: List<DjMotionSample>, count: Int = SHARED): List<DjMotionSample> {
        if (count <= 0 || samples.isEmpty()) return emptyList()
        if (samples.size <= count) return samples.map { DjMotionSample(clamp(it.x), clamp(it.y)) }
        val size = samples.size.toDouble() / count
        return (0 until count).map { index ->
            val start = floor(index * size).toInt()
            val end = max(start + 1, min(samples.size, floor((index + 1) * size).toInt()))
            val peak = samples.subList(start, end).maxByOrNull { abs(it.x) + abs(it.y) } ?: DjMotionSample.STILL
            DjMotionSample(hundredths(peak.x), hundredths(peak.y))
        }
    }

    fun energy(samples: List<DjMotionSample>): Double {
        if (samples.isEmpty()) return 0.0
        val power = samples.sumOf { it.x * it.x + it.y * it.y } / samples.size
        val rms = sqrt(power)
        if (!rms.isFinite()) return 0.0
        return min(1.0, max(0.0, (rms - 0.04) / 0.6))
    }

    fun replay(samples: List<DjMotionSample>, progress: Double): DjMotionSample {
        if (samples.isEmpty() || !progress.isFinite()) return DjMotionSample.STILL
        if (samples.size == 1) return samples[0]
        val wrapped = progress - floor(progress)
        val place = wrapped * (samples.size - 1)
        val low = min(samples.size - 1, place.toInt())
        val high = min(samples.size - 1, low + 1)
        val mix = place - low
        return DjMotionSample(samples[low].x + (samples[high].x - samples[low].x) * mix, samples[low].y + (samples[high].y - samples[low].y) * mix)
    }

    fun body(samples: List<DjMotionSample>): List<List<Double>> = samples.map { listOf(it.x, it.y) }

    fun parse(value: Any?): List<DjMotionSample> = (value as? List<*> ?: emptyList<Any?>()).take(32).mapNotNull { item ->
        val pair = item as? List<*> ?: return@mapNotNull null
        if (pair.size != 2) return@mapNotNull null
        val x = (pair[0] as? Number)?.toDouble() ?: return@mapNotNull null
        val y = (pair[1] as? Number)?.toDouble() ?: return@mapNotNull null
        DjMotionSample(clamp(x), clamp(y))
    }
}

enum class DjPresenceState(val key: String) { STUDIO("studio"), LISTENING("listening"), IDLE("idle") }

enum class DjPersonStatus(val label: String) {
    INVITED("Invited"), AWAY("Away"), IDLE("Not listening"), LISTENING("Listening in the background"), STUDIO("In the studio");

    val live: Boolean get() = this == LISTENING || this == STUDIO
}

data class DjJamPresence(
    val name: String,
    val state: String = "studio",
    val playing: Boolean = false,
    val energy: Double = 0.0,
    val motion: List<DjMotionSample> = emptyList(),
    val ago: Long = 0,
) {
    val dancing: Boolean get() = energy >= DjMotion.DANCING

    companion object {
        fun parse(row: JsonMap): DjJamPresence? {
            val name = row["name"] as? String ?: return null
            if (name.isEmpty()) return null
            return DjJamPresence(
                name.take(64), row["state"] as? String ?: "studio", row["playing"] == true,
                min(1.0, max(0.0, (row["energy"] as? Number)?.toDouble() ?: 0.0)), DjMotion.parse(row["motion"]),
                max(0L, (row["ago"] as? Number)?.toLong() ?: 0),
            )
        }

        fun body(state: DjPresenceState, playing: Boolean, motion: List<DjMotionSample>): Map<String, Any?> = linkedMapOf(
            "state" to state.key, "playing" to playing,
            "energy" to (DjMotion.energy(motion) * 1000).roundToLong() / 1000.0, "motion" to DjMotion.body(DjMotion.downsample(motion)),
        )
    }
}

data class DjJamReaction(val id: String, val name: String, val kind: String, val ago: Long = 0) {
    companion object {
        fun parse(row: JsonMap): DjJamReaction? {
            val id = row["id"] as? String ?: return null
            val kind = row["kind"] as? String ?: return null
            if (id.isEmpty() || DjReaction.find(kind) == null) return null
            return DjJamReaction(id, row["name"] as? String ?: "", kind, max(0L, (row["ago"] as? Number)?.toLong() ?: 0))
        }
    }
}

enum class DjReactionStyle { RISE, RAIN, BURST, ZOOM, SPIN, ORBIT, WAVE, CONFETTI, FIREWORKS, FLASH }

data class DjReaction(val id: String, val emoji: String, val name: String, val style: DjReactionStyle, val hue: Double) {
    val seconds: Double
        get() = when (style) {
            DjReactionStyle.ZOOM, DjReactionStyle.FLASH -> 1.6
            DjReactionStyle.SPIN, DjReactionStyle.ORBIT, DjReactionStyle.BURST -> 2.2
            else -> 3.0
        }

    companion object {
        val ALL = listOf(
            DjReaction("hearts", "❤️", "Hearts", DjReactionStyle.RISE, 0.98),
            DjReaction("fire", "🔥", "Fire", DjReactionStyle.RISE, 0.06),
            DjReaction("clap", "👏", "Applause", DjReactionStyle.BURST, 0.12),
            DjReaction("confetti", "🎉", "Confetti", DjReactionStyle.CONFETTI, 0.9),
            DjReaction("fireworks", "🎆", "Fireworks", DjReactionStyle.FIREWORKS, 0.62),
            DjReaction("hundred", "💯", "100", DjReactionStyle.ZOOM, 0.0),
            DjReaction("laugh", "😂", "LOL", DjReactionStyle.RISE, 0.14),
            DjReaction("starstruck", "🤩", "Star-struck", DjReactionStyle.BURST, 0.15),
            DjReaction("mindblown", "🤯", "Mind Blown", DjReactionStyle.ZOOM, 0.05),
            DjReaction("rocket", "🚀", "Lift Off", DjReactionStyle.RISE, 0.58),
            DjReaction("sparkles", "✨", "Sparkles", DjReactionStyle.RAIN, 0.15),
            DjReaction("notes", "🎵", "Music", DjReactionStyle.ORBIT, 0.75),
            DjReaction("headphones", "🎧", "Headphones", DjReactionStyle.SPIN, 0.55),
            DjReaction("dancer", "💃", "Dance", DjReactionStyle.WAVE, 0.95),
            DjReaction("groove", "🕺", "Groove", DjReactionStyle.WAVE, 0.8),
            DjReaction("hands", "🙌", "Hands Up", DjReactionStyle.BURST, 0.1),
            DjReaction("thumbs", "👍", "Thumbs Up", DjReactionStyle.ZOOM, 0.12),
            DjReaction("cool", "😎", "Cool", DjReactionStyle.ZOOM, 0.6),
            DjReaction("lightning", "⚡", "Lightning", DjReactionStyle.FLASH, 0.15),
            DjReaction("disco", "🪩", "Disco", DjReactionStyle.SPIN, 0.7),
            DjReaction("rainbow", "🌈", "Rainbow", DjReactionStyle.WAVE, 0.33),
            DjReaction("balloons", "🎈", "Balloons", DjReactionStyle.RISE, 0.0),
            DjReaction("snow", "❄️", "Snow", DjReactionStyle.RAIN, 0.55),
            DjReaction("money", "💸", "Make It Rain", DjReactionStyle.RAIN, 0.33),
            DjReaction("tears", "🥹", "Feels", DjReactionStyle.RAIN, 0.58),
            DjReaction("eyes", "👀", "Eyes", DjReactionStyle.ZOOM, 0.1),
            DjReaction("skull", "💀", "Dead", DjReactionStyle.BURST, 0.0),
            DjReaction("bomb", "💣", "Drop", DjReactionStyle.FLASH, 0.03),
            DjReaction("rock", "🤘", "Rock On", DjReactionStyle.BURST, 0.85),
            DjReaction("loveeyes", "😍", "Love It", DjReactionStyle.ORBIT, 0.95),
        )

        fun find(id: String): DjReaction? = ALL.firstOrNull { it.id == id }
    }
}

data class DjJam(
    val id: String,
    val title: String = "",
    val owner: String = "",
    val me: String = "",
    val myStatus: String = "",
    val now: Long = 0,
    val members: List<DjJamMember> = emptyList(),
    val queue: List<DjQueueItem> = emptyList(),
    val decks: Map<DjDeckSide, DjJamDeck> = emptyMap(),
    val presence: List<DjJamPresence> = emptyList(),
    val reactions: List<DjJamReaction> = emptyList(),
) {
    companion object {
        const val AWAY_AFTER = 12_000L

        fun status(member: DjJamMember, seen: DjJamPresence?): DjPersonStatus {
            if (!member.joined) return DjPersonStatus.INVITED
            if (seen == null || seen.ago > AWAY_AFTER) return DjPersonStatus.AWAY
            if (seen.state == DjPresenceState.STUDIO.key) return DjPersonStatus.STUDIO
            return if (seen.playing) DjPersonStatus.LISTENING else DjPersonStatus.IDLE
        }

        fun parse(reply: JsonMap): DjJam? {
            val jam = reply["jam"] as? Map<*, *> ?: return null
            val row = jam.asJsonMap()
            val id = row["id"] as? String ?: return null
            if (id.isEmpty()) return null
            val decks = mutableMapOf<DjDeckSide, DjJamDeck>()
            val rows = row["decks"] as? Map<*, *>
            if (rows != null) for (side in DjDeckSide.entries) (rows[side.key] as? Map<*, *>)?.let { decks[side] = DjJamDeck.parse(it.asJsonMap()) }
            fun maps(key: String): List<JsonMap> = (row[key] as? List<*> ?: emptyList<Any?>()).mapNotNull { (it as? Map<*, *>)?.asJsonMap() }
            return DjJam(
                id = id,
                title = row["title"] as? String ?: "",
                owner = row["owner"] as? String ?: "",
                me = row["me"] as? String ?: "",
                myStatus = row["myStatus"] as? String ?: "",
                now = (row["now"] as? Number)?.toLong() ?: 0,
                members = maps("members").mapNotNull { member ->
                    val name = member["name"] as? String ?: return@mapNotNull null
                    if (name.isEmpty()) null else DjJamMember(name, member["status"] as? String ?: "")
                },
                queue = maps("queue").mapNotNull(DjQueueItem::fromServer),
                decks = decks,
                presence = maps("presence").mapNotNull(DjJamPresence::parse),
                reactions = maps("reactions").mapNotNull(DjJamReaction::parse),
            )
        }

        fun userNames(reply: JsonMap, excluding: Set<String> = emptySet()): List<String> {
            val names = mutableListOf<String>()
            for (value in reply["users"] as? List<*> ?: emptyList<Any?>()) {
                val name = ((value as? Map<*, *>)?.get("name") as? String ?: "").trim().lowercase()
                if (name.isNotEmpty() && name !in excluding && name !in names) names.add(name)
            }
            return names
        }

        fun needsSeek(local: Double, remote: Double, playing: Boolean): Boolean {
            if (!local.isFinite() || !remote.isFinite()) return false
            return abs(local - remote) > if (playing) 1.5 else 0.5
        }
    }

    fun presenceOf(name: String): DjJamPresence? = presence.firstOrNull { it.name == name }

    fun status(member: DjJamMember): DjPersonStatus = status(member, presenceOf(member.name))

    val shared: DjJam get() = copy(now = 0, presence = emptyList(), reactions = emptyList())

    val isOwner: Boolean get() = myStatus == "owner"
    val joined: Boolean get() = myStatus == "owner" || myStatus == "joined"
    val listeners: List<DjJamMember> get() = members.filter { it.joined }

    operator fun get(side: DjDeckSide): DjJamDeck = decks[side] ?: DjJamDeck()
}

sealed interface DjAutomixStep {
    data object Wait : DjAutomixStep
    data class Preload(val side: DjDeckSide) : DjAutomixStep
    data class Start(val side: DjDeckSide) : DjAutomixStep
    data class Mix(val from: DjDeckSide, val to: DjDeckSide, val seconds: Double) : DjAutomixStep
}

data class DjAutomixDeck(
    val loaded: Boolean,
    val playing: Boolean = false,
    val position: Double = 0.0,
    val duration: Double = 0.0,
    val rate: Double = 1.0,
    val repeats: Boolean = false,
) {
    val remaining: Double?
        get() {
            if (duration <= 0 || !position.isFinite()) return null
            return max(0.0, duration - position) / max(0.25, rate)
        }

    val fresh: Boolean get() = loaded && !playing && (duration <= 0 || position < duration - DjStudio.AUTOMIX_SECONDS - 2)
}

object DjAutomix {
    const val LEAD = 2.0
    const val AUDIBLE = 3.0

    fun step(a: DjAutomixDeck, b: DjAutomixDeck, favoured: DjDeckSide, queued: Boolean): DjAutomixStep {
        val decks = mapOf(DjDeckSide.A to a, DjDeckSide.B to b)
        val playing = DjDeckSide.entries.filter { decks[it]?.playing == true }
        val live = if (playing.size > 1) favoured else playing.firstOrNull()
        if (live == null) {
            for (side in listOf(favoured, favoured.other)) if (decks[side]?.fresh == true) return DjAutomixStep.Start(side)
            return if (queued) DjAutomixStep.Preload(favoured) else DjAutomixStep.Wait
        }
        val other = live.other
        val incoming = decks[other] ?: return DjAutomixStep.Wait
        val outgoing = decks[live] ?: return DjAutomixStep.Wait
        if (incoming.playing || outgoing.repeats) return DjAutomixStep.Wait
        if (!incoming.fresh) return if (queued) DjAutomixStep.Preload(other) else DjAutomixStep.Wait
        val remaining = outgoing.remaining ?: return DjAutomixStep.Wait
        if (remaining > DjStudio.AUTOMIX_SECONDS + LEAD) return DjAutomixStep.Wait
        return DjAutomixStep.Mix(live, other, max(2.0, min(DjStudio.AUTOMIX_SECONDS, remaining)))
    }

    fun playNow(a: DjAutomixDeck, b: DjAutomixDeck, crossfader: Double): DjDeckSide? {
        if (a.playing || b.playing) return null
        val favoured = if (crossfader <= 0) DjDeckSide.A else DjDeckSide.B
        val decks = mapOf(DjDeckSide.A to a, DjDeckSide.B to b)
        val open = decks[favoured]?.loaded == true && decks[favoured.other]?.loaded == false
        return if (abs(crossfader) < 0.5 && open) favoured.other else favoured
    }

    fun next(library: List<DjPlayedSong>, loaded: Set<String>, playing: Set<String>, seed: Long): DjPlayedSong? {
        var pool = library.filter { it.id !in loaded && it.id !in playing }
        if (pool.isEmpty()) pool = library.filter { it.id !in playing }
        if (pool.isEmpty()) return null
        val oldest = pool.withIndex()
            .sortedWith(compareBy<IndexedValue<DjPlayedSong>> { it.value.last }.thenByDescending { it.index })
            .map { it.value }
        val choices = oldest.take(max(1, (oldest.size + 1) / 2))
        val random = DjRandom(seed)
        return choices[random.below(choices.size)]
    }
}
