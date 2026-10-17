package com.forkmesh.nativeapp.market

import com.forkmesh.nativeapp.json.JsonMap
import com.forkmesh.nativeapp.json.asJsonMap
import com.forkmesh.nativeapp.json.bool
import com.forkmesh.nativeapp.json.int
import com.forkmesh.nativeapp.json.jsonText
import com.forkmesh.nativeapp.json.long
import com.forkmesh.nativeapp.json.maps
import com.forkmesh.nativeapp.json.obj
import com.forkmesh.nativeapp.json.str
import com.forkmesh.nativeapp.json.strings
import java.time.Instant
import java.time.OffsetDateTime
import java.util.Base64

enum class AppStoreTone(val wire: String) {
    ACTION("action"), PENDING("pending"), GOOD("good"), NEUTRAL("neutral");

    companion object {
        fun of(raw: String?): AppStoreTone = entries.firstOrNull { it.wire == raw?.trim()?.lowercase() } ?: NEUTRAL
    }
}

data class AppStoreState(val state: String = "", val label: String = "Unknown", val tone: AppStoreTone = AppStoreTone.NEUTRAL) {
    companion object {
        fun fromJson(value: Any?): AppStoreState {
            val json = value.asJsonMap()
            val state = json.str("state").trim()
            return AppStoreState(state, json.str("label").trim().ifEmpty { state.ifEmpty { "Unknown" } }, AppStoreTone.of(json.str("tone")))
        }
    }
}

data class AppStoreVersion(
    val version: String = "",
    val build: String = "",
    val releaseType: String = "",
    val createdAt: String = "",
    val status: AppStoreState = AppStoreState(),
) {
    companion object {
        fun fromJson(json: JsonMap) = AppStoreVersion(
            json.str("version"), json.str("build"), json.str("releaseType"), json.str("createdAt"), AppStoreState.fromJson(json["status"]),
        )
    }
}

data class AppStoreSubmissionItem(val name: String = "", val status: AppStoreState = AppStoreState())

data class AppStoreSubmission(
    val id: String = "",
    val submittedAt: String = "",
    val status: AppStoreState = AppStoreState(),
    val items: List<AppStoreSubmissionItem> = emptyList(),
) {
    companion object {
        fun fromJson(json: JsonMap) = AppStoreSubmission(
            json.str("id"), json.str("submittedAt"), AppStoreState.fromJson(json["status"]),
            json.maps("items").map { AppStoreSubmissionItem(it.str("name"), AppStoreState.fromJson(it["status"])) },
        )
    }
}

data class AppStoreCounts(val downloads: Long = 0, val redownloads: Long = 0, val updates: Long = 0) {
    companion object {
        fun fromJson(json: JsonMap) =
            AppStoreCounts(json.long("downloads").coerceAtLeast(0), json.long("redownloads").coerceAtLeast(0), json.long("updates").coerceAtLeast(0))
    }
}

data class AppStoreSpan(val days: Int = 0, val counts: AppStoreCounts = AppStoreCounts()) {
    val label: String get() = if (days == 1) "Last day" else "Last $days days"
}

data class AppStoreDownloads(
    val available: Boolean = false,
    val note: String = "",
    val since: String = "",
    val total: AppStoreCounts = AppStoreCounts(),
    val recent: List<AppStoreSpan> = emptyList(),
) {
    val totalLabel: String get() = if (since.isEmpty()) "Total" else "Total since $since"

    companion object {
        fun fromJson(json: JsonMap) = AppStoreDownloads(
            json.bool("available"), json.str("note"), json.str("since"), AppStoreCounts.fromJson(json.obj("total")),
            json.maps("recent").map { AppStoreSpan(it.int("days"), AppStoreCounts.fromJson(it)) },
        )
    }
}

data class AppStoreBuild(
    val version: String = "",
    val build: String = "",
    val uploadedAt: String = "",
    val expired: Boolean = false,
    val processing: AppStoreState = AppStoreState(),
    val internal: AppStoreState = AppStoreState(),
    val external: AppStoreState = AppStoreState(),
    val betaReview: AppStoreState = AppStoreState(),
    val groups: List<String> = emptyList(),
) {
    companion object {
        fun fromJson(json: JsonMap) = AppStoreBuild(
            json.str("version"), json.str("build"), json.str("uploadedAt"), json.bool("expired"),
            AppStoreState.fromJson(json["processing"]), AppStoreState.fromJson(json["internal"]),
            AppStoreState.fromJson(json["external"]), AppStoreState.fromJson(json["betaReview"]),
            json.strings("groups").filter { it.isNotBlank() },
        )
    }
}

