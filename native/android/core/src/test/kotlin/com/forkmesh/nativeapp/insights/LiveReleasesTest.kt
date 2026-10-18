package com.forkmesh.nativeapp.insights

import com.forkmesh.nativeapp.crypto.Ed25519
import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.market.AppStoreStatusReply
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.assertFails
import com.forkmesh.nativeapp.testing.expect
import com.forkmesh.nativeapp.testing.resourceBytes
import java.util.Base64

class LiveReleasesTest {
    private val seed = ByteArray(32) { (it * 7 + 1).toByte() }
    private val key = Ed25519.publicKey(seed)
    private val sha = "a".repeat(64)

    private fun manifest(tag: String, vararg assets: Pair<String, Int>): String = Json.stringify(
        mapOf(
            "schema" to "forkmesh-release-v2",
            "repo" to "forkmesh/forkmesh",
            "tag" to tag,
            "assets" to assets.map { (name, build) -> mapOf("name" to name, "blob_sha256" to sha, "size" to 10L, "build" to build.toLong()) },
        ),
    )

    private fun signed(blobs: MutableMap<String, Any?>, channel: String, text: String, signer: ByteArray = seed) {
        val (manifestPath, signaturePath) = LiveReleases.manifestPaths(channel)
        blobs[manifestPath] = mapOf("content" to text)
        blobs[signaturePath] = mapOf(
            "content" to Base64.getEncoder().encodeToString(Ed25519.sign(signer, text.toByteArray())),
            "encoding" to "base64",
        )
    }

    private class BlobTransport(val first: Map<String, Any?>, val second: Map<String, Any?>) : InsightsRequesting {
        val paths = ArrayList<String>()

        override fun call(method: String, path: String, body: Map<String, Any?>?): Map<String, Any?> {
            paths.add(path)
            return mapOf("blobs" to if (paths.size == 1) first else second)
        }
    }

    fun testTheRealManifestVerifiesAgainstTheHomepageKey() {
        val blobs = mapOf(
            ".forkmesh/releases/latest/release.json" to mapOf("content" to String(resourceBytes("releases/release.json"))),
            ".forkmesh/releases/latest/release.json.sig" to mapOf(
                "content" to Base64.getEncoder().encodeToString(resourceBytes("releases/release.json.sig")),
                "encoding" to "base64",
            ),
        )
        val release = LiveReleases.verified(blobs, "latest")
        assertEq("v1.2.8", release?.tag)
        expect(release!!.asset("forkmesh-android-universal.apk") != null)
        val tampered = blobs + (".forkmesh/releases/latest/release.json" to mapOf("content" to String(resourceBytes("releases/release.json")).replace("v1.2.8", "v9.9.9")))
        expect(LiveReleases.verified(tampered, "latest") == null)
    }

    fun testEveryPlatformShowsTheVersionTheHomepageOffers() {
        val first = HashMap<String, Any?>()
        signed(
            first, "latest",
            manifest("v1.2.8", "forkmesh-android-universal.apk" to 3, "forkmesh-linux-x86_64.AppImage" to 0, "forkmesh-macos-arm64.dmg" to 0),
        )
        first[LiveReleases.VERSION_FILE] = mapOf("content" to "project(ForkMesh VERSION 1.2.9 LANGUAGES CXX)")
        val second = HashMap<String, Any?>()
        signed(second, "candidate-1.2.9", manifest("v1.2.9", "forkmesh-linux-x86_64.AppImage" to 0))
        signed(second, "candidate-1.2.6", manifest("v1.2.6", "forkmesh-windows-x86_64-setup.exe" to 0, "forkmesh-macos-x86_64.dmg" to 0))
        signed(second, "candidate-1.2.7", manifest("v1.2.7", "forkmesh-macos-x86_64.dmg" to 0))
        val transport = BlobTransport(first, second)
        val summary = LiveReleases.load(transport, key)
        expect(transport.paths[0].startsWith("/api/repo/forkmesh/forkmesh/blobs?path=.forkmesh%2Freleases%2Flatest%2Frelease.json&"))
        expect(transport.paths[0].endsWith("&ref=main"))
        expect("candidate-1.2.9" in transport.paths[1] && "candidate-1.2.2" in transport.paths[1] && "candidate-1.2.1" !in transport.paths[1])
        assertEq("v1.2.8", summary.latest)
        assertEq("v1.2.9", summary.newest)
        val tiles = LiveReleases.tiles(summary)
        assertEq("v1.2.9", tiles.getValue(LiveReleases.LATEST_ID).value)
        assertEq("downloads are v1.2.8 until v1.2.9 builds are published", tiles.getValue(LiveReleases.LATEST_ID).note)
        assertEq("v1.2.8", tiles.getValue("release-android").value)
        assertEq("APK · build 3", tiles.getValue("release-android").note)
        assertEq(StatsTone.Good, tiles.getValue("release-android").status)
        assertEq("v1.2.8", tiles.getValue("release-macos").value)
        assertEq("DMG · Apple silicon v1.2.8, Intel v1.2.7 · behind v1.2.8", tiles.getValue("release-macos").note)
        assertEq(StatsTone.Warning, tiles.getValue("release-macos").status)
        assertEq("v1.2.6", tiles.getValue("release-windows").value)
        assertEq("Installer · behind v1.2.8", tiles.getValue("release-windows").note)
        assertEq("v1.2.8", tiles.getValue("release-linux").value)
    }

    fun testMissingBuildsAndBadSignaturesAreNotOffered() {
        val first = HashMap<String, Any?>()
        signed(first, "latest", manifest("v1.0.0", "forkmesh-linux-x86_64.AppImage" to 0))
        first[LiveReleases.VERSION_FILE] = mapOf("content" to "project(ForkMesh VERSION 1.0.1)")
        val second = HashMap<String, Any?>()
        signed(second, "candidate-1.0.1", manifest("v1.0.1", "forkmesh-linux-x86_64.AppImage" to 0), signer = ByteArray(32) { 9 })
        val summary = LiveReleases.load(BlobTransport(first, second), key)
        assertEq("v1.0.0", summary.newest)
        val tiles = LiveReleases.tiles(summary)
        assertEq("1 verified download", tiles.getValue(LiveReleases.LATEST_ID).note)
        expect(!tiles.getValue("release-android").available)
        assertEq("Not in v1.0.0", tiles.getValue("release-android").note)
        assertEq("Coming soon", tiles.getValue("release-windows").note)
        val failure = assertFails<InsightsException> { LiveReleases.load(BlobTransport(emptyMap(), emptyMap()), key) }
        assertEq("release_unverified", failure.code)
    }

    fun testIosShowsTheVersionLiveOnTheAppStore() {
        val reply = AppStoreStatusReply.fromJson(
            mapOf(
                "snapshot" to mapOf(
                    "versions" to listOf(
                        mapOf("version" to "1.2.9", "status" to mapOf("state" to "WAITING_FOR_REVIEW", "label" to "Waiting for review")),
                        mapOf("version" to "1.2.8", "build" to "41", "status" to mapOf("state" to "READY_FOR_SALE", "label" to "Ready for sale")),
                    ),
                ),
            ),
        )
        val tile = LiveReleases.appStore(reply)
        assertEq("v1.2.8", tile.value)
        assertEq("App Store · build 41 · v1.2.9 waiting for review", tile.note)
        expect(!LiveReleases.appStore(AppStoreStatusReply()).available)
    }
}
