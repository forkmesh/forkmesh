package com.forkmesh.nativeapp.market

import com.forkmesh.nativeapp.json.JsonMap
import com.forkmesh.nativeapp.json.bool
import com.forkmesh.nativeapp.json.double
import com.forkmesh.nativeapp.json.int
import com.forkmesh.nativeapp.json.jsonLong
import com.forkmesh.nativeapp.json.jsonText
import com.forkmesh.nativeapp.json.list
import com.forkmesh.nativeapp.json.long
import com.forkmesh.nativeapp.json.maps
import com.forkmesh.nativeapp.json.obj
import com.forkmesh.nativeapp.json.str
import com.forkmesh.nativeapp.json.strings
import kotlin.math.abs

data class PackageOrganization(val name: String, val role: String = "member", val installed: Boolean = false) {
    val canManage: Boolean get() = role == "owner" || role == "admin"

    companion object {
        fun fromJson(json: JsonMap) = PackageOrganization(json.str("name"), json.str("role", "member"), json.bool("installed"))

        fun list(json: JsonMap): List<PackageOrganization> =
            json.maps("organizations").map(::fromJson).filter { PackagesText.isOrgName(it.name) }
    }
}

data class CrmFlow(val id: String, val name: String, val icon: String = "pipeline", val stages: List<String>) {
    companion object {
        val FALLBACK = CrmFlow("customers", "Customers", stages = listOf("lead", "qualified", "proposal", "customer"))

        fun fromJson(json: JsonMap): CrmFlow {
            val stages = json.strings("stages").filter { it.isNotEmpty() }
            return CrmFlow(json.str("id"), json.str("name", json.str("id")), json.str("icon", "pipeline"), stages.ifEmpty { listOf("lead") })
        }
    }
}

data class CrmContact(
    val id: String = "",
    val organization: String = "",
    val name: String = "",
    val email: String = "",
    val company: String = "",
    val title: String = "",
    val phone: String = "",
    val website: String = "",
    val location: String = "",
    val linkedin: String = "",
    val github: String = "",
    val twitter: String = "",
    val mastodon: String = "",
    val bluesky: String = "",
    val flow: String = "customers",
    val stage: String = "lead",
    val value: Long = 0,
    val nextStep: String = "",
    val tags: List<String> = emptyList(),
    val owner: String = "",
    val source: String = "",
    val notes: String = "",
    val lastContactedAt: Long = 0,
    val fields: Map<String, String> = emptyMap(),
    val views: List<String> = emptyList(),
    val createdBy: String = "",
    val updatedBy: String = "",
    val createdAt: Long = 0,
    val updatedAt: Long = 0,
) {
    val displayName: String get() = name.ifEmpty { email.ifEmpty { "Unnamed contact" } }

    val subtitle: String get() = listOf(title, company).filter { it.isNotEmpty() }.joinToString(" · ")

    val routeId: String get() = CrmContactRef(organization, id).routeId

    val initials: String
        get() = displayName.split(' ', '.', '@').filter { it.isNotEmpty() }.take(2).joinToString("") { it.first().uppercase() }.ifEmpty { "?" }

    fun valueOf(field: String): String = when (field) {
        "name" -> name
        "email" -> email
        "company" -> company
        "title" -> title
        "phone" -> phone
        "twitter" -> twitter
        "mastodon" -> mastodon
        "bluesky" -> bluesky
        "linkedin" -> linkedin
        "github" -> github
        "website" -> website
        "location" -> location
        "flow" -> flow
        "stage" -> CrmStageLabel.title(stage)
        "value" -> if (value > 0) value.toString() else ""
        "nextStep" -> nextStep
        "owner" -> owner
        "source" -> source
        "tags" -> tags.joinToString(", ")
        "notes" -> notes
        "lastContactedAt" -> if (lastContactedAt > 0) lastContactedAt.toString() else ""
        else -> fields[field].orEmpty()
    }

    fun matches(query: String): Boolean = PackagesText.matches(
        query, name, email, company, title, tags.joinToString(" "), nextStep, owner, location, phone, fields.values.joinToString(" "),
    )

    companion object {
        fun fromJson(json: JsonMap) = CrmContact(
            id = json.str("id"), organization = json.str("organization"), name = json.str("name"), email = json.str("email"),
            company = json.str("company"), title = json.str("title"), phone = json.str("phone"), website = json.str("website"),
            location = json.str("location"), linkedin = json.str("linkedin"), github = json.str("github"), twitter = json.str("twitter"),
            mastodon = json.str("mastodon"), bluesky = json.str("bluesky"), flow = json.str("flow", "customers"),
            stage = json.str("stage", "lead"), value = maxOf(0, json.long("value")), nextStep = json.str("nextStep"),
            tags = json.strings("tags"), owner = json.str("owner"), source = json.str("source"), notes = json.str("notes"),
            lastContactedAt = json.long("lastContactedAt"),
            fields = json.obj("fields").mapValues { jsonText(it.value) }.filterValues { it.isNotEmpty() },
            views = json.strings("views").filter(PackagesText::isHexId),
            createdBy = json.str("createdBy"), updatedBy = json.str("updatedBy"),
            createdAt = json.long("createdAt"), updatedAt = json.long("updatedAt"),
        )
    }
}