data class AppStoreGroup(
    val name: String = "",
    val internal: Boolean = false,
    val testers: Int = 0,
    val allBuilds: Boolean = false,
    val publicLinkEnabled: Boolean = false,
    val publicLink: String = "",
    val publicLinkLimit: Int = 0,
    val feedbackEnabled: Boolean = false,
) {
    val openableLink: String? get() = publicLink.takeIf { publicLinkEnabled && it.startsWith("https://") }

    companion object {
        fun fromJson(json: JsonMap) = AppStoreGroup(
            json.str("name"), json.bool("internal"), json.int("testers").coerceAtLeast(0), json.bool("allBuilds"),
            json.bool("publicLinkEnabled"), json.str("publicLink"), json.int("publicLinkLimit").coerceAtLeast(0), json.bool("feedbackEnabled"),
        )
    }
}

data class AppStoreScreenshot(val id: String = "", val width: Int = 0, val height: Int = 0) {
    val aspectRatio: Float? get() = if (width > 0 && height > 0) width.toFloat() / height else null

    companion object {
        const val MAX_PER_FEEDBACK = 4
        private val ID = Regex("^[0-9a-f]{24}$")

        fun isValidId(id: String): Boolean = ID.matches(id)

        fun fromJson(json: JsonMap): AppStoreScreenshot? {
            val id = json.str("id").trim()
            if (!isValidId(id)) return null
            return AppStoreScreenshot(id, json.int("width").coerceIn(0, 20000), json.int("height").coerceIn(0, 20000))
        }
    }
}

data class AppStoreFeedback(
    val kind: String = "screenshot",
    val createdAt: String = "",
    val from: String = "",
    val device: String = "",
    val comment: String = "",
    val screenshots: List<AppStoreScreenshot> = emptyList(),
) {
    val crash: Boolean get() = kind == "crash"

    companion object {
        fun fromJson(json: JsonMap) = AppStoreFeedback(
            json.str("kind").ifEmpty { "screenshot" }, json.str("createdAt"), json.str("from").ifEmpty { "anonymous" },
            json.str("device"), json.str("comment"),
            json.maps("screenshots").mapNotNull(AppStoreScreenshot::fromJson).take(AppStoreScreenshot.MAX_PER_FEEDBACK),
        )
    }
}

data class AppStoreReview(
    val createdAt: String = "",
    val rating: Int = 0,
    val title: String = "",
    val territory: String = "",
    val body: String = "",
) {
    companion object {
        fun fromJson(json: JsonMap) = AppStoreReview(
            json.str("createdAt"), json.int("rating").coerceIn(0, 5), json.str("title"), json.str("territory"), json.str("body"),
        )
    }
}

data class AppStoreSnapshot(
    val format: String = "",
    val generatedAt: Long = 0,
    val appId: String = "",
    val bundleId: String = "",
    val attention: List<String> = emptyList(),
    val versions: List<AppStoreVersion> = emptyList(),
    val submissions: List<AppStoreSubmission> = emptyList(),
    val downloads: AppStoreDownloads = AppStoreDownloads(),
    val builds: List<AppStoreBuild> = emptyList(),
    val groups: List<AppStoreGroup> = emptyList(),
    val feedback: List<AppStoreFeedback> = emptyList(),
    val reviews: List<AppStoreReview> = emptyList(),
    val unavailable: Map<String, String> = emptyMap(),
) {
    val orderedGroups: List<AppStoreGroup> get() = groups.sortedBy { if (it.internal) 0 else 1 }

    fun unavailable(section: String): String = unavailable[section].orEmpty()

    companion object {
        fun fromJson(json: JsonMap): AppStoreSnapshot {
            val app = json.obj("app")
            return AppStoreSnapshot(
                format = json.str("format"),
                generatedAt = json.long("generatedAt"),
                appId = app.str("id"),
                bundleId = app.str("bundleId"),
                attention = json.strings("attention").map { it.trim() }.filter { it.isNotEmpty() },
                versions = json.maps("versions").map(AppStoreVersion::fromJson),
                submissions = json.maps("submissions").map(AppStoreSubmission::fromJson),
                downloads = AppStoreDownloads.fromJson(json.obj("downloads")),
                builds = json.maps("builds").map(AppStoreBuild::fromJson),
                groups = json.maps("groups").map(AppStoreGroup::fromJson),
                feedback = json.maps("feedback").map(AppStoreFeedback::fromJson),
                reviews = json.maps("reviews").map(AppStoreReview::fromJson),
                unavailable = json.obj("unavailable").mapValues { jsonText(it.value).trim() }.filterValues { it.isNotEmpty() },
            )
        }
    }
}

