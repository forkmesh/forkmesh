package com.forkmesh.nativeapp.market

import com.forkmesh.nativeapp.json.JsonMap
import com.forkmesh.nativeapp.json.bool
import com.forkmesh.nativeapp.json.double
import com.forkmesh.nativeapp.json.int
import com.forkmesh.nativeapp.json.long
import com.forkmesh.nativeapp.json.maps
import com.forkmesh.nativeapp.json.obj
import com.forkmesh.nativeapp.json.str
import java.time.LocalDate
import java.time.format.DateTimeFormatter
import java.util.Locale
import kotlin.math.roundToLong

data class MarketingAppStoreApp(val id: String = "", val name: String = "", val bundleId: String = "") {
    companion object {
        fun fromJson(json: JsonMap) = MarketingAppStoreApp(json.str("id"), json.str("name"), json.str("bundleId"))
    }
}

data class MarketingAppStoreLookup(
    val appId: String = "",
    val country: String = "",
    val name: String = "",
    val seller: String = "",
    val version: String = "",
    val releasedAt: Long = 0,
    val firstReleasedAt: Long = 0,
    val genre: String = "",
    val price: String = "",
    val minimumOs: String = "",
    val rating: Double = 0.0,
    val ratings: Long = 0,
    val versionRating: Double = 0.0,
    val versionRatings: Long = 0,
    val url: String = "",
    val lookedUpAt: Long = 0,
) {
    val isEmpty: Boolean get() = appId.isEmpty() && name.isEmpty() && version.isEmpty()

    val ratingText: String get() = if (rating > 0) MarketingAppStore.stars(rating) else "None yet"

    companion object {
        fun fromJson(json: JsonMap) = MarketingAppStoreLookup(
            json.str("appId"), json.str("country"), json.str("name"), json.str("seller"), json.str("version"), json.long("releasedAt"),
            json.long("firstReleasedAt"), json.str("genre"), json.str("price"), json.str("minimumOs"), json.double("rating").coerceIn(0.0, 5.0),
            json.long("ratings").coerceAtLeast(0), json.double("versionRating").coerceIn(0.0, 5.0), json.long("versionRatings").coerceAtLeast(0),
            json.str("url").takeIf { it.startsWith("https://") }.orEmpty(), json.long("lookedUpAt"),
        )
    }
}

data class MarketingAppStoreCounts(val downloads: Long = 0, val redownloads: Long = 0, val updates: Long = 0) {
    companion object {
        fun fromJson(json: JsonMap) = MarketingAppStoreCounts(
            json.long("downloads").coerceAtLeast(0), json.long("redownloads").coerceAtLeast(0), json.long("updates").coerceAtLeast(0),
        )
    }
}

data class MarketingAppStoreDay(val day: String, val counts: MarketingAppStoreCounts = MarketingAppStoreCounts()) {
    val downloads: Long get() = counts.downloads
    val redownloads: Long get() = counts.redownloads
    val updates: Long get() = counts.updates

    companion object {
        fun fromJson(json: JsonMap) = MarketingAppStoreDay(json.str("day"), MarketingAppStoreCounts.fromJson(json))
    }
}

data class MarketingAppStoreVersion(val version: String, val build: String, val status: AppStoreState, val createdAt: Long) {
    val title: String get() = if (build.isEmpty()) version else "$version ($build)"

    companion object {
        fun fromJson(json: JsonMap) = MarketingAppStoreVersion(json.str("version"), json.str("build"), AppStoreState.fromJson(json["status"]), json.long("createdAt"))
    }
}

data class MarketingAppStoreReview(val createdAt: Long, val rating: Int, val title: String, val territory: String, val body: String) {
    val starsText: String get() = "★".repeat(rating) + "☆".repeat(5 - rating)

    companion object {
        fun fromJson(json: JsonMap) = MarketingAppStoreReview(
            json.long("createdAt"), json.int("rating").coerceIn(0, 5), json.str("title"), json.str("territory"), json.str("body"),
        )
    }
}

data class MarketingAppStoreShare(val label: String, val downloads: Long)

data class MarketingAppStoreWindow(
    val days: Int,
    val from: String,
    val to: String,
    val counts: MarketingAppStoreCounts,
    val previous: MarketingAppStoreCounts,
    val comparable: Boolean,
    val territories: List<MarketingAppStoreShare>,
    val territoryCount: Int,
    val devices: List<MarketingAppStoreShare>,
) {
    val downloadsChange: Double? get() = MarketingAppStore.change(counts.downloads, previous.downloads, comparable)

    val changeText: String get() = downloadsChange?.let(MarketingAppStore::changeText).orEmpty()

    companion object {
        fun fromJson(json: JsonMap) = MarketingAppStoreWindow(
            json.int("days"), json.str("from"), json.str("to"), MarketingAppStoreCounts.fromJson(json), MarketingAppStoreCounts.fromJson(json.obj("previous")),
            json.bool("comparable"),
            json.maps("territories").map { MarketingAppStoreShare(it.str("code"), it.long("downloads")) }.filter { it.label.isNotEmpty() },
            json.int("territoryCount"),
            json.maps("devices").map { MarketingAppStoreShare(it.str("name"), it.long("downloads")) }.filter { it.label.isNotEmpty() },
        )
    }
}