data class CrmContactEdit(
    val name: String = "",
    val email: String = "",
    val company: String = "",
    val title: String = "",
    val phone: String = "",
    val flow: String = CrmFlow.FALLBACK.id,
    val stage: String = "lead",
    val value: Long = 0,
    val nextStep: String = "",
    val tags: String = "",
    val notes: String = "",
    val website: String = "",
    val location: String = "",
    val linkedin: String = "",
    val github: String = "",
    val twitter: String = "",
    val mastodon: String = "",
    val bluesky: String = "",
    val owner: String = "",
    val source: String = "",
    val fields: Map<String, String> = emptyMap(),
    val lastContactedAt: Long = 0,
) {
    val tagList: List<String>
        get() {
            val seen = HashSet<String>()
            return tags.split(',').map { it.trim() }.filter { it.isNotEmpty() && seen.add(it.lowercase()) }
        }

    val isValid: Boolean get() = name.trim().isNotEmpty() || email.trim().isNotEmpty()

    fun text(key: String): String = when (key) {
        "name" -> name
        "email" -> email
        "company" -> company
        "title" -> title
        "phone" -> phone
        "website" -> website
        "location" -> location
        "linkedin" -> linkedin
        "github" -> github
        "twitter" -> twitter
        "mastodon" -> mastodon
        "bluesky" -> bluesky
        "owner" -> owner
        "source" -> source
        "flow" -> flow
        "stage" -> stage
        "nextStep" -> nextStep
        "notes" -> notes
        else -> ""
    }

    fun changes(original: CrmContact?): Map<String, Any?> {
        val body = LinkedHashMap<String, Any?>()
        val before = original?.let(::of)
        for (key in TEXT_KEYS) {
            val trimmed = text(key).trim()
            if (before == null || trimmed != before.text(key)) body[key] = trimmed
        }
        if (original == null || value != original.value) body["value"] = maxOf(0, value)
        if (lastContactedAt != (original?.lastContactedAt ?: 0L)) body["lastContactedAt"] = maxOf(0, lastContactedAt)
        if (original == null || tagList != original.tags) body["tags"] = tagList
        val old = original?.fields.orEmpty()
        val custom = LinkedHashMap<String, Any?>()
        for (key in (fields.keys + old.keys).toSortedSet()) {
            val next = fields[key].orEmpty().trim()
            if (next != old[key].orEmpty()) custom[key] = next
        }
        if (custom.isNotEmpty()) body["fields"] = custom
        return body
    }

    companion object {
        val TEXT_KEYS = listOf(
            "name", "email", "company", "title", "phone", "website", "location", "linkedin", "github", "twitter",
            "mastodon", "bluesky", "owner", "source", "flow", "stage", "nextStep", "notes",
        )

        fun of(contact: CrmContact) = CrmContactEdit(
            contact.name, contact.email, contact.company, contact.title, contact.phone, contact.flow, contact.stage, contact.value,
            contact.nextStep, contact.tags.joinToString(", "), contact.notes, contact.website, contact.location, contact.linkedin,
            contact.github, contact.twitter, contact.mastodon, contact.bluesky, contact.owner, contact.source, contact.fields,
            contact.lastContactedAt,
        )
    }
}

data class CrmActivity(
    val id: String,
    val contactId: String,
    val sender: String,
    val transport: String,
    val status: String,
    val to: String,
    val subject: String,
    val provider: String,
    val direction: String,
    val error: String,
    val createdAt: Long,
    val sentAt: Long,
    val externalId: String = "",
    val fromEmail: String = "",
    val fromName: String = "",
    val providerUrl: String = "",
    val text: String = "",
    val html: String = "",
) {
    val at: Long get() = if (sentAt > 0) sentAt else createdAt
    val failed: Boolean get() = status in listOf("failed", "reject", "bounce", "soft bounce", "suspension")
    val incoming: Boolean get() = direction == "inbound" || direction == "received"
    val engaged: Boolean get() = status == "open" || status == "click"
    val delivered: Boolean get() = status in listOf("accepted", "delivery", "delivered", "open", "click")
    val plainBody: String get() = text.ifEmpty { CrmText.strippingTags(html) }

    companion object {
        val STATUSES = listOf(
            "draft", "external", "sending", "accepted", "delivery", "delivered", "open", "click", "failed", "reject",
            "bounce", "soft bounce", "suspension", "unsubscribe", "spam",
        )

        fun fromJson(json: JsonMap) = CrmActivity(
            json.str("id"), json.str("contactId"), json.str("sender"), json.str("transport", "external"), json.str("status", "external"),
            json.str("to"), json.str("subject"), json.str("provider"), json.str("direction"), json.str("error"),
            json.long("createdAt"), json.long("sentAt"), json.str("externalId"), json.str("fromEmail"), json.str("fromName"),
            json.str("providerUrl"), json.str("text"), json.str("html"),
        )
    }
}

data class CrmSummaryCounts(val contacts: Int, val customers: Int, val sent: Int, val failed: Int, val mail: Int)

data class CrmStageColumn(val stage: String, val contacts: List<CrmContact>)

