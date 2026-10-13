package com.forkmesh.nativeapp.market

import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.json.JsonMap
import com.forkmesh.nativeapp.json.bool
import com.forkmesh.nativeapp.json.double
import com.forkmesh.nativeapp.json.int
import com.forkmesh.nativeapp.json.long
import com.forkmesh.nativeapp.json.maps
import com.forkmesh.nativeapp.json.obj
import com.forkmesh.nativeapp.json.optBool
import com.forkmesh.nativeapp.json.str
import java.net.URI
import java.util.Locale

enum class PackageDataScope(val wire: String) {
    ORGANIZATION("organization"), ACCOUNT("account"), NONE("");

    companion object {
        fun of(wire: String): PackageDataScope = entries.firstOrNull { it.wire == wire } ?: NONE
    }
}

data class PackagePermission(val id: String, val label: String, val risk: String)

data class PackageScreenshot(val url: String, val alt: String = "") {
    companion object {
        fun fromJson(json: JsonMap): PackageScreenshot? {
            val url = json.str("url")
            val uri = runCatching { URI(url) }.getOrNull() ?: return null
            if (uri.scheme != "https" || uri.host.isNullOrEmpty()) return null
            return PackageScreenshot(url, PackagesText.bounded(json.str("alt"), 160))
        }
    }
}

sealed class PackagePhoneSurface {
    data class Native(val destination: String) : PackagePhoneSurface()
    data class Web(val route: String) : PackagePhoneSurface()
    data object DesktopOnly : PackagePhoneSurface()
    data object WorldOnly : PackagePhoneSurface()
}

data class MarketPackage(
    val id: String,
    val market: String = "forkmesh",
    val name: String,
    val version: String = "",
    val summary: String = "",
    val category: String = "",
    val type: String = "",
    val desktop: Boolean = false,
    val webRoute: String = "",
    val worker: Boolean = false,
    val world: Boolean = false,
    val api: String = "",
    val dataScope: PackageDataScope = PackageDataScope.NONE,
    val bundled: Boolean = false,
    val free: Boolean = true,
    val permissions: List<PackagePermission> = emptyList(),
    val screenshots: List<PackageScreenshot> = emptyList(),
    val isDefault: Boolean = false,
    val accountSync: Boolean = false,
    val dependencies: List<String> = emptyList(),
    val settings: List<PackageSetting> = emptyList(),
    val monitors: PackageMonitors = PackageMonitors(),
    val installedBefore: String = "",
    val places: List<String> = PackagePlaces.ALL,
) {
    val phoneSurface: PackagePhoneSurface
        get() {
            NATIVE_DESTINATIONS[id]?.let { return PackagePhoneSurface.Native(it) }
            if (webRoute.isNotEmpty()) return PackagePhoneSurface.Web(webRoute)
            if (world && !desktop) return PackagePhoneSurface.WorldOnly
            return PackagePhoneSurface.DesktopOnly
        }

    val installsPerOrganization: Boolean get() = dataScope == PackageDataScope.ORGANIZATION

    val hasSensitivePermissions: Boolean get() = permissions.any { it.risk == "high" || it.risk == "sensitive" }

    companion object {
        val NATIVE_DESTINATIONS = mapOf(
            "crm" to "crm", "marketing" to "marketing", "analytics" to "analytics", "competitor" to "competitors",
            "ledger" to "ledger", "vault" to "vault", "calories" to "calories", "notes" to "notes", "tasks" to "tasks",
            "calendar" to "calendar", "timers" to "timers", "tour" to "tour", "status-page" to "status",
            "glasses-alert" to "glassesAlert", "agents" to "agents", "inbox" to "inbox", "code" to "code", "chat" to "chat",
            "bug-reports" to "reportBug", "desktops" to "desktops", "special-agents" to "teammates", "shortcuts" to "shortcuts",
            "hosts" to "hosts", "network" to "network", "coves" to "coves", "on-device-ai" to "localAI", "activity" to "activity",
            "stats" to "stats", "organizations" to "orgs", "sign-in" to "signIn", "issues" to "repo-issues", "commits" to "repo-commits", "branches" to "repo-branches",
            "pull-requests" to "repo-pulls", "releases" to "repo-releases", "actions" to "repo-actions",
            "discussions" to "repo-discussions",
        )

        fun fromJson(json: JsonMap): MarketPackage? {
            val id = json.str("id")
            if (!PackagesText.isPackageId(id)) return null
            val surfaces = json.obj("surfaces")
            return MarketPackage(
                id = id,
                market = json.str("market", "forkmesh"),
                name = PackagesText.bounded(json.str("name"), 80).ifEmpty { id },
                version = json.str("version"),
                summary = PackagesText.bounded(json.str("summary"), 600),
                category = json.str("category"),
                type = json.str("type"),
                desktop = surfaces.bool("desktop"),
                webRoute = surfaces.str("web"),
                worker = surfaces.bool("worker"),
                world = surfaces.bool("world"),
                api = json.str("api"),
                dataScope = PackageDataScope.of(json.str("dataScope")),
                bundled = json.bool("bundled"),
                free = json.optBool("free") ?: true,
                permissions = json.maps("permissions").take(40).map { PackagePermission(it.str("id"), it.str("label"), it.str("risk", "sensitive")) },
                screenshots = json.maps("screenshots").take(8).mapNotNull(PackageScreenshot::fromJson),
                isDefault = json.bool("default"),
                accountSync = json.bool("accountSync"),
                dependencies = PackageDependencies.parse(json["dependencies"]),
                settings = json.maps("settings").take(20).mapNotNull(PackageSetting::fromJson),
                monitors = PackageMonitors.fromJson(json.obj("monitors")),
                installedBefore = PackagesText.installedBefore(json["installedBefore"] ?: json.obj("install")["installedBefore"]),
                places = PackagePlaces.parse(json["places"] ?: json.obj("install")["places"]),
            )
        }
    }
}

