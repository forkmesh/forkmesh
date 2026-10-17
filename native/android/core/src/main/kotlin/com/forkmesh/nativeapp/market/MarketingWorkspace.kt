package com.forkmesh.nativeapp.market

import com.forkmesh.nativeapp.format.relativeTimeFrom
import com.forkmesh.nativeapp.json.JsonMap
import com.forkmesh.nativeapp.json.bool
import com.forkmesh.nativeapp.json.double
import com.forkmesh.nativeapp.json.int
import com.forkmesh.nativeapp.json.long
import com.forkmesh.nativeapp.json.maps
import com.forkmesh.nativeapp.json.obj
import com.forkmesh.nativeapp.json.str
import com.forkmesh.nativeapp.json.strings
import java.util.Base64
import kotlin.math.abs
import kotlin.math.roundToLong

enum class MarketingKind(val resource: String, val replyKey: String, val title: String) {
    CONTENT("content", "content", "Content"),
    CAMPAIGN("campaigns", "campaign", "Campaign"),
    AD("ads", "ad", "Ad"),
    AD_RESULTS("ads", "ad", "Ad results"),
    EXPERIMENT("experiments", "experiment", "Experiment"),
    FUNNEL_WEEK("funnel", "", "Funnel week"),
    KEYWORD("keywords", "keyword", "Keyword"),
    REDDIT_THREAD("reddit", "thread", "Reddit thread"),
    POST("posts", "post", "Posted link"),
    METRIC("metrics", "metric", "Channel numbers"),
    CHANNEL("settings", "", "Channel"),
    ATTRIBUTION("settings", "", "Link tracking"),
    TARGETS("settings", "", "Funnel targets"),
    GOALS("settings", "", "Conversion goals"),
    REDDIT_WATCH("settings", "", "Reddit watch list"),
    X_CONNECTION("x-connection", "", "Connect X"),
    X_POST("x-post", "", "Post to X"),
    LINK("links", "", "Tagged link");

    val statuses: List<String>
        get() = when (this) {
            CONTENT -> MarketingContent.STATUSES
            CAMPAIGN -> listOf("idea", "planned", "active", "paused", "completed", "archived")
            AD -> MarketingCampaignAd.STATUSES
            EXPERIMENT -> listOf("idea", "planned", "running", "review", "done", "dropped")
            REDDIT_THREAD -> listOf("to_review", "drafting", "replied", "skipped")
            else -> emptyList()
        }

    val deletable: Boolean get() = this in setOf(CONTENT, CAMPAIGN, AD, EXPERIMENT, FUNNEL_WEEK, KEYWORD, REDDIT_THREAD, POST)

    val isSettings: Boolean get() = this in setOf(CHANNEL, ATTRIBUTION, TARGETS, GOALS, REDDIT_WATCH)
}

object MarketingText {
    fun title(value: String): String {
        if (value.isEmpty()) return "None"
        return value.split('-', '_').filter { it.isNotEmpty() }.joinToString(" ") { part -> part.replaceFirstChar { it.uppercase() } }
    }

    fun count(value: Long): String {
        val magnitude = abs(value)
        val sign = if (value < 0) "-" else ""
        return when {
            magnitude >= 1_000_000 -> sign + trimmed(magnitude / 1_000_000.0) + "M"
            magnitude >= 10_000 -> sign + trimmed(magnitude / 1_000.0) + "K"
            else -> sign + magnitude
        }
    }

    fun signed(value: Long): String = (if (value >= 0) "+" else "") + count(value)

    fun percent(value: Double): String = trimmed(value) + "%"

    fun trimmed(value: Double): String {
        val rounded = (value * 10).roundToLong() / 10.0
        return if (rounded == Math.rint(rounded)) rounded.toLong().toString() else String.format(java.util.Locale.ROOT, "%.1f", rounded)
    }

    fun plural(count: Int, one: String, many: String): String = "$count " + if (count == 1) one else many

    fun counted(value: Long, one: String, many: String): String = count(value) + " " + if (value == 1L) one else many
}

private fun JsonMap.optLong(key: String): Long? = (this[key] as? Number)?.toLong()

private fun JsonMap.optDouble(key: String): Double? = (this[key] as? Number)?.toDouble()

data class MarketingTask(val title: String, val done: Boolean = false, val owner: String = "") {
    val json: Map<String, Any?> get() = linkedMapOf("title" to title, "done" to done, "owner" to owner)
}

data class MarketingDate(val label: String, val ms: Long)