data class CrmOrganizationData(
    val organization: String,
    val role: String,
    val canManageSettings: Boolean,
    val contacts: List<CrmContact>,
    val activity: List<CrmActivity>,
    val flows: List<CrmFlow>,
    val members: List<String>,
    val counts: CrmSummaryCounts,
    val revision: String,
    val mail: List<CrmMailMessage> = emptyList(),
    val settings: CrmSettings = CrmSettings.fromJson(emptyMap()),
    val outreach: CrmOutreach = CrmOutreach.fromJson(emptyMap()),
    val views: List<CrmView> = emptyList(),
    val sharedViews: List<CrmView> = emptyList(),
    val team: CrmTeamActivity = CrmTeamActivity.fromJson(emptyMap()),
    val decks: List<CrmDeckLink> = emptyList(),
) {
    fun flow(id: String?): CrmFlow = flows.firstOrNull { it.id == id } ?: flows.firstOrNull() ?: CrmFlow.FALLBACK

    fun contactsIn(flow: CrmFlow, query: String = ""): List<CrmContact> = contacts.filter { it.flow == flow.id && it.matches(query) }

    fun stageColumns(flow: CrmFlow, query: String = ""): List<CrmStageColumn> {
        val rows = contactsIn(flow, query)
        val columns = flow.stages.map { stage -> CrmStageColumn(stage, rows.filter { it.stage == stage }) }.toMutableList()
        val stray = rows.filter { it.stage !in flow.stages }
        if (stray.isNotEmpty()) columns.add(CrmStageColumn("other", stray))
        return columns
    }

    fun activityFor(contactId: String): List<CrmActivity> = activity.filter { it.contactId == contactId }

    fun stageValue(flow: CrmFlow, stage: String): Long = contacts.filter { it.flow == flow.id && it.stage == stage }.sumOf { it.value }

    fun replacing(contact: CrmContact): CrmOrganizationData {
        val index = contacts.indexOfFirst { it.id == contact.id }
        return copy(contacts = if (index >= 0) contacts.toMutableList().also { it[index] = contact } else listOf(contact) + contacts)
    }

    fun removing(contactId: String): CrmOrganizationData =
        copy(contacts = contacts.filter { it.id != contactId }, activity = activity.filter { it.contactId != contactId })

    companion object {
        fun fromJson(json: JsonMap): CrmOrganizationData {
            val contacts = json.maps("contacts").map(CrmContact::fromJson).filter { PackagesText.isHexId(it.id) }
            val flows = json.obj("settings").maps("flows").map(CrmFlow::fromJson).filter { it.id.isNotEmpty() }
            val summary = json.obj("summary")
            return CrmOrganizationData(
                organization = json.str("organization"),
                role = json.str("role", "member"),
                canManageSettings = json.bool("canManageSettings"),
                contacts = contacts,
                activity = json.maps("emails").map(CrmActivity::fromJson).sortedByDescending { it.at },
                flows = flows.ifEmpty { listOf(CrmFlow.FALLBACK) },
                members = json.list("members").map { if (it is Map<*, *>) it.asMap().str("name") else jsonText(it) }.filter { it.isNotEmpty() },
                counts = CrmSummaryCounts(
                    summary.int("contacts", contacts.size), summary.int("customers"), summary.int("sent"), summary.int("failed"), summary.int("mail"),
                ),
                revision = json.str("revision"),
                mail = json.maps("mail").map(CrmMailMessage::fromJson).filter { PackagesText.isHexId(it.id) },
                settings = CrmSettings.fromJson(json.obj("settings")),
                outreach = CrmOutreach.fromJson(json.obj("outreach")),
                views = json.maps("views").map(CrmView::fromJson).filter { PackagesText.isHexId(it.id) },
                sharedViews = json.maps("sharedViews").map(CrmView::fromJson).filter { PackagesText.isHexId(it.id) },
                team = CrmTeamActivity.fromJson(json.obj("activity")),
                decks = json.maps("decks").map(CrmDeckLink::fromJson).filter { it.token.isNotEmpty() },
            )
        }
    }
}

data class CrmContactRef(val organization: String, val contactId: String) {
    val routeId: String get() = "$organization/$contactId"

    companion object {
        fun of(routeId: String): CrmContactRef? {
            val parts = routeId.split('/')
            if (parts.size != 2 || !PackagesText.isOrgName(parts[0]) || !PackagesText.isHexId(parts[1])) return null
            return CrmContactRef(parts[0], parts[1])
        }
    }
}

object CrmStageLabel {
    fun title(stage: String): String = stage.split('-').joinToString(" ") { part -> part.replaceFirstChar { it.uppercase() } }
}

data class MarketingChannel(val id: String, val name: String, val category: String = "")

data class MarketingContent(
    val id: String,
    val channel: String = "blog",
    val status: String = "draft",
    val title: String = "",
    val body: String = "",
    val campaign: String = "",
    val owner: String = "",
    val url: String = "",
    val tags: List<String> = emptyList(),
    val updatedAt: Long = 0,
    val scheduledAt: Long = 0,
    val publishedAt: Long = 0,
    val json: JsonMap = emptyMap(),
) {
    val nextStatuses: List<String> get() = STATUSES.filter { it != status }

    companion object {
        val STATUSES = listOf("draft", "review", "queued", "scheduled", "published", "archived")

        fun fromJson(json: JsonMap) = MarketingContent(
            json.str("id"), json.str("channel", "blog"), json.str("status", "draft"), json.str("title"), json.str("body"),
            json.str("campaign"), json.str("owner"), json.str("url"), json.strings("tags"), json.long("updatedAt"),
            json.long("scheduledAt"), json.long("publishedAt"), json,
        )
    }
}