data class PackageCounts(val installs: Int = 0, val downloads: Int = 0)

data class RatingSummary(val average: Double = 0.0, val count: Int = 0, val histogram: List<Int> = listOf(0, 0, 0, 0, 0)) {
    val label: String get() = if (count == 0) "No ratings yet" else String.format(Locale.US, "%.1f (%d)", average, count)

    companion object {
        fun fromJson(json: JsonMap): RatingSummary {
            val bars = json.obj("histogram")
            return RatingSummary(
                json.double("average").coerceIn(0.0, 5.0),
                maxOf(0, json.int("count")),
                (1..5).map { maxOf(0, bars.int(it.toString())) },
            )
        }
    }
}

data class PluginInstall(
    val id: String,
    val installed: Boolean,
    val installedBy: String = "",
    val installedAt: Long = 0,
    val serverData: Boolean = false,
    val hasData: Boolean = false,
) {
    companion object {
        fun fromJson(json: JsonMap) = PluginInstall(
            json.str("id"), json.bool("installed"), json.str("installedBy"), json.long("installedAt"), json.bool("serverData"), json.bool("hasData"),
        )
    }
}

data class OrgPackageState(val organization: String, val role: String, val canManage: Boolean, val plugins: List<PluginInstall>) {
    fun isInstalled(packageId: String): Boolean = plugins.any { it.id == packageId && it.installed }

    fun removing(packageId: String): OrgPackageState =
        copy(plugins = plugins.map { if (it.id == packageId) PluginInstall(packageId, installed = false) else it })

    companion object {
        fun fromJson(json: JsonMap) = OrgPackageState(
            json.str("organization"), json.str("role", "member"), json.bool("canManage"), json.maps("plugins").map(PluginInstall::fromJson),
        )
    }
}

enum class MarketFilter(val wire: String, val title: String) {
    ALL("all", "All"), INSTALLED("installed", "Installed"), NOT_INSTALLED("not-installed", "Not installed");

    companion object {
        fun of(route: String?): MarketFilter = entries.firstOrNull { it.wire == route?.lowercase() } ?: ALL
    }
}

data class ReviewCategory(val id: String, val label: String)

data class ReviewRules(
    val reviewRequiredBelow: Int = 5,
    val minChars: Int = 40,
    val minWords: Int = 8,
    val maxChars: Int = 4000,
    val categories: List<ReviewCategory> = DEFAULT_CATEGORIES,
) {
    companion object {
        val DEFAULT_CATEGORIES = listOf(
            ReviewCategory("bug", "Bug"), ReviewCategory("missing-feature", "Missing feature"), ReviewCategory("docs", "Docs"),
            ReviewCategory("performance", "Performance"), ReviewCategory("ux", "UX"), ReviewCategory("other", "Other"),
        )

        fun fromJson(json: JsonMap): ReviewRules {
            val categories = json.maps("categories").map { ReviewCategory(it.str("id"), it.str("label")) }
            return ReviewRules(
                json.int("reviewRequiredBelow", 5), json.int("minChars", 40), json.int("minWords", 8), json.int("maxChars", 4000),
                categories.ifEmpty { DEFAULT_CATEGORIES },
            )
        }
    }
}

data class MarketReviewReply(val text: String, val by: String, val at: Long)

