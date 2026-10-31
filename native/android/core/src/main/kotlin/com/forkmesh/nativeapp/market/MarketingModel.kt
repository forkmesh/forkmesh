package com.forkmesh.nativeapp.market

import com.forkmesh.nativeapp.json.JsonMap
import com.forkmesh.nativeapp.json.obj
import com.forkmesh.nativeapp.json.str
import java.time.ZoneId

class MarketingModel(
    private val client: PackagesClient,
    organization: String = "",
    val zone: ZoneId = ZoneId.systemDefault(),
    private val clock: () -> Long = System::currentTimeMillis,
) {
    val packages = OrgPackageModel(organization, { client.marketingOrganizations() }) { org -> client.marketing(org) }

    @Volatile var working: Set<String> = emptySet()
        private set
    var notice: String = ""
    var advice: MarketingAdvice? = null
        private set
    var performance: MarketingPerformance? = null
        private set
    var performanceDays: Int = 28
    var performanceProject: String = ""
    var report: MarketingReport? = null
        private set
    var readyPost: MarketingReadyPosts? = null
        private set
    private var readyOrganization = ""
    var appStoreRange: Int = 30
    private var appStoreLoaded: MarketingAppStore? = null
    private var appStoreOrganization = ""

    val appStore: MarketingAppStore? get() = appStoreLoaded?.takeIf { appStoreOrganization == organization }

    val overview: MarketingOverview? get() = packages.payload
    val organization: String get() = packages.organization
    val now: Long get() = clock()

    var errorText: String
        get() = packages.errorText
        set(value) {
            packages.errorText = value
        }

    fun refresh() = packages.refresh()

    val readyX: MarketingReadyPosts?
        get() {
            val summary = overview?.readyX ?: return null
            val loaded = readyPost?.takeIf { readyOrganization == organization } ?: return summary
            return if (loaded.post?.id == summary.post?.id) loaded else summary
        }

    val readyImageMissing: Boolean get() = readyX?.post?.let { it.hasImage && it.imageDataUrl.isEmpty() } == true

    fun reload() = packages.load()

    fun isWorking(id: String): Boolean = id in working

    private inline fun <T> busy(key: String, work: () -> T): T {
        working = working + key
        try {
            return work()
        } finally {
            working = working - key
        }
    }

    fun draft(kind: MarketingKind, record: MarketingRecord): MarketingDraft? =
        overview?.let { MarketingDraft.of(kind, it, record.json, record.id, zone) }

    fun newDraft(kind: MarketingKind): MarketingDraft? {
        val overview = overview ?: return null
        return when (kind) {
            MarketingKind.ATTRIBUTION, MarketingKind.TARGETS, MarketingKind.GOALS, MarketingKind.REDDIT_WATCH -> MarketingForms.settingsDraft(kind, overview, zone)
            MarketingKind.CONTENT -> MarketingForms.contentDraft(overview, zone = zone)
            else -> MarketingDraft.of(kind, overview, zone = zone)
        }
    }

    fun save(draft: MarketingDraft): String? {
        val overview = overview ?: return "Nothing is loaded yet."
        return busy(draft.id.ifEmpty { draft.kind.name }) {
            try {
                val reply = client.saveMarketing(draft, organization, overview, now, zone)
                notice = savedText(draft, reply)
                packages.load()
                null
            } catch (error: Exception) {
                PackagesException.describe(error)
            }
        }
    }

    fun setStatus(kind: MarketingKind, id: String, status: String) = busy(id) {
        try {
            val extra = if (kind == MarketingKind.CONTENT && status == "published" && overview?.content?.firstOrNull { it.id == id }?.publishedAt == 0L) {
                mapOf("publishedAt" to now)
            } else {
                emptyMap()
            }
            client.setMarketingStatus(kind, id, status, organization, extra)
            notice = "Moved to ${MarketingText.title(status)}."
            packages.load()
        } catch (error: Exception) {
            errorText = PackagesException.describe(error)
        }
    }

    fun delete(kind: MarketingKind, id: String): Boolean = busy(id) {
        try {
            client.deleteMarketing(kind, id, organization)
            notice = kind.title + " deleted."
            packages.load()
            true
        } catch (error: Exception) {
            errorText = PackagesException.describe(error)
            false
        }
    }

    fun toggleTask(campaign: MarketingRecord, index: Int, done: Boolean) = patchCampaign(campaign, MarketingForms.taskToggleBody(campaign, index, done))

    fun addTask(campaign: MarketingRecord, title: String) {
        val clean = title.trim()
        if (clean.isEmpty()) return
        patchCampaign(campaign, MarketingForms.addTaskBody(campaign, clean))
    }

    private fun patchCampaign(campaign: MarketingRecord, body: Map<String, Any?>) = busy(campaign.id) {
        try {
            client.sendMarketing(MarketingKind.CAMPAIGN, campaign.id, organization, body)
            packages.load()
        } catch (error: Exception) {
            errorText = PackagesException.describe(error)
        }
    }

    fun refreshChannel(channel: String) = busy("refresh:$channel") {
        val name = overview?.channelName(channel) ?: channel
        try {
            val (fresh, message) = client.refreshMarketingChannel(channel, organization)
            packages.update { fresh }
            notice = "$name refreshed · $message"
        } catch (error: Exception) {
            errorText = "$name could not be refreshed: " + PackagesException.describe(error)
        }
    }

    fun refreshStale() {
        val stale = overview?.staleChannels.orEmpty()
        if (stale.isEmpty()) {
            notice = "Every channel that refreshes itself is up to date."
            return
        }
        stale.forEach(::refreshChannel)
    }

    fun addChannels(ids: List<String>) {
        val overview = overview ?: return
        if (ids.isEmpty()) return
        busy("channels") {
            try {
                client.addMarketingChannels(ids, organization, overview)
                notice = MarketingText.plural(ids.size, "channel", "channels") + " added."
                packages.load()
            } catch (error: Exception) {
                errorText = PackagesException.describe(error)
            }
        }
    }

    fun ask(campaignId: String = "", question: String = "") = busy("assistant") {
        try {
            advice = client.marketingAdvice(organization, campaignId, question)
        } catch (error: Exception) {
            errorText = PackagesException.describe(error)
        }
    }

    fun clearAdvice() {
        advice = null
    }

    fun apply(idea: MarketingIdea): MarketingDraft? {
        val overview = overview ?: return null
        val campaign = advice?.let { overview.campaign(it.campaignId) }
        if (idea.kind == "task") {
            if (campaign == null) {
                errorText = "Pick a campaign to add tasks to."
                return null
            }
            addTask(campaign, idea.title)
            notice = "Task added to ${campaign.title}."
            return null
        }
        return MarketingForms.ideaDraft(idea, overview, campaign, now, zone)
    }

    fun loadReport() = busy("report") {
        try {
            report = client.marketingReport(organization)
        } catch (error: Exception) {
            errorText = PackagesException.describe(error)
        }
    }

    fun loadPerformance() = busy("performance") {
        try {
            performance = client.marketingPerformance(organization, performanceDays, performanceProject, now)
        } catch (error: Exception) {
            errorText = PackagesException.describe(error)
        }
    }

    fun buildLink(draft: MarketingDraft): Pair<MarketingLink?, String?> {
        val overview = overview ?: return null to "Nothing is loaded yet."
        return try {
            client.buildMarketingLink(draft, organization, overview, now) to null
        } catch (error: Exception) {
            null to PackagesException.describe(error)
        }
    }

    fun importKeywords(text: String, source: String, limit: Int, observedAt: Long): String? {
        val rows = MarketingKeywordImport.top(MarketingKeywordImport.rows(text), limit)
        if (rows.isEmpty()) return "No keywords found in that text."
        return busy("keywords") {
            try {
                val (created, updated, skipped) = client.importMarketingKeywords(rows, organization, source, observedAt)
                notice = "$created added · $updated updated" + if (skipped > 0) " · $skipped skipped" else ""
                packages.load()
                null
            } catch (error: Exception) {
                PackagesException.describe(error)
            }
        }
    }

    fun loadAppStore() = busy("app-store") {
        val org = organization
        try {
            appStoreLoaded = client.marketingAppStore(org)
            appStoreOrganization = org
            true
        } catch (error: Exception) {
            errorText = PackagesException.describe(error)
            false
        }
    }

    fun loadReadyPost() = busy("ready-post") {
        try {
            val fresh = client.readyPost(organization)
            readyPost = fresh
            readyOrganization = organization
            packages.update { it.copy(readyX = fresh.copy(post = fresh.post?.copy(imageDataUrl = ""))) }
        } catch (error: Exception) {
            errorText = PackagesException.describe(error)
        }
    }

    fun closeReadyPost(status: String, url: String = ""): Boolean {
        val post = readyX?.post ?: return false
        return busy("ready-post") {
            val closed = try {
                readyPost = client.closeReadyPost(organization, post.id, status, url = if (status == "posted") url else "")
                readyOrganization = organization
                notice = (if (status == "posted") "Marked posted." else "Skipped.") + " Herald writes the next one."
                true
            } catch (error: Exception) {
                readyPost = null
                notice = PackagesException.describe(error)
                false
            }
            packages.load()
            closed
        }
    }

    fun disconnectX() = busy("x") {
        try {
            client.disconnectX(organization)
            notice = "X disconnected."
            packages.load()
        } catch (error: Exception) {
            errorText = PackagesException.describe(error)
        }
    }

    companion object {
        fun savedText(draft: MarketingDraft, reply: JsonMap): String = when (draft.kind) {
            MarketingKind.X_POST -> if (reply.str("url").isEmpty()) "Posted to X." else "Posted to X: " + reply.str("url")
            MarketingKind.X_CONNECTION -> "Connected X as @" + reply.obj("x").str("username") + "."
            MarketingKind.METRIC -> "Numbers logged."
            MarketingKind.FUNNEL_WEEK -> "Week saved."
            MarketingKind.CHANNEL, MarketingKind.ATTRIBUTION, MarketingKind.TARGETS, MarketingKind.GOALS, MarketingKind.REDDIT_WATCH -> "Settings saved."
            else -> if (draft.isNew) draft.kind.title + " added." else "Saved."
        }
    }
}
