package com.forkmesh.nativeapp.insights

import com.forkmesh.nativeapp.control.ControlCalling
import com.forkmesh.nativeapp.control.LocalControlException
import com.forkmesh.nativeapp.hosts.DesktopHealth
import com.forkmesh.nativeapp.hosts.DesktopHealthLevel
import com.forkmesh.nativeapp.hosts.isUnsupportedControl
import com.forkmesh.nativeapp.json.bool
import com.forkmesh.nativeapp.json.int
import com.forkmesh.nativeapp.json.long
import com.forkmesh.nativeapp.json.maps
import com.forkmesh.nativeapp.json.obj
import com.forkmesh.nativeapp.json.str
import com.forkmesh.nativeapp.json.strings

object DesktopStatsMethod {
    const val TIMELINE = "stats.timeline"
    const val QUALITY = "stats.quality"
    const val REPO = "stats.repo"
    const val RATCHET_PREPARE = "stats.ratchet.prepare"
    const val RATCHET_RUN = "stats.ratchet.run"
    const val SENTRY = "stats.sentry"
    const val TILES = "stats.tiles"
    const val HEALTH = "diag.health"
}

val DesktopHealth.tone: StatsTone
    get() = when (level) {
        DesktopHealthLevel.Strained -> StatsTone.Critical
        DesktopHealthLevel.Busy -> StatsTone.Warning
        DesktopHealthLevel.Ok -> StatsTone.Good
        DesktopHealthLevel.Unknown -> StatsTone.None
    }

data class TimelineDay(val dayTs: Long, val errors: Int, val warnings: Int, val info: Int) {
    val total: Int get() = errors + warnings + info
}

data class LogTimeline(val days: List<TimelineDay>) {
    val errorSeries: StatsTileSeries get() = StatsTileSeries(days.map { it.dayTs.toString() }, days.map { it.errors.toDouble() })

    val busiest: TimelineDay? get() = days.maxByOrNull { it.total }

    val maxTotal: Int get() = days.maxOfOrNull { it.total } ?: 0

    companion object {
        const val MAX_DAYS = 30

        fun fromJson(json: Map<String, Any?>) = LogTimeline(
            json.maps("days").takeLast(MAX_DAYS).map { TimelineDay(it.long("dayTs"), it.int("errors"), it.int("warnings"), it.int("info")) },
        )
    }
}

data class QualityFile(val path: String, val score: Int, val findings: List<String>)

data class RepoQuality(val score: Int, val grade: String, val computedAt: Long, val files: List<QualityFile>) {
    val tone: StatsTone get() = if (score >= 80) StatsTone.Good else if (score >= 60) StatsTone.Warning else StatsTone.Critical

    companion object {
        const val MAX_FILES = 200

        fun fromJson(json: Map<String, Any?>) = RepoQuality(
            score = json.int("score"),
            grade = json.str("grade"),
            computedAt = json.long("computedAt"),
            files = json.maps("files").take(MAX_FILES)
                .map { QualityFile(it.str("path"), it.int("score"), it.strings("findings").take(10)) }
                .sortedWith(compareBy<QualityFile> { it.score }.thenBy { it.path }),
        )
    }
}

data class RepoStatsSnapshot(val commits: Int, val files: Int, val lines: Int, val ratchetEnabled: Boolean, val ratchetNote: String) {
    companion object {
        fun fromJson(json: Map<String, Any?>): RepoStatsSnapshot {
            val ratchet = json.obj("ratchet")
            return RepoStatsSnapshot(json.int("commits"), json.int("files"), json.int("lines"), ratchet.bool("enabled"), ratchet.str("note"))
        }
    }
}

data class SentryIssue(val id: String, val title: String, val project: String, val level: String, val count: Int, val lastSeen: Long) {
    val tone: StatsTone
        get() = when (level) {
            "fatal", "error" -> StatsTone.Critical
            "warning" -> StatsTone.Warning
            else -> StatsTone.None
        }
}