data class MarketingNamedLink(val id: String, val name: String, val url: String = "")

data class MarketingRecord(val kind: MarketingKind, val json: JsonMap) {
    val id: String get() = json.str("id")
    val status: String get() = json.str("status")

    val title: String
        get() = when (kind) {
            MarketingKind.CAMPAIGN -> json.str("name")
            MarketingKind.KEYWORD -> json.str("term")
            MarketingKind.REDDIT_THREAD -> json.str("title").ifEmpty { "r/${json.str("subreddit")} thread" }
            MarketingKind.POST -> json.str("title").ifEmpty { json.str("url") }
            MarketingKind.FUNNEL_WEEK -> "Week"
            else -> json.str("title")
        }

    val tasks: List<MarketingTask> get() = json.maps("tasks").map { MarketingTask(it.str("title"), it.bool("done"), it.str("owner")) }

    fun facts(overview: MarketingOverview): List<String> = when (kind) {
        MarketingKind.CAMPAIGN -> {
            val stats = json.obj("stats")
            val parts = mutableListOf(MarketingText.title(json.str("objective")))
            val total = stats.int("tasksTotal")
            if (total > 0) parts.add("${stats.int("tasksDone")}/$total tasks")
            parts.add(MarketingText.plural(stats.int("content"), "post", "posts"))
            if (stats.int("ads") > 0) parts.add(MarketingText.plural(stats.int("ads"), "ad", "ads"))
            if (stats.int("daysLeft") > 0) parts.add(MarketingText.plural(stats.int("daysLeft"), "day left", "days left"))
            parts
        }
        MarketingKind.EXPERIMENT -> {
            val parts = mutableListOf(MarketingText.title(json.str("stage")))
            val score = json.int("score")
            parts.add(if (score > 0) "ICE $score" else "Not scored")
            if (json.str("decision").isNotEmpty()) parts.add(MarketingText.title(json.str("decision")))
            if (json.bool("reviewDue") || json.bool("ended")) parts.add("Review due")
            parts
        }
        MarketingKind.KEYWORD -> {
            val parts = mutableListOf<String>()
            val position = json.optDouble("position")
            if (position != null) {
                var text = "Position " + MarketingText.trimmed(position)
                val change = json.optDouble("positionChange")
                if (change != null && change != 0.0) text += " (" + (if (change > 0) "+" else "") + MarketingText.trimmed(change) + ")"
                parts.add(text)
            } else {
                parts.add("No ranking yet")
            }
            json.optLong("impressions")?.let { parts.add(MarketingText.count(it) + " impressions") }
            json.optLong("clicks")?.let { parts.add(MarketingText.count(it) + " clicks") }
            if (json.str("intent").isNotEmpty()) parts.add(MarketingText.title(json.str("intent")))
            if (json.int("priority") > 0) parts.add("Priority ${json.int("priority")}")
            parts
        }
        MarketingKind.REDDIT_THREAD -> {
            val parts = mutableListOf("r/" + json.str("subreddit"), MarketingText.title(json.str("status")))
            if (json.bool("monitored")) parts.add("Monitored")
            parts
        }
        MarketingKind.POST -> {
            val parts = mutableListOf(overview.channelName(json.str("channel")))
            for (key in listOf("views", "likes", "replies", "reposts")) json.optLong(key)?.let { parts.add(MarketingText.count(it) + " " + key) }
            parts
        }
        MarketingKind.FUNNEL_WEEK -> {
            val parts = mutableListOf<String>()
            for (key in listOf("visits", "signups", "activated", "retained")) json.optLong(key)?.let { parts.add(MarketingText.count(it) + " " + key) }
            parts.ifEmpty { mutableListOf("No numbers") }
        }
        else -> emptyList()
    }

    val dates: List<MarketingDate>
        get() {
            val pairs = when (kind) {
                MarketingKind.CAMPAIGN -> listOf("Starts" to "startsAt", "Ends" to "endsAt")
                MarketingKind.EXPERIMENT -> listOf("Starts" to "startsAt", "Ends" to "endsAt", "Review" to "reviewAt")
                MarketingKind.POST -> listOf("Posted" to "postedAt", "Checked" to "checkedAt")
                MarketingKind.REDDIT_THREAD -> listOf("Logged" to "createdAt")
                MarketingKind.KEYWORD -> listOf("Read" to "observedAt")
                MarketingKind.FUNNEL_WEEK -> listOf("Week of" to "weekStart")
                else -> emptyList()
            }
            return pairs.map { MarketingDate(it.first, json.long(it.second)) }.filter { it.ms > 0 }
        }

