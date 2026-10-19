package com.forkmesh.nativeapp.market

import com.forkmesh.nativeapp.api.ApiJsonResult
import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.json.JsonMap
import com.forkmesh.nativeapp.json.bool
import com.forkmesh.nativeapp.json.int
import com.forkmesh.nativeapp.json.jsonText
import com.forkmesh.nativeapp.json.maps
import com.forkmesh.nativeapp.json.obj
import com.forkmesh.nativeapp.json.str
import com.forkmesh.nativeapp.control.LocalControlException
import java.io.IOException
import java.net.SocketTimeoutException

class PackagesException(val code: String, val status: Int = 0, val fields: Map<String, String> = emptyMap()) :
    RuntimeException(if (code == "form_invalid") fields["message"] ?: sentence(code, status) else sentence(code, status)) {
    companion object {
        fun sentence(code: String, status: Int = 0): String = when (code) {
            "invalid_session", "unauthorized", "sign_in_required" -> "Sign in again to continue."
            "organization_not_found" -> "That organization was not found, or you are not a member."
            "plugin_not_installed" -> "This package is not installed for that organization yet."
            "plugin_not_found", "unknown_package" -> "This package is not in the Market."
            "maintainer_required" -> "Only the package's maintainers can see who opened it."
            "invalid_kind" -> "The app sent an activity the server does not know."
            "plugin_installs_per_account" -> "Each account installs this package for itself."
            "organization_admin_required" -> "Only an owner or admin of the organization can do that."
            "forbidden", "permission_denied" -> "Your account is not allowed to do that."
            "origin_not_allowed" -> "The server refused a request from this app."
            "email_not_verified" -> "Confirm your email address first."
            "maintainer_cannot_rate" -> "Maintainers cannot rate their own package."
            "package_use_required" -> "Install or use the package before rating it."
            "rating_rate_limited" -> "Too many ratings for now. Try again later."
            "review_required" -> "Say what didn't work and how to improve it."
            "review_invalid" -> "Some review fields need more detail."
            "contact_email_exists" -> "Another contact already uses that email address."
            "name_or_email_required" -> "A contact needs a name or an email address."
            "invalid_email" -> "That email address is not valid."
            "contact_not_found", "competitor_not_found", "content_not_found", "not_found" -> "It was removed or you no longer have access."
            "reason_required" -> "Give the advertiser a reason."
            "daily_limit_reached" -> "You reached today's outreach limit. Try again tomorrow."
            "outreach_forbidden" -> "Ask an owner or admin to add you to the outreach team."
            "invalid_recipient" -> "That recipient address is not valid."
            "subject_required", "subject_and_message_required" -> "Add a subject and a message."
            "body_required" -> "Write a message first."
            "send_failed", "mailtrap_send_failed" -> "The email provider refused the send."
            "invalid_sender" -> "Set a valid sender address in CRM settings."
            "merge_needs_two_contacts" -> "Pick a different contact to merge."
            "view_not_found" -> "That list was removed or is no longer shared with you."
            "view_owner_required" -> "Only the list's owner or an admin can do that."
            "view_already_in_organization" -> "That list is already in this organization."
            "invalid_mail_box" -> "That mail box doesn't exist."
            "zoho_not_connected" -> "Zoho Mail is not connected."
            "zoho_one_click_unavailable" -> "Zoho one-click connect is not available on this server."
            "deck_link_not_found" -> "That deck link was removed."
            "member_not_found" -> "No organization member has that name."
            "csv_required" -> "The file is empty."
            "invalid_csv" -> "The file is not a readable CSV."
            "csv_header_required" -> "The file has no header row."
            "too_many_csv_rows" -> "The file has more than 2000 rows."
            "edits_required" -> "Select some contacts first."
            "contact_exists" -> "Another contact already uses that email address."
            "title_required", "experiment_title_required" -> "Give it a title."
            "campaign_name_required", "ad_name_required" -> "Give it a name."
            "keyword_term_required" -> "Write the search phrase."
            "invalid_content_url", "invalid_ad_url", "invalid_campaign_url", "invalid_keyword_url", "invalid_channel_url", "invalid_link_url" ->
                "Links must start with https://."
            "invalid_ad_dates", "invalid_campaign_dates", "invalid_experiment_dates" -> "The end date is before the start."
            "invalid_ad_currency", "invalid_campaign_currency" -> "Use a three-letter currency code such as USD."
            "invalid_ad_channel" -> "Pick an ad platform."
            "invalid_utm_value" -> "Tags use lowercase letters, digits, dots, dashes or underscores."
            "invalid_reddit_thread_url" -> "Paste a Reddit thread link (https://www.reddit.com/r/…/comments/…)."
            "invalid_reddit_reply_url", "invalid_reddit_comment_url" -> "Paste the link of a comment in that thread."
            "reddit_reply_not_in_thread", "reddit_comment_not_in_thread" -> "That comment is in a different thread."
            "reddit_reply_checks_required" ->
                "Before marking it replied, confirm you read the rules and disclosed who you work for, and add your reply link."
            "reddit_thread_exists" -> "That thread is already on the desk."
            "invalid_landing_path" -> "The landing path is a single page such as / or /pricing."
            "invalid_subreddit" -> "That is not a subreddit name."
            "too_many_reddit_watch" -> "Up to 40 subreddits and 40 searches."
            "invalid_reddit_query" -> "Searches are up to 120 characters."
            "invalid_reddit_account" -> "That is not a Reddit username."
            "too_many_reddit_accounts" -> "Up to 10 Reddit accounts."
            "invalid_post_url" -> "Paste the https link of the post."
            "ready_post_changed" -> "Herald replaced that post. The card shows the current one."
            "ready_post_not_found" -> "That post was already marked posted or skipped."
            "post_exists" -> "That link is already tracked."
            "post_channel_required" -> "Pick the channel the link was posted on."
            "marketing_posts_full" -> "The posted-link list is full."
            "keyword_exists" -> "That keyword is already tracked."
            "invalid_keyword_rows" -> "No keywords found in that text."
            "funnel_week_required" -> "Pick the week."
            "x_credentials_required" -> "Fill in all four X keys."
            "x_not_connected" -> "Connect an X account in Marketing settings first."
            "x_post_text_required" -> "Write the post first."
            "x_post_too_long" -> "The post is too long."
            "x_account_not_found" -> "X did not recognise those keys."
            "x_access_token_required", "x_access_token_invalid" -> "Add an X API bearer token to this channel."
            "github_rate_limited" -> "GitHub is rate limiting; add an access token to this channel."
            "channel_refresh_unsupported" -> "This channel's numbers are logged by hand or with the browser extension."
            "channel_url_required" -> "Add the channel's link first."
            "channel_disabled" -> "Turn the channel on first."
            "channel_not_configured" -> "Add the channel first."
            "ai_unavailable" -> "The marketing assistant is not available right now."
            "rate_limited" -> "Too many requests for now. Try again in a minute."
            "campaign_not_found", "ad_not_found", "experiment_not_found", "keyword_not_found", "post_not_found", "reddit_thread_not_found",
            "funnel_week_not_found" -> "It was removed or you no longer have access."
            "zoho_code_incomplete" -> "Enter the Self Client's client ID, its client secret and the code."
            "zoho_code_rejected" -> "Zoho rejected the code in every data center. Codes work once and expire in minutes: generate a new one."
            "zoho_account_missing" -> "Zoho accepted the code, but that login has no Zoho Mail account."
            "plugin_not_account_synced" -> "This package is installed per device or organization, not for your account."
            "invalid_setting" -> "That setting value is not allowed."
            "invalid_places" -> "Pick at least one place to show it."
            "install_first" -> "Install this package first."
            "posting_too_fast" -> "You are posting quickly. Wait a few minutes and try again."
            "text_required" -> "Write something first."
            "text_too_long" -> "Posts can be at most 500 characters."
            "account_not_found" -> "That account was not found."
            "status_page_not_installed" -> "Install Status Page first."
            "invalid_name" -> "Give the monitor a name of 1 to 40 characters."
            "invalid_url" -> "Use an http:// or https:// address with a host, up to 300 characters."
            "too_many_monitors" -> "You already have the most monitors allowed. Remove one first."
            "monitor_not_found" -> "That monitor was removed."
            "offline" -> "You are offline."
            "desktop_unavailable" -> "Your desktop is not connected."
            "" -> if (status > 0) "The server answered HTTP $status." else "The request failed."
            else -> code.replace('_', ' ').replaceFirstChar { it.uppercase() }
        }

        fun from(reply: ApiJsonResult): PackagesException {
            val fields = reply.body.obj("fields").mapValues { jsonText(it.value) }
            return PackagesException(reply.body.str("error"), reply.statusCode, fields)
        }

        fun describe(error: Throwable): String = when (error) {
            is PackagesException -> error.message ?: ""
            is SocketTimeoutException -> "The server took too long to answer."
            is IOException -> "Couldn't reach the server."
            else -> "Something went wrong."
        }
    }
}

