package com.forkmesh.nativeapp.insights

import com.forkmesh.nativeapp.crypto.Ed25519
import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.json.asJsonMap
import com.forkmesh.nativeapp.json.int
import com.forkmesh.nativeapp.json.long
import com.forkmesh.nativeapp.json.maps
import com.forkmesh.nativeapp.json.obj
import com.forkmesh.nativeapp.json.str
import com.forkmesh.nativeapp.market.AppStoreStatus
import com.forkmesh.nativeapp.market.AppStoreStatusReply
import java.net.URLEncoder
import java.util.Base64

data class ReleasePlatform(
    val id: String,
    val title: String,
    val kind: String,
    val assets: List<Pair<String, String>>,
    val required: Boolean,
)

data class ReleaseAsset(val name: String, val sha256: String, val size: Long, val build: Int) {
    val valid: Boolean get() = LiveReleases.SHA256.matches(sha256) && size > 0
}

data class SignedRelease(val tag: String, val assets: Map<String, ReleaseAsset>) {
    fun asset(name: String): ReleaseAsset? = assets[name]?.takeIf { it.valid }
}

data class PlatformBuild(val arch: String, val tag: String, val build: Int)

data class PlatformRelease(val platform: ReleasePlatform, val builds: List<PlatformBuild>) {
    val newestTag: String
        get() = builds.map { it.tag }.reduceOrNull { best, tag -> if (LiveReleases.isNewer(tag, best)) tag else best }.orEmpty()
}

data class LiveReleaseSummary(val latest: String, val newest: String, val platforms: List<PlatformRelease>) {
    val downloads: Int get() = platforms.sumOf { it.builds.size }
}

object LiveReleases {
    const val LATEST_ID = "release-latest"
    const val IOS_ID = "release-ios"
    const val REPO_BLOBS = "/api/repo/forkmesh/forkmesh/blobs"
    const val CHANNEL_ROOT = ".forkmesh/releases/"
    const val VERSION_FILE = "desktop/CMakeLists.txt"
    const val LOOKBACK = 6
    const val PUBLIC_KEY_SPKI = "MCowBQYDK2VwAyEAHBYOCW4qnyZkYAnEoqUrYxPiDRszjfJa+xJUeyQPUS0="
    val SHA256 = Regex("^[0-9a-f]{64}$")
    private val VERSION = Regex("^v?(\\d+)\\.(\\d+)\\.(\\d+)$")
    private val PROJECT_VERSION = Regex("project\\(\\s*ForkMesh\\s+VERSION\\s+(\\d+\\.\\d+\\.\\d+)")
    private val LIVE_STATES = setOf("READY_FOR_SALE", "READY_FOR_DISTRIBUTION")

    val publicKey: ByteArray by lazy { Base64.getDecoder().decode(PUBLIC_KEY_SPKI).takeLast(Ed25519.PUBLIC_KEY_BYTES).toByteArray() }

    val platforms: List<ReleasePlatform> = listOf(
        ReleasePlatform("release-android", "Android", "APK", listOf("forkmesh-android-universal.apk" to "Universal"), required = true),
        ReleasePlatform(
            "release-macos", "macOS", "DMG",
            listOf("forkmesh-macos-arm64.dmg" to "Apple silicon", "forkmesh-macos-x86_64.dmg" to "Intel"),
            required = false,
        ),
        ReleasePlatform("release-windows", "Windows", "Installer", listOf("forkmesh-windows-x86_64-setup.exe" to "x86_64"), required = false),
        ReleasePlatform("release-linux", "Linux", "AppImage", listOf("forkmesh-linux-x86_64.AppImage" to "x86_64"), required = true),
    )

    val homepageIds: Set<String> = setOf(LATEST_ID) + platforms.map { it.id }

