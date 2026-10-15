package com.forkmesh.nativeapp.market

import com.forkmesh.nativeapp.json.JsonMap
import com.forkmesh.nativeapp.json.bool
import com.forkmesh.nativeapp.json.long
import com.forkmesh.nativeapp.json.maps
import com.forkmesh.nativeapp.json.obj
import com.forkmesh.nativeapp.json.str
import com.forkmesh.nativeapp.json.strings
import java.time.Instant
import java.time.LocalDate
import java.time.LocalDateTime
import java.time.LocalTime
import java.time.ZoneId

sealed class MarketingFieldKind {
    object Text : MarketingFieldKind()
    object Multiline : MarketingFieldKind()
    object Url : MarketingFieldKind()
    object Secret : MarketingFieldKind()
    data class Choice(val options: List<String>) : MarketingFieldKind()
    data class Channel(val options: List<String>) : MarketingFieldKind()
    data class Channels(val options: List<String>) : MarketingFieldKind()
    object Date : MarketingFieldKind()
    object DateTime : MarketingFieldKind()
    object Money : MarketingFieldKind()
    object Count : MarketingFieldKind()
    object Decimal : MarketingFieldKind()
    object Toggle : MarketingFieldKind()
    object ListText : MarketingFieldKind()
    object Lines : MarketingFieldKind()
    object Tasks : MarketingFieldKind()
}

data class MarketingField(
    val key: String,
    val label: String,
    val kind: MarketingFieldKind = MarketingFieldKind.Text,
    val required: Boolean = false,
    val optional: Boolean = false,
    val hint: String = "",
)

class MarketingFormException(message: String) : IllegalArgumentException(message)

data class MarketingDraft(
    val kind: MarketingKind,
    val id: String,
    val fields: List<MarketingField>,
    val values: Map<String, String>,
    val original: JsonMap,
) {
    val isNew: Boolean get() = id.isEmpty()

    operator fun get(key: String): String = values[key].orEmpty()

    fun with(key: String, value: String): MarketingDraft = copy(values = values + (key to value))

    val title: String
        get() = when (kind) {
            MarketingKind.METRIC -> MarketingText.title(original.str("channel")) + " numbers"
            MarketingKind.CHANNEL -> original.str("displayName", MarketingKind.CHANNEL.title)
            else -> (if (isNew) "New " else "Edit ") + kind.title.lowercase()
        }

    fun body(overview: MarketingOverview, now: Long, zone: ZoneId = ZoneId.systemDefault()): Map<String, Any?> {
        val body = linkedMapOf<String, Any?>()
        for (field in fields) {
            val raw = values[field.key].orEmpty().trim()
            if (field.required && raw.isEmpty()) throw MarketingFormException("${field.label} is required.")
            if (field.key == "currency") {
                if (raw.isNotEmpty()) body["currency"] = raw.uppercase()
                continue
            }
            val value = MarketingForms.value(field, raw, original, zone)
            if (value === MarketingForms.Skip) continue
            if (field.key.startsWith("snapshot.")) {
                @Suppress("UNCHECKED_CAST")
                val snapshot = (body["snapshot"] as? LinkedHashMap<String, Any?>) ?: linkedMapOf<String, Any?>().also { body["snapshot"] = it }
                snapshot[field.key.removePrefix("snapshot.")] = value
            } else {
                body[field.key] = value
            }
        }
        return MarketingForms.shape(kind, body, this, overview, now)
    }

    companion object {
        fun of(kind: MarketingKind, overview: MarketingOverview, json: JsonMap = emptyMap(), id: String = "", zone: ZoneId = ZoneId.systemDefault()): MarketingDraft {
            val fields = MarketingForms.fields(kind, overview, json)
            return MarketingDraft(kind, id, fields, fields.associate { it.key to MarketingForms.text(it, json, zone) }, json)
        }
    }
}

object MarketingForms {
    internal object Skip

    val CAMPAIGN_OBJECTIVES = listOf("awareness", "traffic", "signups", "activation", "engagement", "launch", "community", "sales")
    val AD_OBJECTIVES = listOf("awareness", "traffic", "engagement", "video-views", "leads", "app-installs", "sales")
    val EXPERIMENT_STAGES = listOf("acquisition", "activation", "retention", "referral", "revenue", "measurement")
    val EXPERIMENT_DECISIONS = listOf("", "keep", "iterate", "revert", "extend")
    val KEYWORD_INTENTS = listOf("", "informational", "navigational", "commercial", "transactional")
    val FUNNEL_RATES = listOf("signupRate" to "Signup rate", "activationRate" to "Activation rate", "retentionRate" to "Retention rate", "referralRate" to "Referral share")
    val GOAL_OPERATORS = listOf("exact", "icontains")
    private val NUMERIC_CHOICES = setOf("priority", "impact", "confidence", "ease")