data class MarketingCampaignAd(
    val id: String,
    val channel: String = "",
    val status: String = "draft",
    val name: String = "",
    val campaign: String = "",
    val headline: String = "",
    val platform: String = "",
    val currency: String = "",
    val budgetCents: Long = 0,
    val spendCents: Long = 0,
    val impressions: Long = 0,
    val clicks: Long = 0,
    val conversions: Long = 0,
    val pacing: String = "",
    val startsAt: Long = 0,
    val endsAt: Long = 0,
    val managerUrl: String = "",
    val platformUrl: String = "",
    val linkChecks: List<String> = emptyList(),
    val json: JsonMap = emptyMap(),
) {
    val clickRate: Double get() = if (impressions > 0) clicks.toDouble() / impressions else 0.0
    val spendLabel: String get() = MoneyText.cents(spendCents, currency) + " of " + MoneyText.cents(budgetCents, currency)
    val consoleUrl: String get() = managerUrl.ifEmpty { platformUrl }
    val needsAttention: Boolean get() = pacing in setOf("over", "under", "overspent")

    companion object {
        val STATUSES = listOf("draft", "review", "scheduled", "active", "paused", "completed", "archived")

        fun fromJson(json: JsonMap) = MarketingCampaignAd(
            json.str("id"), json.str("channel"), json.str("status", "draft"), json.str("name"), json.str("campaign"),
            json.str("headline"), json.str("platform"), json.str("currency"), json.long("budgetCents"), json.long("spendCents"),
            json.long("impressions"), json.long("clicks"), json.long("conversions"), json.obj("pacing").str("state"),
            json.long("startsAt"), json.long("endsAt"), json.str("managerUrl"), json.str("platformUrl"),
            json.maps("linkChecks").map { it.str("message") }.filter { it.isNotEmpty() }, json,
        )
    }
}

data class MarketingHealthCheck(val id: String, val label: String, val ok: Boolean)

data class MarketingChannelStats(
    val id: String,
    val reach: Long,
    val followers: Long,
    val impressions: Long,
    val engagements: Long,
    val clicks: Long,
    val posts: Long = 0,
    val views: Long = 0,
    val reviews: Long = 0,
    val visits: Long = 0,
    val indexedPages: Long = 0,
    val outbound: Long = 0,
    val replies: Long = 0,
    val closedDeals: Long = 0,
    val closedValue: Long = 0,
    val stars: Long = 0,
    val forks: Long = 0,
    val rating: Double = 0.0,
    val health: String = "",
    val checks: List<MarketingHealthCheck> = emptyList(),
    val observedAt: Long = 0,
    val source: String = "",
    val followersChange: Long? = null,
    val starsChange: Long? = null,
    val engagementRate: Double? = null,
    val postsLast30d: Long = 0,
    val lastPostAt: Long = 0,
    val profileUrl: String = "",
    val downloads: Long = 0,
    val redownloads: Long = 0,
    val updates: Long = 0,
    val ratings: Long = 0,
) {
    val healthTitle: String get() = HEALTH_TITLES[health].orEmpty()
    val canCapture: Boolean get() = profileUrl.startsWith("https://") && !isEmail
    val isGoogle: Boolean get() = id == "google-search"
    val isEmail: Boolean get() = id == "email"
    val isGitHub: Boolean get() = id == "github"
    val isAppStore: Boolean get() = id == "app-store"
    val missingChecks: List<String> get() = checks.filter { !it.ok }.map { it.label }

    fun captureUrl(organization: String): String =
        "https://app.forkmesh.com/dashboard/marketing?org=" + java.net.URLEncoder.encode(organization, "UTF-8") +
            "&page=channels&capture=" + java.net.URLEncoder.encode(id, "UTF-8")

    companion object {
        val HEALTH_TITLES = mapOf(
            "good" to "On track", "attention" to "Needs attention", "stale" to "Stale numbers",
            "setup" to "Not set up", "paused" to "Paused",
        )

        fun fromJson(json: JsonMap, settings: JsonMap): MarketingChannelStats {
            val health = json.obj("health")
            val id = json.str("channel")
            return MarketingChannelStats(
                id, json.long("reach"), json.long("followers"), json.long("impressions"), json.long("engagements"), json.long("clicks"),
                posts = json.long("posts"), views = json.long("views"), reviews = json.long("reviews"), rating = json.double("rating"),
                visits = json.long("visits"), indexedPages = json.long("indexedPages"),
                outbound = json.long("outbound"), replies = json.long("replies"),
                closedDeals = json.long("closedDeals"), closedValue = json.long("closedValue"),
                stars = json.long("stars"), forks = json.long("forks"),
                health = health.str("state"),
                checks = health.maps("checks").map { MarketingHealthCheck(it.str("id"), it.str("label"), it.bool("ok")) },
                observedAt = health.long("observedAt"), source = health.str("source"),
                followersChange = (health["followersChange"] as? Number)?.toLong(),
                starsChange = (health["starsChange"] as? Number)?.toLong(),
                engagementRate = (health["engagementRate"] as? Number)?.toDouble(),
                postsLast30d = health.long("postsLast30d"), lastPostAt = health.long("lastPostAt"),
                profileUrl = settings.maps("channels").firstOrNull { it.str("id") == id }?.str("profileUrl").orEmpty(),
                downloads = json.long("downloads"), redownloads = json.long("redownloads"), updates = json.long("updates"),
                ratings = json.long("ratings"),
            )
        }
    }
}