object PackagesText {
    fun isPackageId(value: String): Boolean {
        if (value.isEmpty() || value.length > 63 || value[0] !in 'a'..'z') return false
        return value.all { it in 'a'..'z' || it in '0'..'9' || it == '-' }
    }

    fun isOrgName(value: String): Boolean = isPackageId(value) && !value.endsWith("-")

    fun isHexId(value: String): Boolean = value.length == 32 && value.all { it in '0'..'9' || it in 'a'..'f' }

    fun bounded(value: String, limit: Int): String = if (value.length > limit) value.take(limit) else value

    private val DATE = Regex("^\\d{4}-\\d{2}-\\d{2}")

    fun installedBefore(value: Any?): String {
        val text = (value as? String)?.trim().orEmpty()
        return DATE.find(text)?.value.orEmpty()
    }

    fun matches(query: String, vararg fields: String): Boolean {
        val needle = query.trim().lowercase()
        if (needle.isEmpty()) return true
        return fields.any { it.lowercase().contains(needle) }
    }
}

fun interface PackagesTransport {
    fun send(method: String, path: String, query: List<Pair<String, String>>, body: Map<String, Any?>?): ApiJsonResult
}

class ApiPackagesTransport(private val api: ApiService) : PackagesTransport {
    override fun send(method: String, path: String, query: List<Pair<String, String>>, body: Map<String, Any?>?): ApiJsonResult =
        api.packagesRequest(method, path, query, body)
}