    fun fields(kind: MarketingKind, overview: MarketingOverview, json: JsonMap = emptyMap()): List<MarketingField> {
        val configured = overview.channelSettings.filter { it.enabled }.map { it.id }
        val channels = configured + overview.catalog.map { it.id }.filter { it !in configured }
        return when (kind) {
            MarketingKind.CONTENT -> listOf(
                MarketingField("title", "Title", required = true),
                MarketingField("channel", "Channel", MarketingFieldKind.Channel(channels)),
                MarketingField("status", "Status", MarketingFieldKind.Choice(kind.statuses)),
                MarketingField("campaign", "Campaign", hint = "Campaign name"),
                MarketingField("scheduledAt", "Scheduled", MarketingFieldKind.DateTime),
                MarketingField("owner", "Owner"),
                MarketingField("url", "Published link", MarketingFieldKind.Url),
                MarketingField("tags", "Tags", MarketingFieldKind.ListText, hint = "Comma separated"),
                MarketingField("body", "Copy", MarketingFieldKind.Multiline),
                MarketingField("notes", "Notes", MarketingFieldKind.Multiline),
            )
            MarketingKind.CAMPAIGN -> listOf(
                MarketingField("name", "Name", required = true),
                MarketingField("status", "Status", MarketingFieldKind.Choice(kind.statuses)),
                MarketingField("objective", "Objective", MarketingFieldKind.Choice(CAMPAIGN_OBJECTIVES)),
                MarketingField("startsAt", "Starts", MarketingFieldKind.Date),
                MarketingField("endsAt", "Ends", MarketingFieldKind.Date),
                MarketingField("owner", "Owner"),
                MarketingField("audience", "Audience", MarketingFieldKind.Multiline),
                MarketingField("message", "Message", MarketingFieldKind.Multiline),
                MarketingField("offer", "Offer"),
                MarketingField("landingUrl", "Landing page", MarketingFieldKind.Url),
                MarketingField("slug", "Link tag (utm_campaign)", hint = "Made from the name when empty"),
                MarketingField("channels", "Channels", MarketingFieldKind.Channels(channels)),
                MarketingField("hashtags", "Hashtags", MarketingFieldKind.ListText, hint = "Comma separated"),
                MarketingField("keywords", "Keywords", MarketingFieldKind.ListText, hint = "Comma separated"),
                MarketingField("currency", "Currency", hint = "USD"),
                MarketingField("budgetCents", "Budget", MarketingFieldKind.Money),
                MarketingField("targetVisits", "Target visits", MarketingFieldKind.Count, optional = true),
                MarketingField("targetConversions", "Target conversions", MarketingFieldKind.Count, optional = true),
                MarketingField("tasks", "Tasks", MarketingFieldKind.Tasks, hint = "One per line; start a line with [x] when done"),
                MarketingField("brief", "Brief", MarketingFieldKind.Multiline),
                MarketingField("notes", "Notes", MarketingFieldKind.Multiline),
            )
            MarketingKind.AD -> {
                val platforms = overview.adPlatforms.map { it.id }
                listOf(
                    MarketingField("name", "Name", required = true),
                    MarketingField("channel", "Platform", MarketingFieldKind.Channel(platforms.ifEmpty { channels })),
                    MarketingField("status", "Status", MarketingFieldKind.Choice(kind.statuses)),
                    MarketingField("objective", "Objective", MarketingFieldKind.Choice(AD_OBJECTIVES)),
                    MarketingField("campaign", "Campaign"),
                    MarketingField("headline", "Headline"),
                    MarketingField("body", "Ad copy", MarketingFieldKind.Multiline),
                    MarketingField("audience", "Audience", MarketingFieldKind.Multiline),
                    MarketingField("destinationUrl", "Destination link", MarketingFieldKind.Url),
                    MarketingField("managerUrl", "Ads manager link", MarketingFieldKind.Url),
                    MarketingField("budgetType", "Budget type", MarketingFieldKind.Choice(listOf("lifetime", "daily"))),
                    MarketingField("currency", "Currency", hint = "USD"),
                    MarketingField("budgetCents", "Budget", MarketingFieldKind.Money),
                    MarketingField("startsAt", "Starts", MarketingFieldKind.Date),
                    MarketingField("endsAt", "Ends", MarketingFieldKind.Date),
                    MarketingField("owner", "Owner"),
                    MarketingField("tags", "Tags", MarketingFieldKind.ListText, hint = "Comma separated"),
                    MarketingField("notes", "Notes", MarketingFieldKind.Multiline),
                )
            }
            MarketingKind.AD_RESULTS -> listOf(
                MarketingField("spendCents", "Spend", MarketingFieldKind.Money),
                MarketingField("impressions", "Impressions", MarketingFieldKind.Count),
                MarketingField("clicks", "Clicks", MarketingFieldKind.Count),
                MarketingField("conversions", "Conversions", MarketingFieldKind.Count),
                MarketingField("revenueCents", "Revenue", MarketingFieldKind.Money),
            )
            MarketingKind.EXPERIMENT -> {
                val scores = listOf("") + (1..10).map { it.toString() }
                listOf(
                    MarketingField("title", "Title", required = true),
                    MarketingField("status", "Status", MarketingFieldKind.Choice(kind.statuses)),
                    MarketingField("stage", "Funnel stage", MarketingFieldKind.Choice(EXPERIMENT_STAGES)),
                    MarketingField("hypothesis", "Hypothesis", MarketingFieldKind.Multiline, hint = "If we … then … because …"),
                    MarketingField("audience", "Audience"),
                    MarketingField("metric", "Metric"),
                    MarketingField("baseline", "Baseline"),
                    MarketingField("target", "Target"),
                    MarketingField("impact", "Impact", MarketingFieldKind.Choice(scores)),
                    MarketingField("confidence", "Confidence", MarketingFieldKind.Choice(scores)),
                    MarketingField("ease", "Ease", MarketingFieldKind.Choice(scores)),
                    MarketingField("startsAt", "Starts", MarketingFieldKind.Date),
                    MarketingField("endsAt", "Ends", MarketingFieldKind.Date),
                    MarketingField("reviewAt", "Review on", MarketingFieldKind.Date),
                    MarketingField("owner", "Owner"),
                    MarketingField("campaign", "Campaign"),
                    MarketingField("result", "Result", MarketingFieldKind.Multiline),
                    MarketingField("decision", "Decision", MarketingFieldKind.Choice(EXPERIMENT_DECISIONS)),
                    MarketingField("notes", "Notes", MarketingFieldKind.Multiline),
                )
            }
            MarketingKind.FUNNEL_WEEK -> listOf(
                MarketingField("weekStart", "Week of", MarketingFieldKind.Date, required = true),
                MarketingField("visits", "Visits", MarketingFieldKind.Count, optional = true),
                MarketingField("signups", "Signups", MarketingFieldKind.Count, optional = true),
                MarketingField("activated", "Activated", MarketingFieldKind.Count, optional = true),
                MarketingField("retainedBase", "Retention base", MarketingFieldKind.Count, optional = true, hint = "Signups old enough to retain"),
                MarketingField("retained", "Retained", MarketingFieldKind.Count, optional = true),
                MarketingField("referrals", "Referrals", MarketingFieldKind.Count, optional = true),
                MarketingField("northStar", "North star", MarketingFieldKind.Count, optional = true),
                MarketingField("revenueCents", "Revenue", MarketingFieldKind.Money, optional = true),
                MarketingField("currency", "Currency", hint = "USD"),
                MarketingField("source", "Source", hint = "Where the numbers came from"),
                MarketingField("note", "Note", MarketingFieldKind.Multiline),
            )
            MarketingKind.KEYWORD -> listOf(
                MarketingField("term", "Search phrase", required = true),
                MarketingField("intent", "Intent", MarketingFieldKind.Choice(KEYWORD_INTENTS)),
                MarketingField("priority", "Priority", MarketingFieldKind.Choice(listOf("0", "1", "2", "3"))),
                MarketingField("targetUrl", "Page that should rank", MarketingFieldKind.Url),
                MarketingField("campaign", "Campaign"),
                MarketingField("notes", "Notes", MarketingFieldKind.Multiline),
                MarketingField("snapshot.position", "New reading: position", MarketingFieldKind.Decimal, optional = true),
                MarketingField("snapshot.impressions", "New reading: impressions", MarketingFieldKind.Count, optional = true),
                MarketingField("snapshot.clicks", "New reading: clicks", MarketingFieldKind.Count, optional = true),
            )
            MarketingKind.REDDIT_THREAD -> listOf(
                MarketingField("url", "Thread link", MarketingFieldKind.Url, required = true, hint = "https://www.reddit.com/r/…/comments/…"),
                MarketingField("title", "Title"),
                MarketingField("status", "Status", MarketingFieldKind.Choice(kind.statuses)),
                MarketingField("landingPath", "Landing path", hint = "/ or /pricing"),
                MarketingField("rulesChecked", "I read the subreddit rules", MarketingFieldKind.Toggle),
                MarketingField("disclosed", "My reply discloses who I work for", MarketingFieldKind.Toggle),
                MarketingField("replyUrl", "My reply link", MarketingFieldKind.Url),
                MarketingField("monitored", "Monitor this thread", MarketingFieldKind.Toggle),
                MarketingField("commentUrl", "Comment to monitor", MarketingFieldKind.Url),
                MarketingField("note", "Note", MarketingFieldKind.Multiline),
            )
            MarketingKind.POST -> listOf(
                MarketingField("url", "Link", MarketingFieldKind.Url, required = true),
                MarketingField("channel", "Channel", MarketingFieldKind.Channel(listOf("") + channels), hint = "Found from the link when empty"),
                MarketingField("title", "Title"),
                MarketingField("postedAt", "Posted", MarketingFieldKind.Date),
                MarketingField("views", "Views", MarketingFieldKind.Count, optional = true),
                MarketingField("likes", "Likes", MarketingFieldKind.Count, optional = true),
                MarketingField("replies", "Replies", MarketingFieldKind.Count, optional = true),
                MarketingField("reposts", "Reposts", MarketingFieldKind.Count, optional = true),
                MarketingField("note", "Note", MarketingFieldKind.Multiline),
            )
            MarketingKind.METRIC -> {
                val head = when (json.str("channel")) {
                    "google-search" -> listOf(
                        MarketingField("visits", "Visits from Google (30 days)", MarketingFieldKind.Count),
                        MarketingField("indexedPages", "Pages on Google", MarketingFieldKind.Count),
                    )
                    "email" -> listOf(
                        MarketingField("outbound", "Emails sent (30 days)", MarketingFieldKind.Count),
                        MarketingField("replies", "Replies", MarketingFieldKind.Count),
                        MarketingField("closedDeals", "Closed deals", MarketingFieldKind.Count),
                        MarketingField("closedValue", "Closed value", MarketingFieldKind.Count),
                    )
                    "github" -> listOf(
                        MarketingField("stars", "Stars", MarketingFieldKind.Count),
                        MarketingField("forks", "Forks", MarketingFieldKind.Count),
                        MarketingField("followers", "Followers", MarketingFieldKind.Count),
                    )
                    "app-store" -> listOf(
                        MarketingField("downloads", "Downloads (30 days)", MarketingFieldKind.Count),
                        MarketingField("redownloads", "Redownloads (30 days)", MarketingFieldKind.Count),
                        MarketingField("updates", "Updates (30 days)", MarketingFieldKind.Count),
                        MarketingField("ratings", "Ratings", MarketingFieldKind.Count),
                    )
                    else -> listOf(MarketingField("followers", "Followers", MarketingFieldKind.Count), MarketingField("posts", "Posts", MarketingFieldKind.Count))
                }
                head + listOf(
                    MarketingField("reach", "Reach", MarketingFieldKind.Count),
                    MarketingField("impressions", "Impressions", MarketingFieldKind.Count),
                    MarketingField("engagements", "Engagements", MarketingFieldKind.Count),
                    MarketingField("clicks", "Clicks", MarketingFieldKind.Count),
                    MarketingField("views", "Views", MarketingFieldKind.Count),
                    MarketingField("rating", "Rating (0–5)", MarketingFieldKind.Decimal, optional = true),
                    MarketingField("note", "Note"),
                )
            }
            MarketingKind.CHANNEL -> {
                val result = mutableListOf(
                    MarketingField("enabled", "Track this channel", MarketingFieldKind.Toggle),
                    MarketingField("displayName", "Name"),
                    MarketingField("profileUrl", "Channel link", MarketingFieldKind.Url),
                )
                if (json.str("id") in MarketingChannelSetting.TOKEN_CHANNELS) {
                    result.add(
                        MarketingField(
                            "accessToken", if (json.str("id") == "x") "X API bearer token" else "GitHub access token", MarketingFieldKind.Secret,
                            hint = if (json.bool("hasAccessToken")) "Saved; type a new one to replace it" else "Write-only",
                        ),
                    )
                    if (json.bool("hasAccessToken")) result.add(MarketingField("clearAccessToken", "Remove the saved token", MarketingFieldKind.Toggle))
                }
                result
            }
            MarketingKind.ATTRIBUTION -> listOf(
                MarketingField("landingUrl", "Default landing page", MarketingFieldKind.Url),
                MarketingField("sources", "Allowed utm_source values", MarketingFieldKind.ListText),
                MarketingField("mediums", "Allowed utm_medium values", MarketingFieldKind.ListText),
                MarketingField("campaigns", "Allowed utm_campaign values", MarketingFieldKind.ListText),
                MarketingField("domains", "Your domains", MarketingFieldKind.ListText, hint = "Links elsewhere are not checked"),
                MarketingField("untrackedPaths", "Paths without analytics", MarketingFieldKind.ListText, hint = "/docs, /app"),
            )
            MarketingKind.TARGETS -> FUNNEL_RATES.map { MarketingField(it.first, it.second + " target (%)", MarketingFieldKind.Decimal, optional = true) }
            MarketingKind.GOALS -> listOf(
                MarketingField("project", "Analytics project id", hint = "Empty uses the organization's default"),
                MarketingField("host", "Site host", hint = "forkmesh.com"),
                MarketingField("goals", "Goals", MarketingFieldKind.Lines, hint = "One per line: label | event | property | exact or icontains | value"),
            )
            MarketingKind.REDDIT_WATCH -> listOf(
                MarketingField("subreddits", "Subreddits", MarketingFieldKind.ListText, hint = "Comma separated, without r/"),
                MarketingField("queries", "Searches", MarketingFieldKind.Lines, hint = "One per line"),
            )
            MarketingKind.X_CONNECTION -> listOf(
                MarketingField("apiKey", "API key", MarketingFieldKind.Secret, required = true),
                MarketingField("apiSecret", "API key secret", MarketingFieldKind.Secret, required = true),
                MarketingField("accessToken", "Access token", MarketingFieldKind.Secret, required = true),
                MarketingField("accessSecret", "Access token secret", MarketingFieldKind.Secret, required = true),
            )
            MarketingKind.X_POST -> listOf(MarketingField("text", "Post", MarketingFieldKind.Multiline, required = true, hint = "Up to 280 characters on most accounts"))
            MarketingKind.LINK -> listOf(
                MarketingField("url", "Page", MarketingFieldKind.Url, hint = "Empty uses the default landing page"),
                MarketingField("source", "Source", hint = "x, reddit, newsletter"),
                MarketingField("medium", "Medium", hint = "social, email, cpc"),
                MarketingField("campaign", "Campaign"),
                MarketingField("content", "Content"),
                MarketingField("term", "Term"),
            )
        }
    }