data class DesktopConfirmation(val token: String, val summary: String, val expiresAt: Long) {
    companion object {
        fun fromJson(json: Map<String, Any?>): DesktopConfirmation? {
            val token = json.str("token")
            if (token.isEmpty()) return null
            return DesktopConfirmation(token, json.str("summary"), json.long("expiresAt"))
        }
    }
}

class DesktopStatsClient(private val channel: ControlCalling) {
    fun health(): DesktopHealth = DesktopHealth.fromJson(channel.request(DesktopStatsMethod.HEALTH, emptyMap(), 15_000))

    fun tiles(): DesktopTilesReply = DesktopTiles.fromJson(channel.request(DesktopStatsMethod.TILES, emptyMap(), 20_000))

    fun timeline(days: Int = 30): LogTimeline = LogTimeline.fromJson(
        channel.request(DesktopStatsMethod.TIMELINE, mapOf("days" to days.coerceIn(1, LogTimeline.MAX_DAYS).toLong()), 20_000),
    )

    fun quality(repo: String): RepoQuality = RepoQuality.fromJson(channel.request(DesktopStatsMethod.QUALITY, mapOf("repo" to repo), 45_000))

    fun repoStats(repo: String): RepoStatsSnapshot =
        RepoStatsSnapshot.fromJson(channel.request(DesktopStatsMethod.REPO, mapOf("repo" to repo), 30_000))

    fun prepareRatchet(repo: String, enabled: Boolean): DesktopConfirmation {
        val reply = channel.request(DesktopStatsMethod.RATCHET_PREPARE, mapOf("repo" to repo, "enabled" to enabled), 20_000)
        return DesktopConfirmation.fromJson(reply)
            ?: throw InsightsException("bad_answer", "The desktop sent an answer this app does not understand.")
    }

    fun runRatchet(confirmation: DesktopConfirmation): RepoStatsSnapshot =
        RepoStatsSnapshot.fromJson(channel.request(DesktopStatsMethod.RATCHET_RUN, mapOf("token" to confirmation.token), 30_000))

    fun sentry(): List<SentryIssue> = channel.request(DesktopStatsMethod.SENTRY, emptyMap(), 30_000).maps("issues").take(100).map {
        SentryIssue(it.str("id"), it.str("title"), it.str("project"), it.str("level"), it.int("count"), it.long("lastSeen"))
    }

    companion object {
        fun explain(error: Throwable): String {
            if (isUnsupportedControl(error)) return "Your desktop does not offer this yet. Update ForkMesh on the desktop."
            if (error is InsightsException) return error.message
            val text = if (error is LocalControlException) error.code else error.message ?: error.toString()
            if ("sentry_not_installed" in text) return "Install the Sentry package on the desktop."
            if ("sentry_not_connected" in text) return "Connect Sentry in the desktop's Sentry package."
            if (listOf("timeout", "timed_out").any { it in text }) return "Your desktop took too long to answer. Refresh to try again."
            if (listOf("not_paired", "not_connected", "offline", "unreachable").any { it in text }) {
                return "Your desktop is not reachable right now."
            }
            if (listOf("expired", "bad_token").any { it in text }) return "The confirmation expired. Try again."
            return text
        }
    }
}

sealed class DesktopLoad<out T> {
    data object Idle : DesktopLoad<Nothing>()
    data object Loading : DesktopLoad<Nothing>()
    data object Unsupported : DesktopLoad<Nothing>()
    data class Failed(val message: String) : DesktopLoad<Nothing>()
    data class Loaded<T>(val value: T) : DesktopLoad<T>()

    val valueOrNull: T? get() = (this as? Loaded<T>)?.value

    companion object {
        fun <T> from(error: Throwable): DesktopLoad<T> =
            if (isUnsupportedControl(error)) Unsupported else Failed(DesktopStatsClient.explain(error))

        fun <T> attempt(fetch: () -> T): DesktopLoad<T> = try {
            Loaded(fetch())
        } catch (error: Exception) {
            from(error)
        }
    }
}