    val link: String
        get() = when (kind) {
            MarketingKind.CAMPAIGN -> json.obj("stats").str("taggedLink")
            MarketingKind.REDDIT_THREAD, MarketingKind.POST -> json.str("url")
            MarketingKind.KEYWORD -> json.str("targetUrl")
            else -> ""
        }

    val needsAttention: Boolean
        get() = when (kind) {
            MarketingKind.EXPERIMENT -> json.bool("reviewDue") || json.bool("ended")
            MarketingKind.REDDIT_THREAD -> status in setOf("to_review", "drafting")
            else -> false
        }

    fun redditActivity(now: Long): String {
        if (kind != MarketingKind.REDDIT_THREAD) return ""
        val parts = mutableListOf<String>()
        json.optLong("comments")?.let { parts.add(MarketingText.counted(it, "comment", "comments")) }
        json.optLong("score")?.let { parts.add(MarketingText.counted(it, "point", "points")) }
        val views = json.optLong("views")
        views?.let { parts.add(MarketingText.counted(it, "view", "views")) }
        val watching = json.str("monitorCommentUrl").isNotEmpty()
        val commentViews = if (watching) json.optLong("commentViews") else null
        if (watching) {
            val comment = listOfNotNull(
                commentViews?.let { MarketingText.counted(it, "view", "views") },
                json.optLong("commentScore")?.let { MarketingText.counted(it, "point", "points") },
            )
            if (comment.isNotEmpty()) parts.add("your comment: " + comment.joinToString(", "))
        }
        val checkedAt = json.long("checkedAt")
        if (views == null && commentViews == null && checkedAt > 0) {
            val viewer = json.str("commentAuthor").ifEmpty { json.str("author") }
            if (viewer.isNotEmpty()) parts.add("views show only to u/$viewer")
        }
        val readAs = json.str("readAs").let { if (it.isEmpty()) "" else " as u/$it" }
        parts.add(if (checkedAt > 0) "read " + relativeTimeFrom(checkedAt, now) + readAs else "not read yet")
        return parts.joinToString(" · ")
    }

    val redditReplyPrompt: String
        get() {
            if (kind != MarketingKind.REDDIT_THREAD) return ""
            val text = json.str("body").ifEmpty { json.str("text") }
            return """
                Draft one helpful, genuine Reddit reply to the post below. Help the author first and mention ForkMesh only when it directly solves their problem. Do not use hype, unverifiable claims, or a salesy tone. Respect r/${json.str("subreddit")}'s rules, disclose that I work on or contribute to ForkMesh if recommending it, and use at most one relevant link. Return only a concise draft for me to review and rewrite before posting.

                Reddit post
                Subreddit: r/${json.str("subreddit")}
                Title: $title
                URL: ${json.str("url")}
                Text: $text

                ForkMesh links
                https://forkmesh.com/
                https://forkmesh.com/docs/
                https://github.com/forkmesh/forkmesh
            """.trimIndent()
        }

    fun matches(query: String): Boolean = PackagesText.matches(
        query, title, json.str("campaign"), json.str("owner"), json.str("subreddit"), json.str("url"), json.str("note"),
        json.str("notes"), json.str("hypothesis"),
    )
}

data class MarketingChannelSetting(
    val id: String,
    val enabled: Boolean,
    val displayName: String,
    val profileUrl: String,
    val hasAccessToken: Boolean,
    val lastReadAt: Long,
    val lastError: String,
) {
    val payload: Map<String, Any?> get() = linkedMapOf("id" to id, "enabled" to enabled, "displayName" to displayName, "profileUrl" to profileUrl)

    val takesToken: Boolean get() = id in TOKEN_CHANNELS

    companion object {
        val TOKEN_CHANNELS = setOf("x", "github")

        fun fromJson(json: JsonMap): MarketingChannelSetting {
            val feed = json.obj("feed")
            return MarketingChannelSetting(
                json.str("id"), (json["enabled"] as? Boolean) ?: true, json.str("displayName"), json.str("profileUrl"),
                json.bool("hasAccessToken"), feed.long("lastReadAt"), feed.str("lastError"),
            )
        }
    }
}

data class MarketingSuggestion(val level: String, val code: String, val title: String, val detail: String, val campaign: String, val action: JsonMap) {
    val id: String get() = "$code:$campaign:$title"
    val actionType: String get() = action.str("type")