class PackagesClient(val transport: PackagesTransport) {
    fun call(method: String, path: String, query: List<Pair<String, String>> = emptyList(), body: Map<String, Any?>? = null): JsonMap {
        val reply = transport.send(method, path, query, body)
        if (reply.ok) return reply.body
        throw PackagesException.from(reply)
    }

    internal fun segment(value: String): String {
        if (!PackagesText.isOrgName(value)) throw PackagesException("organization_not_found")
        return value
    }

    internal fun item(value: String): String {
        if (!PackagesText.isHexId(value)) throw PackagesException("not_found", 404)
        return value
    }

    fun packages(): List<MarketPackage> = call("GET", "/api/market/packages").maps("packages").mapNotNull(MarketPackage::fromJson)

    fun installCounts(): Map<String, PackageCounts> =
        call("GET", "/api/market/installs").obj("packages").mapValues { (_, value) ->
            val map = value.asMap()
            PackageCounts(map.int("installs"), map.int("downloads"))
        }

    fun ratingSummaries(): Map<String, RatingSummary> =
        call("GET", "/api/market/reviews/summary").obj("packages").mapValues { (_, value) -> RatingSummary.fromJson(value.asMap()) }

    fun marketSummary(): MarketSummary = MarketSummary.fromJson(call("GET", "/api/market/reviews/summary"))

    fun organizationPackages(organization: String): OrgPackageState =
        OrgPackageState.fromJson(call("GET", "/api/plugins/" + segment(organization)))

    fun install(packageId: String, organization: String): OrgPackageState {
        if (!PackagesText.isPackageId(packageId)) throw PackagesException("plugin_not_found")
        val state = OrgPackageState.fromJson(call("POST", "/api/plugins/${segment(organization)}/$packageId/install", body = emptyMap()))
        runCatching { reportUse(packageId) }
        return state
    }

    fun uninstall(packageId: String, organization: String): PackageRemoval {
        if (!PackagesText.isPackageId(packageId)) throw PackagesException("plugin_not_found")
        val org = segment(organization)
        return PackageRemoval.fromJson(call("POST", "/api/plugins/$org/$packageId/uninstall", body = emptyMap()), packageId, org)
    }

    fun reportUse(packageId: String, market: String = "forkmesh") {
        call("POST", "/api/market/reviews/uses", body = linkedMapOf("market" to market, "package" to packageId, "source" to "mobile"))
    }