    private fun optLong(json: JsonMap, key: String): Long? = (json[key] as? Number)?.toLong()

    fun text(field: MarketingField, json: JsonMap, zone: ZoneId): String {
        if (field.key.startsWith("snapshot.")) return ""
        val value = json[field.key]
        return when (val kind = field.kind) {
            MarketingFieldKind.Secret -> ""
            MarketingFieldKind.Toggle -> if (field.key == "enabled") (if ((value as? Boolean) ?: true) "true" else "") else if (value == true) "true" else ""
            is MarketingFieldKind.Channels, MarketingFieldKind.ListText -> json.strings(field.key).joinToString(", ")
            MarketingFieldKind.Lines -> if (field.key == "goals") {
                json.maps("goals").joinToString("\n") { goal ->
                    val parts = mutableListOf(goal.str("label"), goal.str("event"))
                    if (goal.str("property").isNotEmpty()) parts += listOf(goal.str("property"), goal.str("operator", "exact"), goal.str("value"))
                    parts.joinToString(" | ")
                }
            } else {
                json.strings(field.key).joinToString("\n")
            }
            MarketingFieldKind.Tasks -> json.maps(field.key).joinToString("\n") { (if (it.bool("done")) "[x] " else "") + it.str("title") }
            MarketingFieldKind.Date, MarketingFieldKind.DateTime -> {
                val ms = optLong(json, field.key) ?: 0
                if (ms > 0) MarketingDates.text(ms, kind == MarketingFieldKind.DateTime, zone) else ""
            }
            MarketingFieldKind.Money -> {
                val cents = optLong(json, field.key) ?: 0
                if (cents != 0L) MoneyText.cents(cents, "") else ""
            }
            MarketingFieldKind.Count -> {
                val number = optLong(json, field.key)
                if (number == null) "" else if (field.optional || number != 0L) number.toString() else ""
            }
            MarketingFieldKind.Decimal -> {
                val number = (value as? Number)?.toDouble()
                if (number == null || number == 0.0) "" else MarketingText.trimmed(number)
            }
            is MarketingFieldKind.Choice -> {
                if (field.key in NUMERIC_CHOICES) {
                    val number = optLong(json, field.key) ?: 0
                    if (number == 0L && "" in kind.options) "" else number.toString()
                } else {
                    json.str(field.key).ifEmpty { defaultChoice(field, kind.options) }
                }
            }
            is MarketingFieldKind.Channel -> json.str(field.key).ifEmpty { if ("" in kind.options) "" else kind.options.firstOrNull().orEmpty() }
            else -> json.str(field.key)
        }
    }

