package com.forkmesh.nativeapp.code

import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.api.FakeTransport
import com.forkmesh.nativeapp.api.TestSettings
import com.forkmesh.nativeapp.api.jsonResponse
import com.forkmesh.nativeapp.api.path
import com.forkmesh.nativeapp.api.query
import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.net.HttpClient
import com.forkmesh.nativeapp.testing.assertEq

class ReleaseDownloadsTest {
    private val releaseOne = mapOf(
        "tag" to "v1.0.0",
        "channel" to "stable",
        "tag_commit" to "0123456789abcdef",
        "created_at" to 1_000L,
        "assets" to listOf(mapOf("name" to "app.apk", "sha256" to "sha-a", "size" to 2048L, "os" to "android", "arch" to "arm64")),
    )
    private val releaseTwo = mapOf(
        "tag" to "v1.1.0",
        "created_at" to 2_000L,
        "assets" to listOf(mapOf("name" to "b.zip", "sha256" to "sha-b"), mapOf("filename" to "c.tar", "blob_sha256" to "sha-c")),
    )

    private fun api(downloads: (HttpClient.Request) -> HttpClient.Response): Pair<ApiService, FakeTransport> {
        val transport = FakeTransport { request ->
            val path = request.path()
            val query = request.query()
            when {
                path.endsWith("/releases/downloads") -> downloads(request)
                path.endsWith("/tree") && query["path"] == ".forkmesh/releases" -> jsonResponse(
                    mapOf(
                        "entries" to listOf(
                            mapOf("name" to "v1.0.0", "type" to "dir"),
                            mapOf("name" to "v1.1.0", "type" to "tree"),
                            mapOf("name" to "README.md", "type" to "file"),
                        ),
                    ),
                )
                path.endsWith("/blob") && query["path"] == ".forkmesh/releases/v1.0.0/release.json" ->
                    jsonResponse(mapOf("content" to Json.stringify(releaseOne)))
                path.endsWith("/blob") && query["path"] == ".forkmesh/releases/v1.1.0/release.json" ->
                    jsonResponse(mapOf("content" to Json.stringify(releaseTwo)))
                else -> jsonResponse(mapOf("error" to "not_found"), 404)
            }
        }
        return ApiService(TestSettings(serverUrl = "wss://forkmesh.test/ws"), transport) to transport
    }

    fun testDownloadCountsMergeIntoAssetsBySha() {
        val (api, _) = api { jsonResponse(mapOf("counts" to mapOf("sha-a" to 5L, "sha-b" to 1L))) }
        val releases = api.releases("owner", "repo")
        assertEq(listOf("v1.1.0", "v1.0.0"), releases.map { it.tag })
        val newest = releases[0]
        assertEq("2 artifacts", newest.assetSummary)
        assertEq(listOf("1 download", "0 downloads"), newest.assets.map { it.downloadLabel })
        assertEq(listOf("b.zip", "c.tar"), newest.assets.map { it.name })
        assertEq("artifact", newest.assets[0].platformLabel)
        assertEq("unknown size", newest.assets[0].sizeLabel)
        val oldest = releases[1]
        assertEq("1 artifact", oldest.assetSummary)
        assertEq("5 downloads", oldest.assets.single().downloadLabel)
        assertEq("android / arm64", oldest.assets.single().platformLabel)
        assertEq("2.0 KB", oldest.assets.single().sizeLabel)
        assertEq("01234567", oldest.shortCommit)
    }

    fun testFlatCountMapsAreAcceptedToo() {
        val (api, _) = api { jsonResponse(mapOf("sha-c" to 3L)) }
        assertEq(listOf("0 downloads", "3 downloads"), api.releases("owner", "repo")[0].assets.map { it.downloadLabel })
    }

    fun testReleasesStillLoadWhenCountsAreUnavailable() {
        val (api, _) = api { jsonResponse(mapOf("error" to "not_found"), 404) }
        val releases = api.releases("owner", "repo")
        assertEq(2, releases.size)
        assertEq(listOf("0 downloads"), releases[1].assets.map { it.downloadLabel })
    }
}