    fun reportActivity(packageId: String, kind: ActivityKind, market: String = "forkmesh"): Pair<Int, Int> {
        if (!PackagesText.isPackageId(packageId)) throw PackagesException("unknown_package")
        val reply = call(
            "POST", "/api/market/reviews/activity",
            body = linkedMapOf("market" to market, "package" to packageId, "kind" to kind.wire, "source" to "mobile"),
        )
        return reply.int("views") to reply.int("opens")
    }

    fun packageActivity(packageId: String, market: String = "forkmesh", page: Int = 1): PackageActivityPage {
        if (!PackagesText.isPackageId(packageId)) throw PackagesException("unknown_package")
        return PackageActivityPage.fromJson(
            call("GET", "/api/market/reviews/$market/$packageId/activity", listOf("page" to maxOf(1, page).toString(), "limit" to "50")),
        )
    }

    fun reviews(packageId: String, market: String = "forkmesh", sort: String = "newest", page: Int = 1): ReviewPage =
        ReviewPage.fromJson(
            call(
                "GET", "/api/market/reviews/$market/$packageId",
                listOf("sort" to sort, "page" to maxOf(1, page).toString(), "limit" to "20"),
            ),
        )

    fun rate(packageId: String, draft: ReviewDraft, market: String = "forkmesh"): MarketReview =
        MarketReview.fromJson(call("POST", "/api/market/reviews/$market/$packageId", body = draft.body).obj("review"))

    fun setHelpful(reviewId: String, helpful: Boolean): Pair<Int, Boolean> {
        val reply = call(if (helpful) "POST" else "DELETE", "/api/market/reviews/items/${item(reviewId)}/helpful", body = emptyMap())
        return reply.int("helpful") to reply.bool("viewerFoundHelpful")
    }

    fun reportReview(reviewId: String, reason: String) {
        call("POST", "/api/market/reviews/items/${item(reviewId)}/report", body = mapOf("reason" to reason.take(500)))
    }

    fun crmOrganizations(): List<PackageOrganization> = PackageOrganization.list(call("GET", "/api/crm"))

    fun crm(organization: String, since: String = "", offsetMinutes: Int = java.util.TimeZone.getDefault().getOffset(System.currentTimeMillis()) / 60_000): CrmOrganizationData? {
        val query = (if (since.isEmpty()) emptyList() else listOf("since" to since)) + ("tz" to offsetMinutes.toString())
        val reply = call("GET", "/api/crm/" + segment(organization), query)
        if (reply.bool("unchanged")) return null
        return CrmOrganizationData.fromJson(reply)
    }

    fun crmContact(ref: CrmContactRef): CrmContact =
        CrmContact.fromJson(call("GET", "/api/crm/${segment(ref.organization)}/contacts/${item(ref.contactId)}").obj("contact"))

    fun updateCrmContact(ref: CrmContactRef, changes: Map<String, Any?>): CrmContact =
        CrmContact.fromJson(
            call("PATCH", "/api/crm/${segment(ref.organization)}/contacts/${item(ref.contactId)}", body = changes).obj("contact"),
        )

    fun createCrmContact(organization: String, fields: Map<String, Any?>): CrmContact =
        CrmContact.fromJson(call("POST", "/api/crm/${segment(organization)}/contacts", body = fields).obj("contact"))

    fun deleteCrmContact(ref: CrmContactRef) {
        call("DELETE", "/api/crm/${segment(ref.organization)}/contacts/${item(ref.contactId)}", body = emptyMap())
    }

    fun marketingOrganizations(): List<PackageOrganization> = PackageOrganization.list(call("GET", "/api/marketing"))

    fun marketing(organization: String): MarketingOverview = MarketingOverview.fromJson(call("GET", "/api/marketing/" + segment(organization)))

    fun setContentStatus(organization: String, contentId: String, status: String, nowMs: Long = System.currentTimeMillis()): MarketingContent {
        if (status !in MarketingContent.STATUSES) throw PackagesException("invalid_status")
        val body = linkedMapOf<String, Any?>("status" to status)
        if (status == "published") body["publishedAt"] = nowMs
        return MarketingContent.fromJson(
            call("PATCH", "/api/marketing/${segment(organization)}/content/${item(contentId)}", body = body).obj("content"),
        )
    }

    fun setAdStatus(organization: String, adId: String, status: String): MarketingCampaignAd {
        if (status !in MarketingCampaignAd.STATUSES) throw PackagesException("invalid_status")
        return MarketingCampaignAd.fromJson(
            call("PATCH", "/api/marketing/${segment(organization)}/ads/${item(adId)}", body = mapOf("status" to status)).obj("ad"),
        )
    }