    private fun defaultChoice(field: MarketingField, options: List<String>): String = when (field.key) {
        "budgetType" -> "lifetime"
        "objective" -> "awareness"
        "stage" -> "acquisition"
        else -> options.firstOrNull().orEmpty()
    }

    internal fun value(field: MarketingField, raw: String, original: JsonMap, zone: ZoneId): Any? = when (field.kind) {
        MarketingFieldKind.Text, MarketingFieldKind.Multiline -> raw
        MarketingFieldKind.Url -> {
            if (raw.isNotEmpty() && !raw.lowercase().startsWith("https://")) throw MarketingFormException("${field.label} must start with https://.")
            raw
        }
        MarketingFieldKind.Secret -> if (raw.isEmpty()) Skip else raw
        is MarketingFieldKind.Choice -> if (field.key in NUMERIC_CHOICES) (raw.toLongOrNull() ?: 0L) else raw
        is MarketingFieldKind.Channel -> raw
        is MarketingFieldKind.Channels, MarketingFieldKind.ListText -> splitList(raw)
        MarketingFieldKind.Lines -> if (field.key == "goals") goals(raw) else raw.lines().map { it.trim() }.filter { it.isNotEmpty() }
        MarketingFieldKind.Tasks -> tasks(raw, original.maps(field.key)).map { it.json }
        MarketingFieldKind.Date, MarketingFieldKind.DateTime -> if (raw.isEmpty()) (if (field.optional) null else 0L) else {
            MarketingDates.parse(raw, zone) ?: throw MarketingFormException(
                "Write ${field.label.lowercase()} as YYYY-MM-DD" + if (field.kind == MarketingFieldKind.DateTime) " or YYYY-MM-DD HH:MM." else ".",
            )
        }
        MarketingFieldKind.Money -> if (raw.isEmpty()) (if (field.optional) null else 0L) else {
            MarketingMoney.cents(raw) ?: throw MarketingFormException("Write ${field.label.lowercase()} as an amount like 12.50.")
        }
        MarketingFieldKind.Count -> if (raw.isEmpty()) (if (field.optional) null else 0L) else {
            raw.replace(",", "").replace(" ", "").toLongOrNull()?.takeIf { it >= 0 } ?: throw MarketingFormException("${field.label} must be a whole number.")
        }
        MarketingFieldKind.Decimal -> if (raw.isEmpty()) (if (field.optional) null else 0L) else {
            raw.replace(',', '.').toDoubleOrNull()?.takeIf { it >= 0 } ?: throw MarketingFormException("${field.label} must be a number.")
        }
        MarketingFieldKind.Toggle -> raw == "true"
    }