data class MarketReview(
    val id: String,
    val stars: Int,
    val reviewerName: String = "",
    val reviewerDisplayName: String = "",
    val problem: String = "",
    val improvement: String = "",
    val context: String = "",
    val comment: String = "",
    val category: String = "",
    val version: String = "",
    val verifiedUse: Boolean = false,
    val helpful: Int = 0,
    val viewerFoundHelpful: Boolean = false,
    val createdAt: Long = 0,
    val updatedAt: Long = 0,
    val edited: Boolean = false,
    val reply: MarketReviewReply? = null,
    val addressedIn: String = "",
) {
    val reviewerLabel: String get() = reviewerDisplayName.ifEmpty { reviewerName }

    companion object {
        fun fromJson(json: JsonMap): MarketReview {
            val reviewer = json.obj("reviewer")
            val reply = json.obj("reply")
            return MarketReview(
                id = json.str("id"),
                stars = json.int("stars", 5).coerceIn(1, 5),
                reviewerName = reviewer.str("name"),
                reviewerDisplayName = reviewer.str("displayName"),
                problem = json.str("problem"),
                improvement = json.str("improvement"),
                context = json.str("context"),
                comment = json.str("comment"),
                category = json.str("category"),
                version = json.str("version"),
                verifiedUse = json.bool("verifiedUse"),
                helpful = maxOf(0, json.int("helpful")),
                viewerFoundHelpful = json.bool("viewerFoundHelpful"),
                createdAt = json.long("createdAt"),
                updatedAt = json.long("updatedAt"),
                edited = json.bool("edited"),
                reply = if (reply.str("text").isEmpty()) null else MarketReviewReply(reply.str("text"), reply.str("by"), reply.long("at")),
                addressedIn = json.str("addressedIn"),
            )
        }
    }
}

data class ReviewViewer(
    val signedIn: Boolean,
    val name: String,
    val isMaintainer: Boolean,
    val isAdmin: Boolean,
    val canRate: Boolean,
    val reason: String,
    val review: MarketReview?,
    val views: Int = 0,
    val opens: Int = 0,
) {
    val cannotRateReason: String get() = if (canRate) "" else PackagesException.sentence(reason.ifEmpty { "sign_in_required" })

    companion object {
        fun fromJson(json: JsonMap): ReviewViewer {
            val own = json.obj("review")
            return ReviewViewer(
                json.bool("signedIn"), json.str("name"), json.bool("isMaintainer"), json.bool("isAdmin"), json.bool("canRate"),
                json.str("reason"), if (own.str("id").isEmpty()) null else MarketReview.fromJson(own),
                maxOf(0, json.int("views")), maxOf(0, json.int("opens")),
            )
        }
    }
}

data class ReviewPage(
    val summary: RatingSummary,
    val reviews: List<MarketReview>,
    val total: Int,
    val hasMore: Boolean,
    val viewer: ReviewViewer,
    val rules: ReviewRules,
    val activity: PackageActivityTotals = PackageActivityTotals(),
) {
    companion object {
        fun fromJson(json: JsonMap): ReviewPage {
            val reviews = json.maps("reviews").map(MarketReview::fromJson)
            return ReviewPage(
                RatingSummary.fromJson(json.obj("summary")), reviews, json.int("total", reviews.size), json.bool("hasMore"),
                ReviewViewer.fromJson(json.obj("viewer")), ReviewRules.fromJson(json.obj("rules")),
                PackageActivityTotals.fromJson(json.obj("activity")),
            )
        }
    }
}

sealed class QuickRate {
    data object Submitted : QuickRate()
    data object NeedsReview : QuickRate()
    data class Failed(val message: String) : QuickRate()
}