data class AppStoreStatusReply(
    val actor: String = "",
    val published: Boolean = false,
    val publishedAt: Long = 0,
    val snapshot: AppStoreSnapshot? = null,
) {
    companion object {
        fun fromJson(json: JsonMap): AppStoreStatusReply {
            val snapshot = (json["snapshot"] as? Map<*, *>)?.let { AppStoreSnapshot.fromJson(it.asJsonMap()) }
            return AppStoreStatusReply(json.str("actor"), snapshot != null, json.long("publishedAt"), snapshot)
        }
    }
}

object AppStoreStatus {
    const val PACKAGE_ID = "app-store-status"
    const val PATH = "/dashboard/app-store-status"
    const val API = "/api/app-store-status"
    const val FORMAT = "forkmesh-app-store-status-v1"

    const val VERSIONS = "versions"
    const val SUBMISSIONS = "submissions"
    const val DOWNLOADS = "downloads"
    const val BUILDS = "builds"
    const val GROUPS = "groups"
    const val FEEDBACK = "feedback"
    const val REVIEWS = "reviews"

    fun load(client: PackagesClient): AppStoreStatusReply = AppStoreStatusReply.fromJson(client.call("GET", API))

    const val SCREENSHOT_MISSING = "This screenshot has not been uploaded yet. Run the iOS App Store status shortcut on your desktop again."
    private val MISSING_CODES = setOf("screenshot_not_found", "screenshot_not_in_snapshot", "not_found", "http_404")
    private val JPEG = byteArrayOf(0xFF.toByte(), 0xD8.toByte(), 0xFF.toByte())
    private val PNG = byteArrayOf(0x89.toByte(), 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A)

    fun screenshotPath(id: String): String = "$API/screenshots/$id"

    fun screenshotTitle(index: Int, count: Int): String = if (count > 1) "Screenshot ${index + 1} of $count" else "Screenshot"

    fun screenshotBytes(json: JsonMap): ByteArray? {
        val bytes = runCatching { Base64.getDecoder().decode(json.str("data")) }.getOrNull() ?: return null
        val magic = when (json.str("mime")) {
            "image/jpeg" -> JPEG
            "image/png" -> PNG
            else -> return null
        }
        if (bytes.size < magic.size || !magic.indices.all { bytes[it] == magic[it] }) return null
        return bytes
    }

    fun loadScreenshot(client: PackagesClient, shot: AppStoreScreenshot): ByteArray {
        if (!AppStoreScreenshot.isValidId(shot.id)) throw PackagesException("not_found", 404)
        val reply = try {
            client.call("GET", screenshotPath(shot.id))
        } catch (error: PackagesException) {
            if (error.code in MISSING_CODES) throw ScreenshotMissingException()
            throw error
        }
        return screenshotBytes(reply) ?: throw PackagesException("invalid_image")
    }

    class ScreenshotMissingException : RuntimeException(SCREENSHOT_MISSING)

    fun epochMs(stamp: String?): Long? {
        val text = stamp?.trim().orEmpty()
        if (text.isEmpty()) return null
        return runCatching { OffsetDateTime.parse(text).toInstant().toEpochMilli() }.getOrNull()
            ?: runCatching { Instant.parse(text).toEpochMilli() }.getOrNull()
    }

    fun stars(rating: Int): String {
        val filled = rating.coerceIn(0, 5)
        return "★".repeat(filled) + "☆".repeat(5 - filled)
    }

    fun buildName(version: String, build: String): String = when {
        version.isNotEmpty() && build.isNotEmpty() -> "$version ($build)"
        version.isNotEmpty() -> version
        build.isNotEmpty() -> "Build $build"
        else -> "Unknown"
    }
}