    fun splitList(raw: String): List<String> {
        val seen = mutableListOf<String>()
        for (part in raw.split(',', ';', '\n')) {
            val item = part.trim()
            if (item.isNotEmpty() && item !in seen) seen.add(item)
        }
        return seen
    }

    fun tasks(raw: String, previous: List<JsonMap> = emptyList()): List<MarketingTask> = raw.lines().mapNotNull { line ->
        var text = line.trim()
        var done = false
        if (text.lowercase().startsWith("[x]")) {
            done = true
            text = text.drop(3).trim()
        } else if (text.startsWith("[ ]")) {
            text = text.drop(3).trim()
        }
        if (text.isEmpty()) null else MarketingTask(text, done, previous.firstOrNull { it.str("title") == text }?.str("owner").orEmpty())
    }

    private fun goals(raw: String): List<Map<String, Any?>> {
        val result = mutableListOf<Map<String, Any?>>()
        for (line in raw.lines()) {
            val parts = line.split('|').map { it.trim() }
            if (parts.all { it.isEmpty() }) continue
            val label = parts[0]
            val event = parts.getOrElse(1) { parts[0] }
            if (event.isEmpty()) throw MarketingFormException("Each goal needs an event name.")
            val goal = linkedMapOf<String, Any?>("label" to label.ifEmpty { event }, "event" to event)
            if (parts.size > 2 && parts[2].isNotEmpty()) {
                goal["property"] = parts[2]
                goal["operator"] = parts.getOrNull(3)?.takeIf { it in GOAL_OPERATORS } ?: "exact"
                goal["value"] = parts.getOrElse(4) { "" }
            }
            result.add(goal)
        }
        if (result.size > 5) throw MarketingFormException("Up to 5 goals.")
        return result
    }