    companion object {
        fun fromJson(json: JsonMap) = MarketingSuggestion(
            json.str("level", "info"), json.str("code"), json.str("title"), json.str("detail"), json.str("campaign"), json.obj("action"),
        )
    }
}

data class MarketingAdTotals(
    val currency: String,
    val ads: Int,
    val active: Int,
    val spendCents: Long,
    val budgetCents: Long,
    val impressions: Long,
    val clicks: Long,
    val conversions: Long,
    val ctr: Double,
    val roas: Double,
) {
    val facts: List<String>
        get() {
            val parts = mutableListOf(MoneyText.cents(spendCents, currency) + " spent of " + MoneyText.cents(budgetCents, currency))
            parts.add(MarketingText.count(impressions) + " impressions")
            parts.add(MarketingText.count(clicks) + " clicks")
            if (ctr > 0) parts.add(MarketingText.percent(ctr) + " CTR")
            if (conversions > 0) parts.add(MarketingText.count(conversions) + " conversions")
            if (roas > 0) parts.add(MarketingText.trimmed(roas) + "x ROAS")
            return parts
        }

    companion object {
        fun fromJson(json: JsonMap) = MarketingAdTotals(
            json.str("currency"), json.int("ads"), json.int("active"), json.long("spendCents"), json.long("budgetCents"),
            json.long("impressions"), json.long("clicks"), json.long("conversions"), json.double("ctr"), json.double("roas"),
        )
    }
}

data class MarketingFunnelRate(val id: String, val label: String, val value: Double?, val change: Double?, val target: Double, val small: Boolean) {
    val text: String
        get() {
            val current = value ?: return "No data"
            val parts = mutableListOf(MarketingText.percent(current))
            if (target > 0) parts.add("target " + MarketingText.percent(target))
            if (change != null && change != 0.0) parts.add((if (change > 0) "+" else "") + MarketingText.trimmed(change) + " pts")
            if (small) parts.add("small sample")
            return parts.joinToString(" · ")
        }

    val belowTarget: Boolean get() = value != null && target > 0 && value < target

    companion object {
        fun fromJson(json: JsonMap) = MarketingFunnelRate(
            json.str("key"), json.str("label"), json.optDouble("value"), json.optDouble("change"), json.double("target"), json.bool("small"),
        )
    }
}

data class MarketingFunnelReport(val weekStart: Long, val rates: List<MarketingFunnelRate>, val bottleneck: String, val bottleneckReason: String) {
    companion object {
        fun fromJson(json: JsonMap) = MarketingFunnelReport(
            json.long("weekStart"), json.maps("rates").map(MarketingFunnelRate::fromJson), json.str("bottleneck"), json.str("bottleneckReason"),
        )
    }
}

data class MarketingXStatus(val connected: Boolean, val username: String, val connectedBy: String, val lastPostedAt: Long, val lastPostUrl: String) {
    companion object {
        fun fromJson(json: JsonMap) = MarketingXStatus(
            json.bool("connected"), json.str("username"), json.str("connectedBy"), json.long("lastPostedAt"), json.str("lastPostUrl"),
        )
    }
}

data class MarketingReadyPost(
    val id: String,
    val text: String,
    val length: Int,
    val imageDataUrl: String = "",
    val hasImage: Boolean = false,
    val imageAlt: String = "",
    val why: String = "",
    val link: String = "",
    val model: String = "",
    val author: String = "",
    val createdAt: Long = 0,
    val intentUrl: String = "",
) {
    val overLimit: Boolean get() = length > LIMIT
    val lengthText: String get() = "$length/$LIMIT"
    val writer: String get() = model.ifEmpty { author }.ifEmpty { "Herald" }

    fun written(relative: String): String = "Written $relative by $writer"

    val imageMime: String
        get() {
            val head = imageDataUrl.substringBefore(',', "")
            if (!head.startsWith("data:") || !head.endsWith(";base64")) return ""
            return head.removePrefix("data:").removeSuffix(";base64").lowercase().takeIf { it in IMAGE_TYPES.keys }.orEmpty()
        }

    val imageExtension: String get() = IMAGE_TYPES[imageMime].orEmpty()

    fun imageBytes(): ByteArray? {
        if (imageMime.isEmpty()) return null
        val data = imageDataUrl.substringAfter(',').filterNot { it.isWhitespace() }
        return runCatching { Base64.getDecoder().decode(data) }.getOrNull()?.takeIf { it.isNotEmpty() }
    }

    companion object {
        const val LIMIT = 280
        val IMAGE_TYPES = mapOf("image/png" to "png", "image/jpeg" to "jpg", "image/webp" to "webp")

        fun fromJson(json: JsonMap): MarketingReadyPost {
            val text = json.str("text")
            val image = json.str("imageDataUrl")
            return MarketingReadyPost(
                json.str("id"), text, json.int("length", text.length), image, json.bool("hasImage") || image.isNotEmpty(),
                json.str("imageAlt"), json.str("why"), json.str("link"), json.str("model"), json.str("author"), json.long("createdAt"),
                json.str("intentUrl"),
            )
        }
    }
}