    fun analyticsOrganizations(): List<PackageOrganization> = PackageOrganization.list(call("GET", "/api/analytics"))

    fun analyticsProjects(organization: String): List<AnalyticsProject> =
        call("GET", "/api/analytics/${segment(organization)}/projects").maps("projects").map(AnalyticsProject::fromJson)
            .filter { PackagesText.isHexId(it.id) }

    fun analyticsOverview(organization: String, projectId: String, range: AnalyticsRange, nowMs: Long): AnalyticsOverview =
        AnalyticsOverview.fromJson(
            call("POST", "/api/analytics/${segment(organization)}/projects/${item(projectId)}/query", body = range.query(nowMs)),
        )

    fun competitorOrganizations(): List<PackageOrganization> = PackageOrganization.list(call("GET", "/api/competitors"))

    fun competitors(organization: String): CompetitorBoard = CompetitorBoard.fromJson(call("GET", "/api/competitors/" + segment(organization)))

    fun saveCompetitor(organization: String, competitor: Competitor): Competitor {
        val base = "/api/competitors/${segment(organization)}/items"
        if (competitor.id.isEmpty()) return Competitor.fromJson(call("POST", base, body = competitor.body).obj("competitor"))
        return Competitor.fromJson(call("PATCH", base + "/" + item(competitor.id), body = competitor.body).obj("competitor"))
    }

    fun markCompetitorReviewed(organization: String, competitor: Competitor, nowMs: Long): Competitor =
        Competitor.fromJson(
            call(
                "PATCH", "/api/competitors/${segment(organization)}/items/${item(competitor.id)}",
                body = mapOf("lastReviewedAt" to nowMs),
            ).obj("competitor"),
        )

    fun deleteCompetitor(organization: String, id: String) {
        call("DELETE", "/api/competitors/${segment(organization)}/items/${item(id)}", body = emptyMap())
    }

    fun myAds(): AdsAccount = AdsAccount.fromJson(call("GET", "/api/ads/mine"))

    fun adAction(adId: String, action: String): OwnedAd {
        if (action !in listOf("submit", "pause", "resume", "withdraw")) throw PackagesException("invalid_action")
        return OwnedAd.fromJson(call("POST", "/api/ads/${item(adId)}/$action", body = emptyMap()).obj("ad"))
    }

    fun adReviewQueue(status: String = "pending"): AdReviewQueue = AdReviewQueue.fromJson(call("GET", "/api/ads/review", listOf("status" to status)))

    fun reviewAd(ad: OwnedAd, decision: AdReviewDecision) {
        if (!decision.isComplete) throw PackagesException("reason_required")
        call("POST", "/api/ads/${item(ad.id)}/review", body = decision.body(ad.contentHash))
    }

    fun ledger(after: LedgerCursor? = null): LedgerPage =
        LedgerPage.fromJson(call("GET", "/api/ledger", listOf("limit" to "40") + (after?.query ?: emptyList())))

    fun rewardEstimates(repository: String? = null): List<RewardEstimate> {
        val query = if (repository != null) listOf("ownerScope" to "1", "repository" to repository.lowercase()) else emptyList()
        return call("GET", "/api/rewards/estimates", query).maps("rewards").map(RewardEstimate::fromJson).filter { it.rewardId.isNotEmpty() }
    }

    fun walletBalance(address: String, refresh: Boolean = false): WalletBalance {
        if (!SolanaAddress.isValid(address)) throw PackagesException("bad_address")
        val query = mutableListOf("address" to address)
        if (refresh) query.add("refresh" to "1")
        return WalletBalance.fromJson(address, call("GET", "/api/world/wallet", query))
    }

    fun payoutAddress(account: String): String {
        val name = account.trim().lowercase()
        if (!PackagesText.isOrgName(name)) throw PackagesException("no_such_account")
        val address = call("GET", "/api/accounts/$name").str("solana")
        return if (SolanaAddress.isValid(address)) address else ""
    }
}

@Suppress("UNCHECKED_CAST")
internal fun Any?.asMap(): Map<String, Any?> = this as? Map<String, Any?> ?: emptyMap()

internal fun controlErrorCode(error: Throwable): String? = when (error) {
    is PackagesException -> error.code
    is LocalControlException -> error.code
    else -> null
}