    val catalog: List<StatsTileCatalogEntry> = listOf(
        StatsTileCatalogEntry(LATEST_ID, "Latest release", "Releases", "The newest signed ForkMesh release the homepage announces", "system_update"),
        StatsTileCatalogEntry("release-android", "Android version", "Releases", "The Android APK the homepage offers for download", "android"),
        StatsTileCatalogEntry("release-macos", "macOS version", "Releases", "The macOS DMGs the homepage offers for download", "laptop_mac"),
        StatsTileCatalogEntry("release-windows", "Windows version", "Releases", "The Windows installer the homepage offers for download", "desktop_windows"),
        StatsTileCatalogEntry("release-linux", "Linux version", "Releases", "The Linux AppImage the homepage offers for download", "terminal"),
        StatsTileCatalogEntry(IOS_ID, "iOS version", "Releases", "The iPhone version live on the App Store, from your App Store status", "phone_iphone"),
    )

    val defaults: List<StatsTilePlacement> = catalog.map { StatsTilePlacement(it.id) }

    fun versionParts(tag: String): List<Int>? = VERSION.find(tag.trim())?.groupValues?.drop(1)?.map { it.toInt() }

    fun isNewer(tag: String, than: String): Boolean {
        val a = versionParts(tag) ?: return false
        val b = versionParts(than) ?: return false
        for (index in 0 until 3) if (a[index] != b[index]) return a[index] > b[index]
        return false
    }

    fun projectVersion(text: String): String? = PROJECT_VERSION.find(text)?.groupValues?.get(1)

    fun manifestPaths(channel: String): List<String> = listOf("$CHANNEL_ROOT$channel/release.json", "$CHANNEL_ROOT$channel/release.json.sig")

    fun blobsPath(paths: List<String>): String =
        REPO_BLOBS + "?" + paths.joinToString("&") { "path=" + URLEncoder.encode(it, "UTF-8") } + "&ref=main"

    fun earlierChannels(tag: String): List<String> {
        val parts = versionParts(tag) ?: return emptyList()
        return (parts[2] - 1 downTo maxOf(0, parts[2] - LOOKBACK)).map { "candidate-${parts[0]}.${parts[1]}.$it" }
    }

    fun blobBytes(blob: Any?): ByteArray? {
        val json = blob as? Map<*, *> ?: return null
        val content = json["content"] as? String ?: return null
        if (json["encoding"] == "base64") return runCatching { Base64.getDecoder().decode(content.trim()) }.getOrNull()
        return content.toByteArray(Charsets.UTF_8)
    }

    fun verified(blobs: Map<String, Any?>, channel: String, key: ByteArray = publicKey): SignedRelease? {
        val (manifestPath, signaturePath) = manifestPaths(channel)
        val manifest = blobBytes(blobs[manifestPath]) ?: return null
        val signature = blobBytes(blobs[signaturePath]) ?: return null
        if (signature.size != Ed25519.SIGNATURE_BYTES || !Ed25519.verify(key, manifest, signature)) return null
        val json = runCatching { Json.parse(String(manifest, Charsets.UTF_8)) }.getOrNull().asJsonMap()
        val tag = json["tag"] as? String ?: return null
        if (json.str("schema") != "forkmesh-release-v2" || json.str("repo") != "forkmesh/forkmesh" || json["assets"] !is List<*>) return null
        val assets = LinkedHashMap<String, ReleaseAsset>()
        for (asset in json.maps("assets")) {
            val name = asset.str("name")
            if (name.isNotEmpty() && name !in assets) assets[name] = ReleaseAsset(name, asset.str("blob_sha256"), asset.long("size"), asset.int("build"))
        }
        return SignedRelease(tag, assets)
    }

    fun load(transport: InsightsRequesting, key: ByteArray = publicKey): LiveReleaseSummary {
        val first = transport.call("GET", blobsPath(manifestPaths("latest") + VERSION_FILE), null).obj("blobs")
        val latest = verified(first, "latest", key)
            ?: throw InsightsException("release_unverified", "The signed release manifest is missing or does not verify.")
        val project = blobBytes(first[VERSION_FILE])?.let { projectVersion(String(it, Charsets.UTF_8)) }
        val newest = project?.takeIf { isNewer(it, latest.tag) }
        val missing = platforms.any { platform -> !platform.required && platform.assets.any { latest.asset(it.first) == null } }
        val channels = listOfNotNull(newest?.let { "candidate-$it" }) + if (missing) earlierChannels(latest.tag) else emptyList()
        val more = if (channels.isEmpty()) emptyMap() else runCatching {
            transport.call("GET", blobsPath(channels.flatMap(::manifestPaths)), null).obj("blobs")
        }.getOrDefault(emptyMap())
        return summarize(latest, newest, more, key)
    }