data class MarketingReadyClosed(
    val id: String,
    val text: String,
    val why: String,
    val status: String,
    val createdAt: Long,
    val closedAt: Long,
    val closedBy: String,
    val url: String,
) {
    val statusTitle: String get() = MarketingText.title(status)

    companion object {
        fun fromJson(json: JsonMap) = MarketingReadyClosed(
            json.str("id"), json.str("text"), json.str("why"), json.str("status", "replaced"), json.long("createdAt"),
            json.long("closedAt"), json.str("closedBy"), json.str("url"),
        )
    }
}

data class MarketingReadyPosts(val channel: String, val needsPost: Boolean, val post: MarketingReadyPost?, val history: List<MarketingReadyClosed>) {
    val last: MarketingReadyClosed? get() = history.firstOrNull()

    companion object {
        const val EMPTY_TEXT = "No post ready yet. Herald (the X special agent) writes the next one."
        val CLOSE_STATUSES = listOf("posted", "skipped")

        fun fromJson(json: JsonMap, channel: String = "x"): MarketingReadyPosts {
            val post = json.obj("post").takeIf { it.isNotEmpty() }?.let(MarketingReadyPost::fromJson)?.takeIf { PackagesText.isHexId(it.id) }
            return MarketingReadyPosts(
                json.str("channel", channel), post == null, post,
                json.maps("history").map(MarketingReadyClosed::fromJson).filter { it.id.isNotEmpty() },
            )
        }
    }
}

data class MarketingRedditSubreddit(val name: String, val url: String, val rulesUrl: String, val rulesRead: Boolean, val rules: List<String>) {
    companion object {
        fun fromJson(json: JsonMap) = MarketingRedditSubreddit(
            json.str("name"), json.str("url"), json.str("rulesUrl"), json.bool("rulesRead"),
            json.maps("rules").map { it.str("title") }.filter { it.isNotEmpty() },
        )
    }
}

data class MarketingIdea(val kind: String, val title: String, val detail: String, val channel: String, val copy: String, val inDays: Int) {
    val actionTitle: String
        get() = when (kind) {
            "content" -> "Add to plan"
            "task" -> "Add as task"
            "keyword" -> "Track keyword"
            "campaign" -> "Start campaign"
            else -> ""
        }

    companion object {
        fun fromJson(json: JsonMap) = MarketingIdea(
            json.str("kind"), json.str("title"), json.str("detail"), json.str("channel"), json.str("copy"), json.int("inDays"),
        )
    }
}

data class MarketingAdvice(val summary: String, val campaignId: String, val suggestions: List<MarketingIdea>) {
    companion object {
        fun fromJson(json: JsonMap) = MarketingAdvice(json.str("summary"), json.str("campaignId"), json.maps("suggestions").map(MarketingIdea::fromJson))
    }
}

data class MarketingPerformanceRow(val label: String, val sessions: Long, val conversions: Long?, val bounceRate: Double) {
    val facts: String
        get() {
            val parts = mutableListOf(MarketingText.count(sessions) + " visits")
            conversions?.let { parts.add(MarketingText.count(it) + " conversions") }
            if (bounceRate > 0) parts.add(MarketingText.percent(bounceRate * 100) + " bounce")
            return parts.joinToString(" · ")
        }

    companion object {
        fun fromJson(json: JsonMap, label: String) =
            MarketingPerformanceRow(label, json.long("sessions"), json.optLong("conversions"), json.double("bounceRate"))
    }
}

