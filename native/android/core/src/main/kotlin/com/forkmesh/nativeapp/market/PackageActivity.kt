package com.forkmesh.nativeapp.market

import com.forkmesh.nativeapp.json.JsonMap
import com.forkmesh.nativeapp.json.bool
import com.forkmesh.nativeapp.json.int
import com.forkmesh.nativeapp.json.long
import com.forkmesh.nativeapp.json.maps
import com.forkmesh.nativeapp.json.obj
import com.forkmesh.nativeapp.json.str
import com.forkmesh.nativeapp.navigation.Route

enum class ActivityKind(val wire: String) { VIEW("view"), OPEN("open") }

data class PackageActivityTotals(val views: Int = 0, val opens: Int = 0, val people: Int = 0) {
    val viewsLabel: String get() = if (people > 0) "$views · $people ${if (people == 1) "person" else "people"}" else views.toString()

    companion object {
        fun fromJson(json: JsonMap) = PackageActivityTotals(maxOf(0, json.int("views")), maxOf(0, json.int("opens")), maxOf(0, json.int("people")))

        fun youLabel(viewer: ReviewViewer): String =
            "Opened ${viewer.opens} ${if (viewer.opens == 1) "time" else "times"} · viewed ${viewer.views} ${if (viewer.views == 1) "time" else "times"}"
    }
}

data class PackageActivityPerson(val name: String, val views: Int, val opens: Int, val firstAt: Long, val lastAt: Long) {
    val label: String get() = "$opens ${if (opens == 1) "open" else "opens"} · $views ${if (views == 1) "view" else "views"}"

    companion object {
        fun fromJson(json: JsonMap) = PackageActivityPerson(
            json.str("name"), maxOf(0, json.int("views")), maxOf(0, json.int("opens")), json.long("firstAt"), json.long("lastAt"),
        )
    }
}

data class PackageActivityPage(
    val totals: PackageActivityTotals,
    val page: Int,
    val limit: Int,
    val hasMore: Boolean,
    val people: List<PackageActivityPerson>,
) {
    companion object {
        fun fromJson(json: JsonMap) = PackageActivityPage(
            PackageActivityTotals.fromJson(json.obj("totals")),
            maxOf(1, json.int("page", 1)),
            maxOf(1, json.int("limit", 50)),
            json.bool("hasMore"),
            json.maps("people").map(PackageActivityPerson::fromJson).filter { it.name.isNotEmpty() }
                .sortedWith(compareByDescending<PackageActivityPerson> { it.opens }.thenByDescending { it.views }),
        )
    }
}

class ActivityThrottle(private val windowMs: Long = WINDOW_MS, private val clock: () -> Long = System::currentTimeMillis) {
    private val last = HashMap<String, Long>()

    @Synchronized
    fun admit(packageId: String, kind: ActivityKind): Boolean {
        val now = clock()
        val key = packageId + "/" + kind.wire
        val previous = last[key]
        if (previous != null && now >= previous && now - previous < windowMs) return false
        last[key] = now
        return true
    }

    companion object {
        const val WINDOW_MS = 60_000L
    }
}

class PackageActivityReporter(private val client: () -> PackagesClient?, private val throttle: ActivityThrottle = ActivityThrottle()) {
    fun report(packageId: String, kind: ActivityKind, market: String = "forkmesh"): Boolean {
        if (!PackagesText.isPackageId(packageId)) return false
        val client = client() ?: return false
        if (!throttle.admit(packageId, kind)) return false
        return runCatching { client.reportActivity(packageId, kind, market) }.isSuccess
    }

    fun opened(kind: Route.Kind): Boolean {
        val packageId = PackageGate.ROUTE_KINDS[kind] ?: return false
        return report(packageId, ActivityKind.OPEN)
    }
}
