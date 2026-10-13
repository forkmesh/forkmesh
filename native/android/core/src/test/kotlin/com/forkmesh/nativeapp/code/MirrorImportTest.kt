package com.forkmesh.nativeapp.code

import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.api.FakeTransport
import com.forkmesh.nativeapp.api.TestSettings
import com.forkmesh.nativeapp.api.bodyJson
import com.forkmesh.nativeapp.api.jsonResponse
import com.forkmesh.nativeapp.api.path
import com.forkmesh.nativeapp.api.textResponse
import com.forkmesh.nativeapp.net.HttpClient
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class MirrorImportTest {
    private class Harness(val api: ApiService, val transport: FakeTransport)

    private fun harness(respond: (HttpClient.Request) -> HttpClient.Response = { jsonResponse(mapOf("ok" to true, "created" to true)) }): Harness {
        val transport = FakeTransport(respond)
        val api = ApiService(TestSettings(serverUrl = "wss://forkmesh.test/ws"), transport)
        api.sessionTokenProvider = { "session-token" }
        return Harness(api, transport)
    }

    private fun Harness.submit(url: String, token: String = "", account: String? = "alice") =
        MirrorImport.submit(api, url, token, account)

    fun testAcceptsEveryCanonicalProviderHost() {
        for (url in listOf(
            "https://github.com/owner/repository",
            "https://gitlab.com/owner/repository",
            "https://codeberg.org/owner/repository",
            "https://gitlab.com/group/subgroup/repository",
        )) {
            assertEq(null, MirrorImport.validateSourceUrl(url), url)
        }
    }

    fun testToleratesMissingSchemeWwwAndSpace() {
        assertEq(null, MirrorImport.validateSourceUrl(" github.com/owner/repository "))
        assertEq(null, MirrorImport.validateSourceUrl("https://www.github.com/owner/repository"))
    }

    fun testRejectsAnEmptyUrl() {
        assertEq("Paste a repository URL.", MirrorImport.validateSourceUrl("   "))
    }

    fun testRejectsNonHttpsAndCredentials() {
        expect(MirrorImport.validateSourceUrl("http://github.com/owner/repo")!!.contains("https://"))
        expect(MirrorImport.validateSourceUrl("https://user:pass@github.com/owner/repo")!!.contains("https://"))
    }

    fun testRejectsNonCanonicalHosts() {
        expect(MirrorImport.validateSourceUrl("https://bitbucket.org/owner/repo")!!.contains("github.com, gitlab.com, codeberg.org"))
        expect(MirrorImport.validateSourceUrl("https://github.com.evil.test/owner/repo")!!.contains("github.com, gitlab.com, codeberg.org"))
    }

    fun testRejectsAnOwnerOnlyUrl() {
        assertEq("Include the owner and the repository name.", MirrorImport.validateSourceUrl("https://codeberg.org/m33"))
    }

    fun testAdvertisesExactlyTheThreeProviders() {
        assertEq(listOf("github.com", "gitlab.com", "codeberg.org"), MirrorImport.providers)
    }

    fun testPostsTheDocumentedBody() {
        val h = harness()
        val outcome = h.submit("  https://github.com/owner/repository  ")
        assertEq(1, h.transport.requests.size)
        val post = h.transport.requests.single()
        assertEq("/api/repository-imports", post.path())
        val body = post.bodyJson()
        assertEq(setOf("sourceUrl", "targetOwner", "mode", "sessionToken"), body.keys)
        assertEq("https://github.com/owner/repository", body["sourceUrl"])
        assertEq("alice", body["targetOwner"])
        assertEq("import", body["mode"])
        assertEq("session-token", body["sessionToken"])
        expect(outcome is MirrorImportOutcome.Done && outcome.notice.contains("external entry"), "$outcome")
    }

    fun testPostsGitlabAndCodebergUnchanged() {
        for (url in listOf("https://gitlab.com/owner/repository", "https://codeberg.org/owner/repository")) {
            val h = harness()
            h.submit(url)
            assertEq(url, h.transport.requests.single().bodyJson()["sourceUrl"], url)
            assertEq("/api/repository-imports", h.transport.requests.single().path())
        }
    }

    fun testSendsATypedProviderTokenTrimmed() {
        val h = harness()
        h.submit("https://github.com/owner/private", token = "  ghp_secret  ")
        assertEq("ghp_secret", h.transport.requests.single().bodyJson()["providerToken"])
        assertEq(MirrorImport.TOKEN_HELPER, "Used for this request only; never stored.")
    }

    fun testRejectsANonCanonicalUrlBeforeAnyRoundTrip() {
        val h = harness()
        val outcome = h.submit("https://bitbucket.org/owner/repo")
        expect(h.transport.requests.isEmpty())
        expect(outcome is MirrorImportOutcome.Problem && outcome.message.contains("Only github.com, gitlab.com, codeberg.org"), "$outcome")
    }

    fun testRefusesToImportWhileSignedOut() {
        val h = harness()
        assertEq(MirrorImportOutcome.Status("Sign in to import a repository."), h.submit("https://github.com/owner/repository", account = null))
        assertEq(MirrorImportOutcome.Status("Sign in to import a repository."), h.submit("https://github.com/owner/repository", account = ""))
        expect(h.transport.requests.isEmpty())
    }

    fun testExplainsAWorkerRefusal() {
        val h = harness { jsonResponse(mapOf("error" to "target_owner_forbidden"), 403) }
        assertEq(
            MirrorImportOutcome.Status("You cannot import into that owner. Pick an account you administer."),
            h.submit("https://github.com/owner/repository"),
        )
        assertEq(1, h.transport.requests.size)
    }

    fun testExplainsProviderRateLimiting() {
        val h = harness { jsonResponse(mapOf("error" to "rate_limited"), 429) }
        assertEq(MirrorImportOutcome.Status("The provider is rate-limiting; try again later."), h.submit("https://github.com/owner/repository"))
    }

    fun testNamesAnUnknownErrorCode() {
        val h = harness { jsonResponse(mapOf("error" to "teapot"), 418) }
        assertEq(MirrorImportOutcome.Status("Import failed (teapot)."), h.submit("https://github.com/owner/repository"))
    }

    fun testReportsABodylessFailureWithItsStatus() {
        val h = harness { textResponse("", 500) }
        assertEq(MirrorImportOutcome.Status("Import failed (HTTP 500)."), h.submit("https://github.com/owner/repository"))
    }

    fun testSurfacesATransportFailureWithoutAStack() {
        val h = harness { throw java.net.SocketException("Connection failed") }
        val outcome = h.submit("https://github.com/owner/repository")
        expect(outcome is MirrorImportOutcome.Status && outcome.message.startsWith("Import failed:"), "$outcome")
        expect(!(outcome as MirrorImportOutcome.Status).message.contains("#0"))
    }

    fun testPrefersTheWorkerNoticeAndReportsARefresh() {
        val refreshed = harness { jsonResponse(mapOf("ok" to true, "created" to false)) }
        assertEq(MirrorImportOutcome.Done("Refreshed the existing external listing."), refreshed.submit("https://github.com/owner/repository"))
        val spoken = harness { jsonResponse(mapOf("ok" to true, "created" to true, "notice" to "Imported 12 issues.")) }
        assertEq(MirrorImportOutcome.Done("Imported 12 issues."), spoken.submit("https://github.com/owner/repository"))
    }

    fun testSaysTheEntryIsMetadataOnly() {
        expect(MirrorImport.DISCLAIMER.contains("Imports metadata only"))
        expect(MirrorImport.DISCLAIMER.contains("does not own or control the source repository"))
        assertEq("Mirror from GitHub/GitLab/Codeberg", MirrorImport.TITLE)
    }
}