data class MarketingPlanGroup(val status: String, val items: List<MarketingContent>)

data class MarketingOverview(
    val organization: String,
    val role: String,
    val content: List<MarketingContent>,
    val ads: List<MarketingCampaignAd>,
    val catalog: List<MarketingChannel>,
    val channels: List<MarketingChannelStats>,
    val published: Int,
    val queued: Int,
    val reach: Long = 0,
    val followers: Long = 0,
    val engagements: Long = 0,
    val healthCounts: Map<String, Int> = emptyMap(),
    val canManageSettings: Boolean = false,
    val assistantAvailable: Boolean = false,
    val settings: JsonMap = emptyMap(),
    val channelSettings: List<MarketingChannelSetting> = emptyList(),
    val refreshable: Set<String> = emptySet(),
    val playbooks: JsonMap = emptyMap(),
    val campaigns: List<MarketingRecord> = emptyList(),
    val campaignTemplates: List<JsonMap> = emptyList(),
    val experiments: List<MarketingRecord> = emptyList(),
    val experimentsRunning: Int = 0,
    val experimentsRunningLimit: Int = 3,
    val funnel: List<MarketingRecord> = emptyList(),
    val funnelReport: MarketingFunnelReport = MarketingFunnelReport.fromJson(emptyMap()),
    val keywords: List<MarketingRecord> = emptyList(),
    val redditThreads: List<MarketingRecord> = emptyList(),
    val redditSubreddits: List<MarketingRedditSubreddit> = emptyList(),
    val redditQueries: List<MarketingNamedLink> = emptyList(),
    val redditSummary: JsonMap = emptyMap(),
    val posts: List<MarketingRecord> = emptyList(),
    val adTotals: List<MarketingAdTotals> = emptyList(),
    val adsNeedingAttention: Int = 0,
    val adPlatforms: List<MarketingChannel> = emptyList(),
    val suggestions: List<MarketingSuggestion> = emptyList(),
    val x: MarketingXStatus = MarketingXStatus.fromJson(emptyMap()),
    val readyX: MarketingReadyPosts? = null,
) {
    fun channelName(id: String): String =
        channelSettings.firstOrNull { it.id == id }?.displayName?.ifEmpty { null } ?: catalog.firstOrNull { it.id == id }?.name ?: id

    fun contentWith(status: String?): List<MarketingContent> = if (status.isNullOrEmpty()) content else content.filter { it.status == status }

    fun content(query: String, status: String?, channel: String?): List<MarketingContent> = content
        .filter { status.isNullOrEmpty() || it.status == status }
        .filter { channel.isNullOrEmpty() || it.channel == channel }
        .filter { PackagesText.matches(query, it.title, it.campaign, it.owner, it.body, it.tags.joinToString(" ")) }

    val plan: List<MarketingPlanGroup>
        get() = MarketingContent.STATUSES.filter { it != "archived" }.map { status -> MarketingPlanGroup(status, content.filter { it.status == status }) }
            .filter { it.items.isNotEmpty() }

    val activeAds: List<MarketingCampaignAd> get() = ads.filter { it.status == "active" || it.status == "scheduled" || it.status == "paused" }

    fun ads(status: String?, query: String): List<MarketingCampaignAd> = ads
        .filter { status.isNullOrEmpty() || it.status == status }
        .filter { PackagesText.matches(query, it.name, it.headline, it.campaign, it.platform) }

    val campaignNames: List<String> get() = campaigns.filter { it.status !in setOf("completed", "archived") }.map { it.title }.filter { it.isNotEmpty() }

    fun campaign(id: String): MarketingRecord? = campaigns.firstOrNull { it.id == id }

    fun setting(id: String): MarketingChannelSetting? = channelSettings.firstOrNull { it.id == id }

    fun stats(id: String): MarketingChannelStats? = channels.firstOrNull { it.id == id }

    fun category(id: String): String = catalog.firstOrNull { it.id == id }?.category.orEmpty()

    fun playbook(id: String): Triple<String, List<String>, List<String>>? {
        val entry = playbooks.obj(category(id))
        if (entry.isEmpty()) return null
        return Triple(entry.str("goal"), entry.strings("kpis"), entry.strings("tips"))
    }

    val addableChannels: List<MarketingChannel>
        get() {
            val configured = channelSettings.map { it.id }.toSet()
            return catalog.filter { it.id !in configured }
        }

    val staleChannels: List<String>
        get() = channels.filter { it.health in setOf("stale", "attention") && it.id in refreshable }
            .filter { setting(it.id)?.enabled ?: true }.map { it.id }

    fun posts(channel: String): List<MarketingRecord> = posts.filter { it.json.str("channel") == channel }

    val openRedditThreads: List<MarketingRecord> get() = redditThreads.filter { it.needsAttention }

    val redditQueriesText: String get() = redditQueries.joinToString("\n") { it.name }

    fun replacing(item: MarketingContent): MarketingOverview = copy(content = content.map { if (it.id == item.id) item else it })

    fun replacing(ad: MarketingCampaignAd): MarketingOverview = copy(ads = ads.map { if (it.id == ad.id) ad else it })

    companion object {
        fun fromJson(json: JsonMap): MarketingOverview {
            val summary = json.obj("summary")
            val settings = json.obj("settings")
            fun records(key: String, kind: MarketingKind) = json.maps(key).map { MarketingRecord(kind, it) }.filter { PackagesText.isHexId(it.id) }
            val experimentsSummary = json.obj("experimentsSummary")
            val desk = json.obj("redditDesk")
            val adsSummary = json.obj("adsSummary")
            return MarketingOverview(
                organization = json.str("organization"),
                role = json.str("role", "member"),
                content = json.maps("content").map(MarketingContent::fromJson).filter { PackagesText.isHexId(it.id) },
                ads = json.maps("ads").map(MarketingCampaignAd::fromJson).filter { PackagesText.isHexId(it.id) },
                catalog = json.maps("channelCatalog").map { MarketingChannel(it.str("id"), it.str("name"), it.str("category")) },
                channels = summary.maps("channels").map { MarketingChannelStats.fromJson(it, settings) },
                published = summary.int("published"),
                queued = summary.int("queued"),
                reach = summary.long("reach"),
                followers = summary.long("followers"),
                engagements = summary.long("engagements"),
                healthCounts = summary.obj("health").mapValues { (_, value) -> jsonLong(value).toInt() },
                canManageSettings = json.bool("canManageSettings"),
                assistantAvailable = json.bool("assistantAvailable"),
                settings = settings,
                channelSettings = settings.maps("channels").map(MarketingChannelSetting::fromJson).filter { it.id.isNotEmpty() },
                refreshable = json.strings("refreshableChannels").toSet(),
                playbooks = json.obj("channelPlaybooks"),
                campaigns = records("campaigns", MarketingKind.CAMPAIGN),
                campaignTemplates = json.maps("campaignTemplates"),
                experiments = records("experiments", MarketingKind.EXPERIMENT),
                experimentsRunning = experimentsSummary.int("running"),
                experimentsRunningLimit = experimentsSummary.int("runningLimit", 3),
                funnel = records("funnel", MarketingKind.FUNNEL_WEEK),
                funnelReport = MarketingFunnelReport.fromJson(json.obj("funnelReport")),
                keywords = records("keywords", MarketingKind.KEYWORD),
                redditThreads = records("redditThreads", MarketingKind.REDDIT_THREAD),
                redditSubreddits = desk.maps("subreddits").map(MarketingRedditSubreddit::fromJson),
                redditQueries = desk.maps("queries").map { MarketingNamedLink(it.str("query"), it.str("query"), it.str("url")) },
                redditSummary = desk.obj("summary"),
                posts = records("posts", MarketingKind.POST),
                adTotals = adsSummary.maps("currencies").map(MarketingAdTotals::fromJson),
                adsNeedingAttention = adsSummary.int("attention"),
                adPlatforms = json.maps("adPlatforms").map { MarketingChannel(it.str("channel"), it.str("name")) },
                suggestions = json.maps("suggestions").map(MarketingSuggestion::fromJson),
                x = MarketingXStatus.fromJson(settings.obj("x")),
                readyX = json.obj("readyPosts").obj("x").takeIf { it.isNotEmpty() }?.let { MarketingReadyPosts.fromJson(it, "x") },
            )
        }
    }
}