    internal fun shape(kind: MarketingKind, input: LinkedHashMap<String, Any?>, draft: MarketingDraft, overview: MarketingOverview, now: Long): Map<String, Any?> {
        val body = LinkedHashMap(input)
        return when (kind) {
            MarketingKind.CONTENT -> {
                if (body["status"] == "published" && draft.original.long("publishedAt") == 0L) body["publishedAt"] = now
                body
            }
            MarketingKind.KEYWORD -> {
                @Suppress("UNCHECKED_CAST")
                val snapshot = body["snapshot"] as? LinkedHashMap<String, Any?>
                if (snapshot == null || snapshot.values.all { it == null }) {
                    body.remove("snapshot")
                } else {
                    snapshot["at"] = now
                    snapshot["source"] = "mobile"
                }
                body
            }
            MarketingKind.POST -> {
                for (key in listOf("views", "likes", "replies", "reposts")) if (body.containsKey(key) && body[key] == null) body.remove(key)
                if ((body["channel"] as? String).isNullOrEmpty()) body.remove("channel")
                if (body["postedAt"] == 0L && draft.original.long("postedAt") == 0L) body.remove("postedAt")
                body
            }
            MarketingKind.METRIC -> {
                body["channel"] = draft.original.str("channel")
                if (body["rating"] == null) body["rating"] = 0L
                body
            }
            MarketingKind.CHANNEL -> {
                val entry = linkedMapOf<String, Any?>(
                    "id" to draft.original.str("id"), "enabled" to (body["enabled"] ?: true),
                    "displayName" to (body["displayName"] ?: ""), "profileUrl" to (body["profileUrl"] ?: ""),
                )
                body["accessToken"]?.let { entry["accessToken"] = it }
                if (body["clearAccessToken"] == true) entry["clearAccessToken"] = true
                channelsBody(overview, entry)
            }
            MarketingKind.ATTRIBUTION -> linkedMapOf("attribution" to body)
            MarketingKind.TARGETS -> {
                val targets = linkedMapOf<String, Any?>()
                for ((key, value) in body) {
                    val number = (value as? Number)?.toDouble() ?: continue
                    if (number <= 0) continue
                    if (number > 100) throw MarketingFormException("Targets are percentages up to 100.")
                    targets[key] = number
                }
                linkedMapOf("targets" to targets)
            }
            MarketingKind.GOALS -> linkedMapOf("analytics" to body)
            MarketingKind.REDDIT_WATCH -> linkedMapOf("reddit" to body)
            MarketingKind.X_POST -> {
                draft.original["contentId"]?.let { body["contentId"] = it }
                body
            }
            else -> body
        }
    }