data class ReviewDraft(
    val stars: Int = 5,
    val problem: String = "",
    val improvement: String = "",
    val context: String = "",
    val comment: String = "",
    val category: String = "",
    val version: String = "",
) {
    fun needsReview(rules: ReviewRules): Boolean = stars < rules.reviewRequiredBelow

    fun issues(rules: ReviewRules): Map<String, String> {
        val out = LinkedHashMap<String, String>()
        val fields = linkedMapOf("problem" to problem, "improvement" to improvement, "context" to context, "comment" to comment)
        for ((key, value) in fields) if (value.length > rules.maxChars) out[key] = "too_long"
        if (!needsReview(rules)) return out
        for (key in listOf("problem", "improvement")) {
            if (out[key] != null) continue
            val text = clean(fields[key] ?: "")
            when {
                text.isEmpty() -> out[key] = "required"
                text.length < rules.minChars -> out[key] = "too_short"
                words(text).size < rules.minWords -> out[key] = "too_few_words"
            }
        }
        if (out["improvement"] == null && improvement.isNotEmpty() && clean(improvement).lowercase() == clean(problem).lowercase()) {
            out["improvement"] = "same_as_problem"
        }
        return out
    }

    val body: Map<String, Any?>
        get() {
            val body = linkedMapOf<String, Any?>("stars" to stars.coerceIn(1, 5).toLong())
            for ((key, value) in listOf(
                "problem" to problem, "improvement" to improvement, "context" to context, "comment" to comment,
                "category" to category, "version" to version,
            )) {
                val trimmed = value.trim()
                if (trimmed.isNotEmpty()) body[key] = trimmed
            }
            return body
        }

    companion object {
        fun of(review: MarketReview) = ReviewDraft(
            review.stars, review.problem, review.improvement, review.context, review.comment, review.category, review.version,
        )

        fun clean(text: String): String =
            text.split('\n').joinToString("\n") { line -> line.split(' ', '\t').filter { it.isNotEmpty() }.joinToString(" ") }.trim()

        fun words(text: String): List<String> =
            text.lowercase().split(Regex("[^\\p{L}\\p{N}]+")).filter { word -> word.count { it.isLetter() } >= 2 }

        fun fieldMessage(code: String): String = when (code) {
            "required" -> "Required below five stars."
            "too_short" -> "Write at least a couple of sentences."
            "too_few_words" -> "Use a few more words."
            "repeated_characters" -> "Too many repeated characters."
            "repetitive" -> "Too repetitive."
            "same_as_problem" -> "Say how to improve, not the problem again."
            "too_long" -> "Too long."
            "invalid" -> "Not a valid choice."
            else -> code.replace('_', ' ')
        }
    }
}
data class PackageRemoval(val packageId: String, val organization: String, val filename: String, val export: String?) {
    companion object {
        fun fromJson(json: JsonMap, packageId: String, organization: String) = PackageRemoval(
            packageId,
            organization,
            "forkmesh-$packageId-$organization.json",
            if (json.bool("hadData") && json["export"] != null) Json.stringify(json["export"]) else null,
        )
    }
}

data class MarketShelf(val category: String, val packages: List<MarketPackage>)

class MarketModel(
    private val client: PackagesClient,
    var filter: MarketFilter = MarketFilter.ALL,
    private val accountSwitch: (String) -> Boolean? = { null },
    private val deviceSwitch: (String) -> Boolean? = { null },
) {
    var packages: List<MarketPackage> = emptyList()
        private set
    var counts: Map<String, PackageCounts> = emptyMap()
        private set
    var ratings: Map<String, RatingSummary> = emptyMap()
        private set
    var organizations: List<OrgPackageState> = emptyList()
        private set
    var errorText: String = ""
        private set
    var partialError: String = ""
        private set
    var removed: PackageRemoval? = null
        private set
    var query: String = ""

    fun refresh(organizationNames: List<String>) {
        try {
            packages = client.packages()
            errorText = ""
        } catch (thrown: Exception) {
            errorText = PackagesException.describe(thrown)
            return
        }
        val problems = ArrayList<String>()
        runCatching { client.installCounts() }.onSuccess { counts = it }.onFailure { problems.add("install counts") }
        runCatching { client.ratingSummaries() }.onSuccess { ratings = it }.onFailure { problems.add("ratings") }
        organizations = organizationNames.filter(PackagesText::isOrgName).take(20).mapNotNull { runCatching { client.organizationPackages(it) }.getOrNull() }
        partialError = if (problems.isEmpty()) "" else "Couldn't load " + problems.joinToString(" and ") + "."
    }

    fun isInstalled(pkg: MarketPackage): Boolean = when {
        pkg.accountSync -> accountSwitch(pkg.id) ?: pkg.isDefault
        deviceSwitch(pkg.id) == false -> false
        else -> when (pkg.dataScope) {
            PackageDataScope.ORGANIZATION -> organizations.any { it.isInstalled(pkg.id) }
            else -> deviceSwitch(pkg.id) ?: pkg.bundled
        }
    }

    fun marketParent(pkg: MarketPackage): String? {
        val parent = pkg.dependencies.firstOrNull() ?: return null
        return parent.takeIf { it != pkg.id && packages.any { other -> other.id == it } }
    }

    fun nested(pkg: MarketPackage): List<MarketPackage> = packages.filter { marketParent(it) == pkg.id }

    val visible: List<MarketPackage>
        get() = packages.filter { pkg ->
            if (marketParent(pkg) != null) return@filter false
            val installed = isInstalled(pkg)
            when (filter) {
                MarketFilter.ALL -> true
                MarketFilter.INSTALLED -> installed
                MarketFilter.NOT_INSTALLED -> !installed
            } && (listOf(pkg) + nested(pkg)).any { PackagesText.matches(query, it.name, it.summary, it.category) }
        }

    val shelves: List<MarketShelf>
        get() {
            val rows = visible
            return rows.map { it.category.ifEmpty { "Other" } }.toSortedSet().map { category ->
                MarketShelf(category, rows.filter { it.category.ifEmpty { "Other" } == category })
            }
        }

    fun pkg(id: String): MarketPackage? = packages.firstOrNull { it.id == id }

    fun installTargets(pkg: MarketPackage): List<OrgPackageState> =
        if (!pkg.installsPerOrganization) emptyList() else organizations.filter { it.canManage }

    fun install(pkg: MarketPackage, organization: String): Boolean = try {
        val state = client.install(pkg.id, organization)
        val index = organizations.indexOfFirst { it.organization == organization }
        organizations = if (index >= 0) organizations.toMutableList().also { it[index] = state } else organizations + state
        errorText = ""
        true
    } catch (thrown: Exception) {
        errorText = PackagesException.describe(thrown)
        false
    }

    fun uninstall(pkg: MarketPackage, organization: String): Boolean {
        try {
            removed = client.uninstall(pkg.id, organization)
        } catch (thrown: Exception) {
            if ((thrown as? PackagesException)?.code != "plugin_not_installed") {
                errorText = PackagesException.describe(thrown)
                return false
            }
        }
        organizations = organizations.map { if (it.organization == organization) it.removing(pkg.id) else it }
        errorText = ""
        return true
    }

    fun dismissRemoval() {
        removed = null
    }
}