object MoneyText {
    fun cents(cents: Long, currency: String): String {
        val whole = cents / 100
        val part = abs(cents % 100)
        val amount = "$whole.${if (part < 10) "0" else ""}$part"
        return if (currency.isEmpty()) amount else "$amount $currency"
    }
}

data class AnalyticsProject(val id: String, val name: String, val siteDefault: Boolean, val eventsToday: Long) {
    companion object {
        fun fromJson(json: JsonMap): AnalyticsProject {
            val settings = json.obj("settings")
            val siteDefault = json.bool("siteDefault")
            return AnalyticsProject(
                json.str("id"), settings.str("name").ifEmpty { if (siteDefault) "Website" else "Project" }, siteDefault, json.obj("usage").long("events"),
            )
        }
    }
}

data class AnalyticsTotals(
    val visitors: Long = 0,
    val pageviews: Long = 0,
    val sessions: Long = 0,
    val bounceRate: Double = 0.0,
    val averageSessionMs: Long = 0,
    val events: Long = 0,
) {
    companion object {
        fun fromJson(json: JsonMap) = AnalyticsTotals(
            json.long("visitors"), json.long("pageviews"), json.long("sessions"), json.double("bounceRate"), json.long("averageSessionMs"), json.long("events"),
        )

        fun change(current: Long, previous: Long): Double? = if (previous > 0) (current - previous).toDouble() / previous else null
    }
}

data class AnalyticsSeries(val name: String, val data: List<Double>, val total: Double)

data class AnalyticsTopRow(val value: String, val count: Long)

enum class AnalyticsRange(val wire: String, val millis: Long) {
    DAY("24h", 86_400_000), WEEK("7d", 7 * 86_400_000L), MONTH("30d", 30 * 86_400_000L);

    fun query(nowMs: Long): Map<String, Any?> = linkedMapOf("kind" to "overview", "from" to nowMs - millis, "to" to nowMs)
}