    fun channelsBody(overview: MarketingOverview, replacing: Map<String, Any?>? = null, adding: List<String> = emptyList()): Map<String, Any?> {
        val channels = overview.channelSettings.map { it.payload }.toMutableList()
        if (replacing != null) {
            val index = channels.indexOfFirst { it.str("id") == replacing.str("id") }
            if (index >= 0) channels[index] = replacing else channels.add(replacing)
        }
        for (id in adding) {
            if (channels.none { it.str("id") == id }) {
                channels.add(linkedMapOf("id" to id, "enabled" to true, "displayName" to overview.channelName(id), "profileUrl" to ""))
            }
        }
        return linkedMapOf("channels" to channels)
    }

    fun contentDraft(
        overview: MarketingOverview, campaign: String = "", channel: String = "", title: String = "", body: String = "", scheduledAt: Long = 0,
        zone: ZoneId = ZoneId.systemDefault(),
    ): MarketingDraft {
        val json = linkedMapOf<String, Any?>("title" to title, "body" to body, "campaign" to campaign)
        if (channel.isNotEmpty()) json["channel"] = channel
        if (scheduledAt > 0) json["scheduledAt"] = scheduledAt
        return MarketingDraft.of(MarketingKind.CONTENT, overview, json, zone = zone)
    }

    fun templateDraft(template: JsonMap, overview: MarketingOverview, zone: ZoneId = ZoneId.systemDefault()): MarketingDraft =
        MarketingDraft.of(MarketingKind.CAMPAIGN, overview, template - setOf("key", "label"), zone = zone)

    fun metricDraft(overview: MarketingOverview, channel: String, zone: ZoneId = ZoneId.systemDefault()): MarketingDraft {
        val json = linkedMapOf<String, Any?>("channel" to channel)
        overview.stats(channel)?.let { stats ->
            json.putAll(
                listOf(
                    "followers" to stats.followers, "reach" to stats.reach, "impressions" to stats.impressions, "engagements" to stats.engagements,
                    "clicks" to stats.clicks, "views" to stats.views, "posts" to stats.posts, "visits" to stats.visits,
                    "indexedPages" to stats.indexedPages, "outbound" to stats.outbound, "replies" to stats.replies,
                    "closedDeals" to stats.closedDeals, "closedValue" to stats.closedValue, "stars" to stats.stars, "forks" to stats.forks,
                    "downloads" to stats.downloads, "redownloads" to stats.redownloads, "updates" to stats.updates, "ratings" to stats.ratings,
                ),
            )
            if (stats.rating > 0) json["rating"] = stats.rating
        }
        return MarketingDraft.of(MarketingKind.METRIC, overview, json, zone = zone)
    }

    fun channelDraft(overview: MarketingOverview, channel: String, zone: ZoneId = ZoneId.systemDefault()): MarketingDraft {
        val setting = overview.setting(channel)
        val json = linkedMapOf<String, Any?>(
            "id" to channel, "enabled" to (setting?.enabled ?: true), "displayName" to (setting?.displayName ?: overview.channelName(channel)),
            "profileUrl" to setting?.profileUrl.orEmpty(), "hasAccessToken" to (setting?.hasAccessToken ?: false),
        )
        return MarketingDraft.of(MarketingKind.CHANNEL, overview, json, zone = zone)
    }

    fun settingsDraft(kind: MarketingKind, overview: MarketingOverview, zone: ZoneId = ZoneId.systemDefault()): MarketingDraft = when (kind) {
        MarketingKind.ATTRIBUTION -> MarketingDraft.of(kind, overview, overview.settings.obj("attribution"), zone = zone)
        MarketingKind.TARGETS -> MarketingDraft.of(kind, overview, overview.settings.obj("targets"), zone = zone)
        MarketingKind.GOALS -> MarketingDraft.of(kind, overview, overview.settings.obj("analytics"), zone = zone)
        MarketingKind.REDDIT_WATCH -> MarketingDraft.of(
            kind, overview,
            mapOf("subreddits" to overview.redditSubreddits.map { it.name }, "queries" to overview.redditQueries.map { it.name }), zone = zone,
        )
        else -> MarketingDraft.of(kind, overview, zone = zone)
    }

