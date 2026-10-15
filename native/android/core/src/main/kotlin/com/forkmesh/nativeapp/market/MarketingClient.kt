package com.forkmesh.nativeapp.market

import com.forkmesh.nativeapp.json.JsonMap
import com.forkmesh.nativeapp.json.int
import com.forkmesh.nativeapp.json.long
import com.forkmesh.nativeapp.json.obj
import com.forkmesh.nativeapp.json.str
import java.time.ZoneId

private fun PackagesClient.marketingPath(organization: String, suffix: String = ""): String =
    "/api/marketing/" + segment(organization) + if (suffix.isEmpty()) "" else "/$suffix"

fun PackagesClient.saveMarketing(
    draft: MarketingDraft, organization: String, overview: MarketingOverview, now: Long, zone: ZoneId = ZoneId.systemDefault(),
): JsonMap {
    val body = try {
        draft.body(overview, now, zone)
    } catch (error: MarketingFormException) {
        throw PackagesException("form_invalid", fields = mapOf("message" to error.message.orEmpty()))
    }
    return sendMarketing(draft.kind, draft.id, organization, body)
}

fun PackagesClient.sendMarketing(kind: MarketingKind, id: String, organization: String, body: Map<String, Any?>): JsonMap = when {
    kind.isSettings -> call("PUT", marketingPath(organization, "settings"), body = body)
    kind in setOf(MarketingKind.X_CONNECTION, MarketingKind.X_POST, MarketingKind.LINK, MarketingKind.METRIC, MarketingKind.FUNNEL_WEEK) ->
        call("POST", marketingPath(organization, kind.resource), body = body)
    kind == MarketingKind.AD_RESULTS -> call("PATCH", marketingPath(organization, "ads/" + item(id)), body = body)
    id.isEmpty() -> call("POST", marketingPath(organization, kind.resource), body = body)
    else -> call("PATCH", marketingPath(organization, kind.resource + "/" + item(id)), body = body)
}

fun PackagesClient.setMarketingStatus(kind: MarketingKind, id: String, status: String, organization: String, extra: Map<String, Any?> = emptyMap()): JsonMap {
    if (status !in kind.statuses) throw PackagesException("invalid_status")
    return sendMarketing(kind, id, organization, linkedMapOf<String, Any?>("status" to status) + extra)
}

fun PackagesClient.deleteMarketing(kind: MarketingKind, id: String, organization: String) {
    if (!kind.deletable) throw PackagesException("method_not_allowed")
    call("DELETE", marketingPath(organization, kind.resource + "/" + item(id)), body = emptyMap())
}

fun PackagesClient.addMarketingChannels(ids: List<String>, organization: String, overview: MarketingOverview) {
    call("PUT", marketingPath(organization, "settings"), body = MarketingForms.channelsBody(overview, adding = ids))
}

fun PackagesClient.refreshMarketingChannel(channel: String, organization: String): Pair<MarketingOverview, String> {
    val reply = call("POST", marketingPath(organization, "refresh"), body = mapOf("channel" to channel))
    val refresh = reply.obj("refresh")
    val items = refresh.int("items")
    var message = "$items post${if (items == 1) "" else "s"} read · ${refresh.int("imported")} new"
    val metrics = refresh.obj("metrics")
    if (metrics.containsKey("followers") && channel != "email") message += " · ${MarketingText.count(metrics.long("followers"))} followers"
    return MarketingOverview.fromJson(reply) to message
}

fun PackagesClient.disconnectX(organization: String) {
    call("DELETE", marketingPath(organization, "x-connection"), body = emptyMap())
}

fun PackagesClient.marketingAdvice(organization: String, campaignId: String = "", question: String = ""): MarketingAdvice {
    val body = linkedMapOf<String, Any?>("question" to question.take(600))
    if (campaignId.isNotEmpty()) body["campaignId"] = item(campaignId)
    return MarketingAdvice.fromJson(call("POST", marketingPath(organization, "assistant"), body = body))
}

fun PackagesClient.marketingReport(organization: String): MarketingReport {
    val reply = call("GET", marketingPath(organization, "report"))
    return MarketingReport(reply.long("generatedAt"), reply.str("markdown"))
}

fun PackagesClient.marketingPerformance(organization: String, days: Int, project: String = "", now: Long): MarketingPerformance {
    val query = mutableListOf("from" to (now - maxOf(1, days) * 86_400_000L).toString(), "to" to now.toString())
    if (PackagesText.isHexId(project)) query.add("project" to project)
    return MarketingPerformance.fromJson(call("GET", marketingPath(organization, "performance"), query).obj("performance"))
}

fun PackagesClient.buildMarketingLink(draft: MarketingDraft, organization: String, overview: MarketingOverview, now: Long): MarketingLink =
    MarketingLink.fromJson(saveMarketing(draft, organization, overview, now))

fun PackagesClient.importMarketingKeywords(rows: List<Map<String, Any?>>, organization: String, source: String, observedAt: Long): Triple<Int, Int, Int> {
    if (rows.isEmpty()) throw PackagesException("invalid_keyword_rows")
    val reply = call(
        "POST", marketingPath(organization, "keywords"),
        body = linkedMapOf("rows" to rows.take(500), "source" to source.take(60), "observedAt" to observedAt),
    )
    return Triple(reply.int("created"), reply.int("updated"), reply.int("skipped"))
}

fun PackagesClient.marketingAppStore(organization: String): MarketingAppStore =
    MarketingAppStore.fromJson(call("GET", marketingPath(organization, "app-store")))

fun PackagesClient.readyPost(organization: String, channel: String = "x"): MarketingReadyPosts =
    MarketingReadyPosts.fromJson(call("GET", marketingPath(organization, "ready-post"), listOf("channel" to channel)).obj("readyPost"), channel)

fun PackagesClient.closeReadyPost(organization: String, id: String, status: String, channel: String = "x", url: String = ""): MarketingReadyPosts {
    if (status !in MarketingReadyPosts.CLOSE_STATUSES) throw PackagesException("invalid_ready_post_status")
    val body = linkedMapOf<String, Any?>("channel" to channel, "id" to item(id), "status" to status)
    if (url.isNotEmpty()) body["url"] = url
    return MarketingReadyPosts.fromJson(call("PATCH", marketingPath(organization, "ready-post"), body = body).obj("readyPost"), channel)
}