data class AnalyticsOverview(
    val totals: AnalyticsTotals,
    val previous: AnalyticsTotals,
    val labels: List<Long>,
    val series: List<AnalyticsSeries>,
    val pages: List<AnalyticsTopRow>,
    val referrers: List<AnalyticsTopRow>,
    val countries: List<AnalyticsTopRow>,
    val devices: List<AnalyticsTopRow>,
    val events: List<AnalyticsTopRow>,
    val truncated: Boolean,
) {
    companion object {
        fun fromJson(json: JsonMap): AnalyticsOverview {
            val result = json.obj("result")
            val timeseries = result.obj("timeseries")
            val breakdowns = result.obj("breakdowns")
            fun top(key: String) = breakdowns.maps(key).take(12).map { AnalyticsTopRow(it.str("value"), it.long("count")) }
            return AnalyticsOverview(
                AnalyticsTotals.fromJson(result.obj("totals")),
                AnalyticsTotals.fromJson(result.obj("previous")),
                timeseries.list("labels").map { jsonLong(it) },
                timeseries.maps("series").take(10).map { series ->
                    AnalyticsSeries(series.str("name"), series.list("data").map { (it as? Number)?.toDouble() ?: 0.0 }, series.double("total"))
                },
                top("pages"), top("referrers"), top("countries"), top("devices"), top("events"),
                json.bool("truncated"),
            )
        }
    }
}

data class Competitor(
    val id: String = "",
    val status: String = "watch",
    val name: String = "",
    val website: String = "",
    val category: String = "",
    val positioning: String = "",
    val strengths: String = "",
    val weaknesses: String = "",
    val notes: String = "",
    val updatedAt: Long = 0,
    val lastReviewedAt: Long = 0,
) {
    fun needsReview(nowMs: Long, days: Long = 90): Boolean = status != "archived" && (lastReviewedAt == 0L || nowMs - lastReviewedAt > days * 86_400_000)

    val body: Map<String, Any?>
        get() = linkedMapOf(
            "name" to name.trim(),
            "status" to if (status in STATUSES) status else "watch",
            "website" to website.trim(),
            "category" to category, "positioning" to positioning, "strengths" to strengths, "weaknesses" to weaknesses, "notes" to notes,
        )

    val websiteIsValid: Boolean
        get() {
            val trimmed = website.trim()
            return trimmed.isEmpty() || (trimmed.lowercase().startsWith("https://") && !trimmed.contains(' '))
        }

    fun matches(query: String): Boolean = PackagesText.matches(query, name, category, positioning, website)

    companion object {
        val STATUSES = listOf("primary", "watch", "archived")

        fun fromJson(json: JsonMap) = Competitor(
            json.str("id"), json.str("status", "watch"), json.str("name"), json.str("website"), json.str("category"),
            json.str("positioning"), json.str("strengths"), json.str("weaknesses"), json.str("notes"), json.long("updatedAt"), json.long("lastReviewedAt"),
        )
    }
}

data class CompetitorGroup(val status: String, val items: List<Competitor>)

data class CompetitorBoard(val organization: String, val role: String, val competitors: List<Competitor>) {
    fun grouped(query: String = ""): List<CompetitorGroup> =
        Competitor.STATUSES.map { status -> CompetitorGroup(status, competitors.filter { it.status == status && it.matches(query) }) }.filter { it.items.isNotEmpty() }

    companion object {
        fun fromJson(json: JsonMap) = CompetitorBoard(
            json.str("organization"), json.str("role", "member"), json.maps("competitors").map(Competitor::fromJson).filter { PackagesText.isHexId(it.id) },
        )
    }
}

data class OwnedAd(
    val id: String,
    val headline: String = "",
    val body: String = "",
    val cta: String = "",
    val sponsor: String = "",
    val url: String = "",
    val category: String = "",
    val placements: List<String> = emptyList(),
    val status: String = "draft",
    val delivery: String = "draft",
    val paused: Boolean = false,
    val reviewReason: String = "",
    val reviewCodes: List<String> = emptyList(),
    val screenFlags: List<String> = emptyList(),
    val contentHash: String = "",
    val clicks: Long = 0,
    val clickWindowDays: Int = 30,
    val submittedAt: Long = 0,
    val updatedAt: Long = 0,
    val owner: String = "",
    val openReports: Int = 0,
) {
    val ownerActions: List<String>
        get() = when (status) {
            "draft", "rejected", "withdrawn" -> listOf("submit")
            "pending" -> listOf("withdraw")
            "approved" -> if (paused) listOf("resume", "withdraw") else listOf("pause", "withdraw")
            else -> emptyList()
        }

    val reviewActions: List<String>
        get() = when (status) {
            "pending" -> listOf("approve", "reject")
            "approved" -> listOf("suspend")
            "suspended" -> listOf("reinstate")
            else -> emptyList()
        }

    val deliveryText: String
        get() = when (delivery) {
            "running" -> "Running"
            "pending" -> "Waiting for review"
            "review-expired" -> "Approval expired"
            "paused" -> "Paused"
            "scheduled" -> "Scheduled"
            "ended" -> "Ended"
            "needs-membership" -> "Needs a membership to run"
            "over-allowance" -> "Over your live-ad allowance"
            "rejected" -> "Rejected"
            "suspended" -> "Suspended"
            "withdrawn" -> "Withdrawn"
            "draft" -> "Draft"
            else -> CrmStageLabel.title(delivery)
        }

    companion object {
        fun fromJson(json: JsonMap): OwnedAd {
            val clicks = json.obj("clicks")
            return OwnedAd(
                id = json.str("id"), headline = json.str("headline"), body = json.str("body"), cta = json.str("cta"),
                sponsor = json.str("sponsor"), url = json.str("url"), category = json.str("categoryLabel", json.str("category")),
                placements = json.strings("placements"), status = json.str("status", "draft"),
                delivery = json.str("delivery", json.str("status")), paused = json.bool("paused"),
                reviewReason = json.str("reviewReason"), reviewCodes = json.strings("reviewCodes"),
                screenFlags = json.strings("screenFlags"), contentHash = json.str("contentHash"),
                clicks = clicks.long("total"), clickWindowDays = clicks.int("windowDays", 30),
                submittedAt = json.long("submittedAt"), updatedAt = json.long("updatedAt"), owner = json.str("owner"),
                openReports = json.int("openReports"),
            )
        }
    }
}