    fun xPostDraft(overview: MarketingOverview, content: MarketingContent? = null): MarketingDraft {
        val json = linkedMapOf<String, Any?>()
        if (content != null) {
            json["text"] = content.body.ifEmpty { content.title }
            json["contentId"] = content.id
        }
        return MarketingDraft.of(MarketingKind.X_POST, overview, json)
    }

    fun ideaDraft(idea: MarketingIdea, overview: MarketingOverview, campaign: MarketingRecord?, now: Long, zone: ZoneId = ZoneId.systemDefault()): MarketingDraft? =
        when (idea.kind) {
            "content" -> contentDraft(
                overview, campaign?.title.orEmpty(), idea.channel, idea.title, idea.copy.ifEmpty { idea.detail },
                if (idea.inDays > 0) now + idea.inDays * 86_400_000L else 0, zone,
            )
            "keyword" -> MarketingDraft.of(
                MarketingKind.KEYWORD, overview,
                mapOf("term" to idea.title.lowercase(), "notes" to idea.detail, "campaign" to campaign?.title.orEmpty()), zone = zone,
            )
            "campaign" -> MarketingDraft.of(MarketingKind.CAMPAIGN, overview, mapOf("name" to idea.title, "brief" to idea.detail, "status" to "idea"), zone = zone)
            else -> null
        }

    fun suggestionDraft(suggestion: MarketingSuggestion, overview: MarketingOverview, zone: ZoneId = ZoneId.systemDefault()): MarketingDraft? {
        val action = suggestion.action
        return when (suggestion.actionType) {
            "campaign" -> MarketingDraft.of(MarketingKind.CAMPAIGN, overview, zone = zone)
            "campaign-edit" -> overview.campaign(action.str("id"))?.let { MarketingDraft.of(MarketingKind.CAMPAIGN, overview, it.json, it.id, zone) }
            "content" -> contentDraft(overview, action.str("campaign"), action.str("channel"), zone = zone)
            "keyword" -> MarketingDraft.of(MarketingKind.KEYWORD, overview, zone = zone)
            "channel" -> metricDraft(overview, action.str("channel"), zone)
            else -> null
        }
    }

    fun taskToggleBody(campaign: MarketingRecord, index: Int, done: Boolean): Map<String, Any?> {
        val tasks = campaign.tasks.toMutableList()
        if (index in tasks.indices) tasks[index] = tasks[index].copy(done = done)
        return linkedMapOf("tasks" to tasks.map { it.json })
    }

    fun addTaskBody(campaign: MarketingRecord, title: String): Map<String, Any?> =
        linkedMapOf("tasks" to (campaign.tasks + MarketingTask(title.take(200))).take(40).map { it.json })
}

object MarketingDates {
    fun text(ms: Long, time: Boolean, zone: ZoneId): String {
        val moment = LocalDateTime.ofInstant(Instant.ofEpochMilli(ms), zone)
        val day = moment.toLocalDate().toString()
        if (!time || (moment.hour == 0 && moment.minute == 0)) return day
        return day + " %02d:%02d".format(moment.hour, moment.minute)
    }

    fun parse(raw: String, zone: ZoneId): Long? {
        val pieces = raw.trim().split(' ', 'T').filter { it.isNotEmpty() }
        if (pieces.isEmpty() || pieces.size > 2) return null
        val day = pieces[0].split('-').mapNotNull { it.toIntOrNull() }
        if (day.size != 3 || day[0] < 1970) return null
        val date = runCatching { LocalDate.of(day[0], day[1], day[2]) }.getOrNull() ?: return null
        var clock = LocalTime.MIDNIGHT
        if (pieces.size == 2) {
            val parts = pieces[1].split(':').mapNotNull { it.toIntOrNull() }
            if (parts.size != 2) return null
            clock = runCatching { LocalTime.of(parts[0], parts[1]) }.getOrNull() ?: return null
        }
        return date.atTime(clock).atZone(zone).toInstant().toEpochMilli()
    }
}

object MarketingMoney {
    fun cents(raw: String): Long? {
        val clean = raw.replace(" ", "").replace("$", "")
        val normalized = if ('.' in clean) clean.replace(",", "") else clean.replace(',', '.')
        val parts = normalized.split('.')
        if (parts.size > 2) return null
        val whole = (parts[0].ifEmpty { "0" }).toLongOrNull()?.takeIf { it >= 0 } ?: return null
        var fraction = 0L
        if (parts.size == 2) {
            val digits = parts[1]
            if (digits.length > 2 || !digits.all { it.isDigit() }) return null
            fraction = when (digits.length) {
                0 -> 0
                1 -> digits.toLong() * 10
                else -> digits.toLong()
            }
        }
        return whole * 100 + fraction
    }
}
