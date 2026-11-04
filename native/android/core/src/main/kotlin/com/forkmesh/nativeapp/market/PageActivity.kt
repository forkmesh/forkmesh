package com.forkmesh.nativeapp.market

import com.forkmesh.nativeapp.format.relativeTimeFrom
import com.forkmesh.nativeapp.json.JsonMap
import com.forkmesh.nativeapp.json.int
import com.forkmesh.nativeapp.json.long
import com.forkmesh.nativeapp.json.maps
import com.forkmesh.nativeapp.json.str

data class PageVisit(
    val kind: String = "",
    val type: String = "",
    val detail: String = "",
    val label: String = "",
    val visitor: String = "",
    val at: Long = 0,
) {
    fun whenLabel(nowMs: Long): String = if (at > 0) relativeTimeFrom(at, nowMs) else ""

    companion object {
        fun fromJson(json: JsonMap) = PageVisit(
            json.str("kind"), json.str("type"), json.str("detail"), json.str("label"), json.str("visitor"), maxOf(0L, json.long("at")),
        )
    }
}

data class PageActivity(
    val days: Int = 30,
    val views: Int = 0,
    val visitors: Int = 0,
    val interactions: Int = 0,
    val starts: Int = 0,
    val submits: Int = 0,
    val leaves: Int = 0,
    val lastActivityAt: Long = 0,
    val recent: List<PageVisit> = emptyList(),
) {
    val isEmpty: Boolean get() = views == 0 && interactions == 0

    val period: String get() = if (days == 1) "day" else "$days days"

    val newest: List<PageVisit> get() = recent.take(SHOWN_VISITS)

    fun summary(nowMs: Long): String {
        if (isEmpty) return "No visits in the last $period"
        val parts = mutableListOf(
            SurveyFormat.plural(views, "view"), SurveyFormat.plural(visitors, "visitor"), "$starts started", "$submits sent",
        )
        if (lastActivityAt > 0) parts.add("last activity " + relativeTimeFrom(lastActivityAt, nowMs))
        return "Last $period: " + parts.joinToString(" · ")
    }

    companion object {
        const val SHOWN_VISITS = 5

        fun fromJson(json: JsonMap) = PageActivity(
            days = maxOf(1, json.int("days", 30)),
            views = maxOf(0, json.int("views")),
            visitors = maxOf(0, json.int("visitors")),
            interactions = maxOf(0, json.int("interactions")),
            starts = maxOf(0, json.int("starts")),
            submits = maxOf(0, json.int("submits")),
            leaves = maxOf(0, json.int("leaves")),
            lastActivityAt = maxOf(0L, json.long("lastActivityAt")),
            recent = json.maps("recent").map(PageVisit::fromJson).filter { it.label.isNotEmpty() },
        )
    }
}