data class MarketingPerformance(
    val status: String,
    val from: Long,
    val to: Long,
    val sessions: Long,
    val previousSessions: Long,
    val conversions: Long?,
    val goals: List<String>,
    val insights: List<MarketingSuggestion>,
    val campaigns: List<MarketingPerformanceRow>,
    val channels: List<MarketingPerformanceRow>,
    val landing: List<MarketingPerformanceRow>,
    val projects: List<MarketingNamedLink>,
    val project: String,
) {
    val ok: Boolean get() = status == "ok"
    val sessionsChange: Long get() = sessions - previousSessions

    companion object {
        fun fromJson(json: JsonMap): MarketingPerformance {
            val totals = json.obj("totals")
            return MarketingPerformance(
                json.str("status", "unavailable"), json.long("from"), json.long("to"), totals.long("sessions"),
                json.obj("previous").long("sessions"), totals.optLong("conversions"), json.strings("goals"),
                json.maps("insights").map(MarketingSuggestion::fromJson),
                json.maps("campaigns").map { row ->
                    val campaign = row.str("campaign")
                    val label = if (campaign.isNotEmpty() && campaign != "(none)") "$campaign (${row.str("source")} / ${row.str("medium")})"
                    else "${row.str("source")} / ${row.str("medium")}"
                    MarketingPerformanceRow.fromJson(row, label)
                },
                json.maps("channels").map { MarketingPerformanceRow.fromJson(it, it.str("label")) },
                json.maps("landing").map { MarketingPerformanceRow.fromJson(it, it.str("landing", it.str("value"))) },
                json.maps("projects").map { MarketingNamedLink(it.str("id"), it.str("name")) },
                json.str("project"),
            )
        }
    }
}

data class MarketingReport(val generatedAt: Long, val markdown: String)

data class MarketingLink(val url: String, val checks: List<String>) {
    companion object {
        fun fromJson(json: JsonMap) = MarketingLink(json.str("url"), json.maps("checks").map { it.str("message") }.filter { it.isNotEmpty() })
    }
}

object MarketingKeywordImport {
    fun rows(source: String): List<Map<String, Any?>> {
        val lines = source.lines().map { it.trim() }.filter { it.isNotEmpty() }
        val first = lines.firstOrNull() ?: return emptyList()
        val separator = when {
            '\t' in first -> '\t'
            ';' in first && ',' !in first -> ';'
            else -> ','
        }
        val head = cells(first, separator).map { it.lowercase() }
        fun find(names: List<String>): Int? = head.indexOfFirst { cell -> names.any { cell.contains(it) } }.takeIf { it >= 0 }
        val term = find(listOf("quer", "keyword", "search term", "term"))
        val clicks = find(listOf("click"))
        val impressions = find(listOf("impression"))
        val position = find(listOf("position", "rank"))
        val header = term != null && (clicks != null || impressions != null || position != null || head.size == 1)
        val rows = mutableListOf<Map<String, Any?>>()
        for (line in lines.drop(if (header) 1 else 0)) {
            val row = linkedMapOf<String, Any?>()
            if (header && term != null) {
                val values = cells(line, separator)
                fun cell(index: Int?): String = index?.let { values.getOrNull(it) } ?: ""
                row["term"] = cell(term)
                row["clicks"] = if (clicks == null) null else number(cell(clicks), true)
                row["impressions"] = if (impressions == null) null else number(cell(impressions), true)
                row["position"] = if (position == null) null else number(cell(position), false)
            } else {
                var value = line.split(separator, limit = 2).first()
                if (value.startsWith("\"")) value = value.drop(1)
                if (value.endsWith("\"")) value = value.dropLast(1)
                row["term"] = value
            }
            if (row.str("term").isNotEmpty()) rows.add(row)
        }
        return rows
    }

    fun top(rows: List<Map<String, Any?>>, limit: Int): List<Map<String, Any?>> =
        rows.sortedWith(compareByDescending<Map<String, Any?>> { it.long("clicks") }.thenByDescending { it.long("impressions") }).take(maxOf(1, limit))

    internal fun cells(line: String, separator: Char): List<String> {
        val result = mutableListOf<String>()
        val current = StringBuilder()
        var quoted = false
        var index = 0
        while (index < line.length) {
            val character = line[index]
            if (character == '"') {
                if (quoted && index + 1 < line.length && line[index + 1] == '"') {
                    current.append('"')
                    index++
                } else {
                    quoted = !quoted
                }
            } else if (character == separator && !quoted) {
                result.add(current.toString().trim())
                current.clear()
            } else {
                current.append(character)
            }
            index++
        }
        result.add(current.toString().trim())
        return result
    }

    internal fun number(value: String, whole: Boolean): Any? {
        var clean = value.filter { it != '%' && !it.isWhitespace() }
        clean = clean.replace(Regex(",(?=\\d{3}\\b)"), "")
        val parsed = clean.replace(',', '.').toDoubleOrNull() ?: return null
        if (clean.isEmpty()) return null
        return if (whole) parsed.roundToLong() else parsed
    }
}