data class MarketingAppStore(
    val app: MarketingAppStoreApp = MarketingAppStoreApp(),
    val lookup: MarketingAppStoreLookup = MarketingAppStoreLookup(),
    val versions: List<MarketingAppStoreVersion> = emptyList(),
    val reviews: List<MarketingAppStoreReview> = emptyList(),
    val publishedAt: Long = 0,
    val publishedBy: String = "",
    val fresh: Boolean = false,
    val firstDay: String = "",
    val lastDay: String = "",
    val days: List<MarketingAppStoreDay> = emptyList(),
    val total: MarketingAppStoreCounts = MarketingAppStoreCounts(),
    val bestDay: MarketingAppStoreDay? = null,
    val windows: List<MarketingAppStoreWindow> = emptyList(),
    val canPublish: Boolean = false,
) {
    val isEmpty: Boolean get() = lastDay.isEmpty()

    val name: String get() = app.name.ifEmpty { lookup.name }.ifEmpty { "App Store" }

    fun window(days: Int): MarketingAppStoreWindow? = windows.firstOrNull { it.days == days }

    fun chartDays(count: Int): List<MarketingAppStoreDay> {
        val last = parse(lastDay) ?: return emptyList()
        if (count <= 0) return emptyList()
        val first = parse(firstDay) ?: parse(days.firstOrNull()?.day) ?: last
        var start = last.minusDays(count - 1L)
        if (first.isAfter(start)) start = first
        if (start.isAfter(last)) return emptyList()
        val byDay = days.associateBy { it.day }
        return generateSequence(start) { it.plusDays(1) }.takeWhile { !it.isAfter(last) }
            .map { date -> date.toString().let { key -> byDay[key] ?: MarketingAppStoreDay(key) } }.toList()
    }

    fun sentText(relative: String): String =
        if (publishedAt <= 0) "" else "Sent from App Store Connect $relative" + if (publishedBy.isEmpty()) "" else " by $publishedBy"

    companion object {
        val RANGES = listOf(30, 90, 365)
        const val CHANNEL = "app-store"
        const val EMPTY_TEXT = "No downloads yet. Run native/ios/tools/asc_store_report.py --publish on the computer that holds the App Store Connect key; it sends each day's downloads here."

        fun fromJson(reply: JsonMap): MarketingAppStore {
            val json = reply.obj("appStore")
            val best = json.obj("bestDay").takeIf { it.str("day").isNotEmpty() }?.let(MarketingAppStoreDay::fromJson)
            return MarketingAppStore(
                MarketingAppStoreApp.fromJson(json.obj("app")), MarketingAppStoreLookup.fromJson(json.obj("lookup")),
                json.maps("versions").map(MarketingAppStoreVersion::fromJson).filter { it.version.isNotEmpty() },
                json.maps("reviews").map(MarketingAppStoreReview::fromJson).filter { it.rating > 0 || it.title.isNotEmpty() || it.body.isNotEmpty() },
                json.long("publishedAt"), json.str("publishedBy"), json.bool("fresh"), json.str("firstDay"), json.str("lastDay"),
                json.maps("days").map(MarketingAppStoreDay::fromJson).filter { parse(it.day) != null }.sortedBy { it.day },
                MarketingAppStoreCounts.fromJson(json.obj("total")), best,
                json.maps("windows").map(MarketingAppStoreWindow::fromJson).filter { it.days > 0 }, reply.bool("canPublish"),
            )
        }

        fun change(current: Long, previous: Long, comparable: Boolean): Double? {
            if (!comparable || previous <= 0) return null
            return (current - previous) * 100.0 / previous
        }

        fun changeText(change: Double): String {
            val rounded = change.roundToLong()
            return (if (rounded > 0) "+" else "") + rounded + "%"
        }

        fun axisMax(value: Long): Long {
            if (value <= 0) return 1
            var scale = 1L
            while (true) {
                for (step in longArrayOf(1, 2, 5)) if (step * scale >= value) return step * scale
                scale *= 10
            }
        }

        fun stars(rating: Double): String = String.format(Locale.ROOT, "%.1f★", rating)

        fun dayLabel(day: String): String = parse(day)?.format(DateTimeFormatter.ofPattern("d MMM", Locale.ENGLISH)) ?: day

        fun dayLabelWithYear(day: String): String = parse(day)?.format(DateTimeFormatter.ofPattern("d MMM yyyy", Locale.ENGLISH)) ?: day

        fun countryName(code: String): String {
            if (code.length != 2) return code
            val name = runCatching { Locale.Builder().setRegion(code.uppercase(Locale.ROOT)).build().getDisplayCountry(Locale.ENGLISH) }.getOrDefault("")
            return if (name.isEmpty() || name.equals(code, ignoreCase = true)) code else name
        }

        private fun parse(day: String?): LocalDate? = day?.takeIf { it.length == 10 }?.let { runCatching { LocalDate.parse(it) }.getOrNull() }
    }
}