class PackageReviewsModel(private val client: PackagesClient, val packageId: String) {
    var page: ReviewPage? = null
        private set
    var errorText: String = ""
        private set
    var serverFields: Map<String, String> = emptyMap()
        private set
    var draft: ReviewDraft = ReviewDraft()
    var activity: PackageActivityPage? = null
        private set
    var activityError: String = ""
        private set

    val rules: ReviewRules get() = page?.rules ?: ReviewRules()

    val fieldIssues: Map<String, String>
        get() = LinkedHashMap(draft.issues(rules)).also { issues -> serverFields.forEach { (k, v) -> issues.putIfAbsent(k, v) } }

    val canSubmit: Boolean get() = draft.issues(rules).isEmpty() && (page?.viewer?.canRate ?: false)

    fun refresh() {
        try {
            val loaded = client.reviews(packageId)
            page = loaded
            loaded.viewer.review?.let { draft = ReviewDraft.of(it) }
            errorText = ""
        } catch (thrown: Exception) {
            errorText = PackagesException.describe(thrown)
        }
    }

    fun submit(): Boolean {
        if (!canSubmit) return false
        return try {
            client.rate(packageId, draft)
            serverFields = emptyMap()
            refresh()
            true
        } catch (thrown: PackagesException) {
            serverFields = thrown.fields
            errorText = thrown.message ?: ""
            false
        } catch (thrown: Exception) {
            errorText = PackagesException.describe(thrown)
            false
        }
    }

    val canSeeActivity: Boolean get() = page?.viewer?.let { it.isMaintainer || it.isAdmin } ?: false

    fun quickRate(stars: Int): QuickRate {
        val viewer = page?.viewer ?: return QuickRate.Failed(errorText.ifEmpty { PackagesException.sentence("sign_in_required") })
        if (!viewer.canRate) return QuickRate.Failed(viewer.cannotRateReason)
        val next = draft.copy(stars = stars.coerceIn(1, 5))
        draft = next
        if (next.needsReview(rules) || next.issues(rules).isNotEmpty()) return QuickRate.NeedsReview
        return if (submit()) QuickRate.Submitted else QuickRate.Failed(errorText.ifEmpty { PackagesException.sentence("review_invalid") })
    }

    fun refreshActivity(market: String = "forkmesh", page: Int = 1) {
        if (!canSeeActivity) {
            activity = null
            return
        }
        try {
            activity = client.packageActivity(packageId, market, page)
            activityError = ""
        } catch (thrown: Exception) {
            activityError = PackagesException.describe(thrown)
        }
    }

    fun toggleHelpful(review: MarketReview) {
        try {
            val (helpful, mine) = client.setHelpful(review.id, !review.viewerFoundHelpful)
            val current = page ?: return
            page = current.copy(reviews = current.reviews.map { if (it.id == review.id) it.copy(helpful = helpful, viewerFoundHelpful = mine) else it })
        } catch (thrown: Exception) {
            errorText = PackagesException.describe(thrown)
        }
    }
}