    fun summarize(latest: SignedRelease, newest: String?, blobs: Map<String, Any?>, key: ByteArray = publicKey): LiveReleaseSummary {
        val earlier = earlierChannels(latest.tag).map { channel ->
            val version = channel.removePrefix("candidate-")
            verified(blobs, channel, key)?.takeIf { it.tag == "v$version" }
        }
        val rows = platforms.map { platform ->
            PlatformRelease(platform, platform.assets.mapNotNull { (name, arch) ->
                latest.asset(name)?.let { PlatformBuild(arch, latest.tag, it.build) }
                    ?: if (platform.required) null else earlier.firstNotNullOfOrNull { release ->
                        release?.asset(name)?.let { PlatformBuild(arch, release.tag, it.build) }
                    }
            })
        }
        val candidate = newest?.let { verified(blobs, "candidate-$it", key) }
        val offered = candidate != null && candidate.tag == "v$newest" &&
            platforms.any { platform -> platform.assets.any { candidate.asset(it.first) != null } }
        return LiveReleaseSummary(latest.tag, if (offered) candidate!!.tag else latest.tag, rows)
    }

    fun tiles(summary: LiveReleaseSummary): Map<String, StatsTileData> {
        val tiles = linkedMapOf<String, StatsTileData>()
        val count = summary.downloads
        tiles[LATEST_ID] = StatsTileData(
            summary.newest,
            if (summary.newest != summary.latest) "downloads are ${summary.latest} until ${summary.newest} builds are published"
            else "$count verified download${if (count == 1) "" else "s"}",
            status = if (summary.newest == summary.latest) StatsTone.Good else StatsTone.None,
        )
        for (row in summary.platforms) tiles[row.platform.id] = platformTile(row, summary.latest)
        return tiles
    }

    private fun platformTile(row: PlatformRelease, latest: String): StatsTileData {
        val platform = row.platform
        if (row.builds.isEmpty()) {
            return StatsTileData(note = if (platform.required) "Not in $latest" else "Coming soon", available = false)
        }
        val tag = row.newestTag
        val mixed = row.builds.any { it.tag != tag }
        val archs = if (platform.assets.size < 2) "" else if (mixed) {
            row.builds.joinToString(", ") { "${it.arch} ${it.tag}" }
        } else {
            row.builds.joinToString(", ") { it.arch }
        }
        val build = row.builds.filter { it.tag == tag }.maxOfOrNull { it.build } ?: 0
        val behind = row.builds.any { it.tag != latest }
        val note = listOf(
            platform.kind,
            archs,
            if (build > 1) "build $build" else "",
            if (behind) "behind $latest" else "",
        ).filter { it.isNotEmpty() }.joinToString(" · ")
        return StatsTileData(tag, note, status = if (behind) StatsTone.Warning else StatsTone.Good)
    }

    fun appStore(reply: AppStoreStatusReply): StatsTileData {
        val snapshot = reply.snapshot ?: return StatsTileData(note = "App Store status is not published yet", available = false)
        val live = snapshot.versions.firstOrNull { it.status.state in LIVE_STATES }
        val next = snapshot.versions.firstOrNull()?.takeIf { it != live && it.version.isNotEmpty() }
        val pending = next?.let { "${tagFor(it.version)} ${it.status.label.lowercase()}" }.orEmpty()
        if (live == null || live.version.isEmpty()) {
            return StatsTileData(note = pending.ifEmpty { "No version is live on the App Store yet" }, available = false)
        }
        val note = listOf("App Store", if (live.build.isNotEmpty()) "build ${live.build}" else "", pending).filter { it.isNotEmpty() }.joinToString(" · ")
        return StatsTileData(tagFor(live.version), note, status = StatsTone.Good)
    }

    fun loadAppStore(transport: InsightsRequesting): StatsTileData =
        appStore(AppStoreStatusReply.fromJson(transport.call("GET", AppStoreStatus.API, null)))

    private fun tagFor(version: String): String = if (version.firstOrNull()?.isDigit() == true) "v$version" else version
}