data class AdsAccount(
    val tierLabel: String,
    val liveAds: Int,
    val running: Int,
    val pending: Int,
    val pendingLimit: Int,
    val total: Int,
    val totalLimit: Int,
    val ads: List<OwnedAd>,
    val isReviewer: Boolean,
) {
    companion object {
        fun fromJson(json: JsonMap): AdsAccount {
            val tier = json.obj("tier")
            val usage = json.obj("usage")
            return AdsAccount(
                tier.str("label"), usage.int("liveAds", tier.int("liveAds")), usage.int("running"), usage.int("pending"),
                usage.int("pendingLimit"), usage.int("total"), usage.int("totalLimit"),
                json.maps("ads").map(OwnedAd::fromJson).filter { it.id.isNotEmpty() }, json.bool("isReviewer"),
            )
        }
    }
}

data class AdReviewReason(val id: String, val label: String)

data class AdReviewQueue(val status: String, val ads: List<OwnedAd>, val counts: Map<String, Int>, val prohibited: List<AdReviewReason>) {
    companion object {
        val FILTERS = listOf("pending", "approved", "suspended", "rejected", "reported")

        fun fromJson(json: JsonMap) = AdReviewQueue(
            json.str("status", "pending"),
            json.maps("ads").map(OwnedAd::fromJson).filter { it.id.isNotEmpty() },
            json.obj("counts").mapValues { jsonLong(it.value).toInt() },
            json.maps("prohibited").map { AdReviewReason(it.str("id"), it.str("label")) },
        )
    }
}

data class AdReviewDecision(val action: String, val reason: String = "", val codes: List<String> = emptyList()) {
    val needsReason: Boolean get() = action == "reject" || action == "suspend"

    val isComplete: Boolean get() = !needsReason || reason.trim().isNotEmpty() || codes.isNotEmpty()

    fun body(contentHash: String): Map<String, Any?> {
        val body = linkedMapOf<String, Any?>("action" to action)
        val trimmed = reason.trim()
        if (trimmed.isNotEmpty()) body["reason"] = trimmed.take(500)
        if (codes.isNotEmpty()) body["codes"] = codes.toSortedSet().toList()
        if (action == "approve") body["contentHash"] = contentHash
        return body
    }
}

class OrgPackageModel<P : Any>(
    var organization: String = "",
    private val listOrganizations: () -> List<PackageOrganization>,
    private val loadPayload: (String) -> P,
) {
    var organizations: List<PackageOrganization> = emptyList()
        private set
    var payload: P? = null
        private set
    var errorText: String = ""

    val installedOrganizations: List<PackageOrganization> get() = organizations.filter { it.installed }

    fun refresh() {
        try {
            organizations = listOrganizations()
            if (organization.isEmpty() || organizations.none { it.name == organization }) {
                organization = installedOrganizations.firstOrNull()?.name ?: ""
            }
            errorText = ""
        } catch (thrown: Exception) {
            errorText = PackagesException.describe(thrown)
            return
        }
        load()
    }

    fun load() {
        if (!PackagesText.isOrgName(organization)) {
            payload = null
            return
        }
        try {
            payload = loadPayload(organization)
            errorText = ""
        } catch (thrown: Exception) {
            errorText = PackagesException.describe(thrown)
        }
    }

    fun update(change: (P) -> P) {
        payload = payload?.let(change)
    }
}

class AdsModel(private val client: PackagesClient) {
    var account: AdsAccount? = null
        private set
    var queue: AdReviewQueue? = null
        private set
    var errorText: String = ""
        private set
    var reviewFilter: String = "pending"

    val isReviewer: Boolean get() = account?.isReviewer ?: false

    fun refresh() {
        try {
            account = client.myAds()
            errorText = ""
        } catch (thrown: Exception) {
            errorText = PackagesException.describe(thrown)
            return
        }
        if (isReviewer) loadQueue()
    }

    fun loadQueue() {
        if (!isReviewer) {
            queue = null
            return
        }
        try {
            queue = client.adReviewQueue(reviewFilter)
        } catch (thrown: Exception) {
            errorText = PackagesException.describe(thrown)
        }
    }

    fun perform(action: String, ad: OwnedAd) {
        try {
            val updated = client.adAction(ad.id, action)
            account = account?.let { current -> current.copy(ads = current.ads.map { if (it.id == ad.id) updated else it }) }
            errorText = ""
        } catch (thrown: Exception) {
            errorText = PackagesException.describe(thrown)
        }
    }

    fun review(ad: OwnedAd, decision: AdReviewDecision): Boolean {
        if (!isReviewer) return false
        return try {
            client.reviewAd(ad, decision)
            queue = queue?.let { current -> current.copy(ads = current.ads.filter { it.id != ad.id }) }
            errorText = ""
            true
        } catch (thrown: Exception) {
            errorText = PackagesException.describe(thrown)
            false
        }
    }
}
