package com.forkmesh.nativeapp.localrepos

import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.api.FakeTransport
import com.forkmesh.nativeapp.api.MemoryPrefStore
import com.forkmesh.nativeapp.api.TestSettings
import com.forkmesh.nativeapp.api.bodyJson
import com.forkmesh.nativeapp.api.jsonResponse
import com.forkmesh.nativeapp.api.path
import com.forkmesh.nativeapp.git.GitErrorCode
import com.forkmesh.nativeapp.git.GitException
import com.forkmesh.nativeapp.net.HttpClient
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.assertFails
import com.forkmesh.nativeapp.testing.expect
import java.io.File
import java.io.IOException

class LocalPublishServiceTest {
    private class FakeWorkspace(worktree: File) : GitWorkspace(worktree) {
        val pushes = ArrayList<Triple<String, String, String>>()
        val fetches = ArrayList<Pair<String, String>>()
        var pushResult = LocalPushResult(
            refs = listOf(LocalPushRef(remoteRef = "refs/heads/main", newId = "a".repeat(40), outcome = "updated")),
            packBytes = 512,
            objects = 4,
        )
        var pushError: Exception? = null

        override fun push(
            credentials: ForkMeshRemoteCredentials,
            branch: String,
            remote: String,
            onProgress: ((GitTransferProgress) -> Unit)?,
        ): LocalPushResult {
            pushes.add(Triple(credentials.owner, credentials.repo, branch))
            pushError?.let { throw it }
            return pushResult
        }

        override fun fetch(
            credentials: ForkMeshRemoteCredentials,
            remote: String,
            onProgress: ((GitTransferProgress) -> Unit)?,
        ): LocalFetchResult {
            fetches.add(credentials.owner to credentials.repo)
            return LocalFetchResult(
                updatedRefs = listOf("refs/remotes/origin/main"),
                packBytes = 128,
                objects = 2,
                remoteHead = "refs/heads/main",
            )
        }
    }

    private class Harness(
        val publisher: PublishService,
        val identity: SeededIdentity,
        val workspace: FakeWorkspace,
        val posted: MutableList<Map<String, Any?>>,
    )

    private fun harness(temp: File, respond: ((Map<String, Any?>) -> HttpClient.Response)? = null): Harness {
        val identity = SeededIdentity(ByteArray(32) { (it * 7 + 3).toByte() })
        val posted = ArrayList<Map<String, Any?>>()
        val transport = FakeTransport { request ->
            val body = request.bodyJson()
            posted.add(linkedMapOf<String, Any?>("path" to request.path()) + body)
            respond?.invoke(body) ?: jsonResponse(mapOf("ok" to true))
        }
        val api = ApiService(TestSettings(serverUrl = "wss://forkmesh.test/ws"), transport)
        return Harness(
            publisher = PublishService(api, identity = { identity }, prefs = MemoryPrefStore(), clock = { 1750000000000 }),
            identity = identity,
            workspace = FakeWorkspace(temp),
            posted = posted,
        )
    }

    fun testASignedOutSessionIsExplainedNotFailed() {
        assertEq("Sign in to publish a repository to ForkMesh.", PublishService.describeSession(null))
    }

    fun testAPlainAccountSessionCannotPublish() {
        val refusal = PublishService.describeSession(mobileNodeSession(capabilities = listOf("browse", "comment")))
        expect(refusal != null && refusal.contains("not a mobile node"))
    }

    fun testOwnerSignWithoutPublishRepoIsStillRefused() {
        expect(PublishService.describeSession(mobileNodeSession(capabilities = listOf("browse", "owner_sign"))) != null)
        expect(PublishService.describeSession(mobileNodeSession(capabilities = listOf("browse", "publish_repo"))) != null)
    }

    fun testAMobileNodeBindMayPublish() {
        assertEq(null, PublishService.describeSession(mobileNodeSession()))
    }

    fun testPublishRefusesWithoutPushingWhenTheSessionCannotPublish() = withScratch("fm-publish-") { temp ->
        val h = harness(temp)
        val outcome = h.publisher.publish(
            session = mobileNodeSession(capabilities = listOf("browse")),
            workspace = h.workspace,
            repo = "demo",
            branch = "main",
            isPublic = true,
        )
        assertEq(PublishStatus.unauthorized, outcome.status)
        expect(h.workspace.pushes.isEmpty())
        expect(h.posted.isEmpty())
    }

    fun testAcceptsTheLocalNameCharset() {
        assertEq(null, PublishService.validatePublishName("demo-repo_1.x"))
    }

    fun testRejectsAnEmptyIllegalOrOverLongName() {
        expect(PublishService.validatePublishName("") != null)
        expect(PublishService.validatePublishName("has space") != null)
        expect(PublishService.validatePublishName("a/b") != null)
        assertEq("Published names are at most 80 characters.", PublishService.validatePublishName("a".repeat(81)))
        expect(PublishService.validatePublishName("a".repeat(100)) != null)
    }

    fun testPushesTheBranchThenPostsAV2SignedCatalogRecord() = withScratch("fm-publish-") { temp ->
        val h = harness(temp)
        val outcome = h.publisher.publish(
            session = mobileNodeSession(),
            workspace = h.workspace,
            repo = "demo",
            branch = "main",
            isPublic = true,
            description = "Notes",
            head = PublishHead(
                commit = "a".repeat(40),
                subject = "Initial commit",
                authorName = "Alice",
                at = 1750000000000,
                commitCount = 1,
                branchCount = 1,
            ),
        )

        assertEq(PublishStatus.published, outcome.status)
        assertEq("alice/demo", outcome.publishedAs)
        assertEq("Published alice/demo.", outcome.message)
        assertEq(Triple("alice", "demo", "main"), h.workspace.pushes.single())

        val body = h.posted.single()
        assertEq("/api/repositories", body["path"])
        assertEq(2L, body["catalogSigVersion"])
        assertEq("user", body["publishIntent"])
        assertEq("local-node", body["source"])
        assertEq("mobile", body["platform"])
        assertEq("public", body["visibility"])
        assertEq(h.identity.publicKeyB64url, body["maintainer"])
        assertEq("https://forkmesh.test/alice/demo", body["cloneUrl"])
        expect(Regex("[0-9a-f]{32}").matches(body["nodeId"] as String))
        assertEq("1750000000", body["commitAt"])
        assertEq("Initial commit", body["commitSubject"])

        val record = LinkedHashMap(body).apply {
            remove("path")
            remove("catalogSig")
            remove("catalogSigVersion")
            remove("publishIntent")
            remove("signature")
        }
        val canonical = catalogV2Canonical(record)
        expect(SeededIdentity.verify(canonical.toByteArray(), body["catalogSig"] as String, h.identity.publicKeyB64url))
    }

    fun testTheNodeIdIsStableAcrossPublications() = withScratch("fm-publish-") { temp ->
        val h = harness(temp)
        repeat(2) {
            h.publisher.publishCatalogRecord(session = mobileNodeSession(), repo = "demo", branch = "main", isPublic = true)
        }
        assertEq(h.posted[0]["nodeId"], h.posted[1]["nodeId"])
    }

    fun testTheNodeIdPersistsInPreferences() {
        val prefs = MemoryPrefStore(mapOf(PublishService.nodeIdKey to "not-a-node-id"))
        val api = ApiService(TestSettings(serverUrl = "wss://forkmesh.test/ws"), FakeTransport { jsonResponse(emptyMap<String, Any?>()) })
        val first = PublishService(api, identity = { null }, prefs = prefs).nodeId()
        expect(Regex("[0-9a-f]{32}").matches(first))
        assertEq(first, prefs.getString(PublishService.nodeIdKey))
        assertEq(first, PublishService(api, identity = { null }, prefs = prefs).nodeId())
    }

    fun testARejectedRefStopsThePublishBeforeTheCatalogPost() = withScratch("fm-publish-") { temp ->
        val h = harness(temp)
        h.workspace.pushResult = LocalPushResult(
            refs = listOf(LocalPushRef(remoteRef = "refs/heads/main", newId = "", outcome = "rejected", message = "non-fast-forward")),
            packBytes = 0,
            objects = 0,
        )
        val outcome = h.publisher.publish(
            session = mobileNodeSession(),
            workspace = h.workspace,
            repo = "demo",
            branch = "main",
            isPublic = true,
        )
        assertEq(PublishStatus.failed, outcome.status)
        assertEq("ForkMesh refused main: non-fast-forward", outcome.message)
        expect(h.posted.isEmpty())
    }

    private fun answer(response: HttpClient.Response): PublishOutcome = withScratch("fm-publish-") { temp ->
        harness(temp) { response }.publisher.publishCatalogRecord(
            session = mobileNodeSession(),
            repo = "demo",
            branch = "main",
            isPublic = true,
        )
    }

    fun test202MirrorTrueAsksTheUserToRename() {
        val outcome = answer(jsonResponse(mapOf("mirror" to true, "canonicalNodeId" to "desktop-node-1"), 202))
        assertEq(PublishStatus.mirrored, outcome.status)
        expect(!outcome.isSuccess)
        assertEq("desktop-node-1", outcome.canonicalNodeId)
        assertEq("alice/demo", outcome.publishedAs)
        expect(outcome.message.contains("different name"))
    }

    fun test409StaleUpdateTellsTheUserToFetchFirst() {
        val outcome = answer(jsonResponse(mapOf("error" to "stale_update"), 409))
        assertEq(PublishStatus.stale, outcome.status)
        expect(outcome.message.contains("newer record"))
    }

    fun test403AccountRequiredIsACapabilityExplanation() {
        val outcome = answer(jsonResponse(mapOf("error" to "account_required"), 403))
        assertEq(PublishStatus.unauthorized, outcome.status)
        expect(outcome.message.contains("no key that may publish"))
    }

    fun test403MaintainerMismatchNamesTheOwner() {
        val outcome = answer(jsonResponse(mapOf("error" to "maintainer_mismatch"), 403))
        assertEq(PublishStatus.unauthorized, outcome.status)
        expect(outcome.message.contains("alice"))
    }

    fun test426PointsPrivatePublicationAtTheDesktopNode() {
        val outcome = answer(jsonResponse(mapOf("error" to "client_upgrade_required"), 426))
        assertEq(PublishStatus.upgradeRequired, outcome.status)
        expect(outcome.message.contains("owner-sealed"))
    }

    fun test429IsReportedAsALimitNotAFailure() {
        assertEq(PublishStatus.limited, answer(jsonResponse(mapOf("error" to "rate_limited"), 429)).status)
    }

    fun test410ExplainsTheTombstone() {
        val outcome = answer(jsonResponse(mapOf("error" to "deleted"), 410))
        assertEq(PublishStatus.failed, outcome.status)
        expect(outcome.message.contains("deleted on ForkMesh"))
    }

    fun testAnUnexpectedStatusKeepsTheErrorCode() {
        val outcome = answer(jsonResponse(mapOf("error" to "boom"), 500))
        assertEq(PublishStatus.failed, outcome.status)
        expect(outcome.message.contains("boom"))
        assertEq(
            "ForkMesh could not publish this repository (HTTP 502).",
            answer(jsonResponse(emptyMap<String, Any?>(), 502)).message,
        )
    }

    fun testPushSignsForTheMeshRemoteAndNeverForces() = withScratch("fm-publish-") { temp ->
        val h = harness(temp)
        val result = h.publisher.push(workspace = h.workspace, owner = "alice", repo = "demo", branch = "main")
        assertEq("main", result.refs.single().branch)
        assertEq("alice", h.workspace.pushes.single().first)
        expect(h.posted.isEmpty())
    }

    fun testCredentialsCarryTheDeviceKeyAndPubkeyByDefault() = withScratch("fm-publish-") { temp ->
        val h = harness(temp)
        val signed = h.publisher.credentials(owner = "alice", repo = "demo")
        expect(signed.canSign)
        expect(signed.signer === h.identity)
        assertEq(h.identity.publicKeyB64url, signed.pubkeyB64url)
        assertEq("https://forkmesh.test/alice/demo", signed.remoteUrl)
        val anonymous = h.publisher.credentials(owner = "alice", repo = "demo", signed = false)
        expect(!anonymous.canSign)
        assertEq("", anonymous.pubkeyB64url)
    }

    fun testFetchReportsTheUpdatedRefs() = withScratch("fm-publish-") { temp ->
        val h = harness(temp)
        val result = h.publisher.fetch(workspace = h.workspace, owner = "alice", repo = "demo")
        expect(!result.isUpToDate)
        assertEq(listOf("refs/remotes/origin/main"), result.updatedRefs)
        assertEq("alice" to "demo", h.workspace.fetches.single())
    }

    private fun workspaceError(message: String, code: GitErrorCode = GitErrorCode.remoteRejected): GitWorkspaceException {
        val failure = describeRemoteFailure(GitException(code, message), push = true)
        return GitWorkspaceException(failure.message, detail = failure.code)
    }

    fun test413PushTooLargeSurfacesThe8MbCap() = withScratch("fm-publish-") { temp ->
        val h = harness(temp)
        h.workspace.pushError = workspaceError("remote returned HTTP 413")
        val error = assertFails<GitWorkspaceException> {
            h.publisher.publish(session = mobileNodeSession(), workspace = h.workspace, repo = "demo", branch = "main", isPublic = true)
        }
        assertEq("push_too_large", error.detail)
        expect(error.message.contains("8 MB"))
        expect(h.posted.isEmpty())
    }

    fun test401PushUnauthorizedNeverWritesACatalogRecord() = withScratch("fm-publish-") { temp ->
        val h = harness(temp)
        h.workspace.pushError = workspaceError("remote returned HTTP 401")
        val error = assertFails<GitWorkspaceException> {
            h.publisher.publish(session = mobileNodeSession(), workspace = h.workspace, repo = "demo", branch = "main", isPublic = true)
        }
        assertEq("push_unauthorized", error.detail)
        expect(h.posted.isEmpty())
    }

    fun test503PushUnavailableIsReportedAsARetryableOutage() = withScratch("fm-publish-") { temp ->
        val h = harness(temp)
        h.workspace.pushError = workspaceError("remote returned HTTP 503")
        val error = assertFails<GitWorkspaceException> {
            h.publisher.push(workspace = h.workspace, owner = "alice", repo = "demo", branch = "main")
        }
        assertEq("push_unavailable", error.detail)
        expect(error.message.contains("try again"))
    }

    fun testANonFastForwardPushSaysFetchAndRetry() = withScratch("fm-publish-") { temp ->
        val h = harness(temp)
        h.workspace.pushError = workspaceError("", GitErrorCode.nonFastForward)
        val error = assertFails<GitWorkspaceException> {
            h.publisher.push(workspace = h.workspace, owner = "alice", repo = "demo", branch = "main")
        }
        assertEq("Someone else pushed first — fetch and retry.", error.message)
    }

    fun testAnUnreachableWorkerFailsThePublishNotTheApp() = withScratch("fm-publish-") { temp ->
        val h = harness(temp) { throw IOException("down") }
        val outcome = h.publisher.publishCatalogRecord(session = mobileNodeSession(), repo = "demo", branch = "main", isPublic = true)
        assertEq(PublishStatus.failed, outcome.status)
        expect(outcome.message.contains("Could not reach ForkMesh"), outcome.message)
    }

    fun testWithoutADeviceIdentityNothingIsSignedOrPosted() = withScratch("fm-publish-") { temp ->
        val posted = ArrayList<Any>()
        val api = ApiService(TestSettings(serverUrl = "wss://forkmesh.test/ws"), FakeTransport { posted.add(it); jsonResponse(emptyMap<String, Any?>()) })
        val publisher = PublishService(api, identity = { null }, prefs = MemoryPrefStore())
        val outcome = publisher.publishCatalogRecord(session = mobileNodeSession(), repo = "demo", branch = "main", isPublic = true)
        assertEq(PublishStatus.unauthorized, outcome.status)
        expect(posted.isEmpty())
        expect(!publisher.credentials(owner = "alice", repo = "demo").canSign)
    }

    private fun normalize(value: String, owner: String = "kaif", name: String = "forkmesh") = normalizeForkedFrom(value, owner, name)

    fun testForkedFromMatchesTheWorkerOnValuesItKeeps() {
        assertEq("mainnode/forkmesh", normalize("mainnode/forkmesh"))
        assertEq("mainnode/forkmesh", normalize("MainNode/ForkMesh"))
        assertEq("mainnode/forkmesh", normalize("  mainnode/forkmesh  "))
        assertEq("main.node/fork-mesh_1", normalize("main.node/fork-mesh_1"))
        assertEq("mainnode/forkmesh", normalize("mainnode/forkmesh", name = "FORKMESH"))
    }

    fun testForkedFromMatchesTheWorkerOnValuesItDrops() {
        for (value in listOf("kaif/forkmesh", "KAIF/ForkMesh", "", "noslash", "a/b/c", "main node/forkmesh", "mainnode/", "/forkmesh", "mainnode/fork mesh", "m".repeat(81) + "/forkmesh")) {
            assertEq(null, normalize(value), value)
        }
    }

    fun testForkedFromIsAbsentNotEmptyWhenNotAFork() {
        val plain = buildCatalogRecord("kaif", "demo", "", true, "main", "k", "0".repeat(32), 1_789_776_000_000)
        expect(!plain.containsKey("forkedFrom"))
        val fork = buildCatalogRecord("kaif", "demo", "", true, "main", "k", "0".repeat(32), 1_789_776_000_000, forkedFrom = "MainNode/Demo")
        assertEq("mainnode/demo", fork["forkedFrom"])
    }

    fun testPublishingAForkPostsTheSignedEdge() = withScratch("fm-publish-") { temp ->
        val h = harness(temp)
        val outcome = h.publisher.publish(mobileNodeSession(), h.workspace, "demo", "main", isPublic = true, forkedFrom = "mainnode/demo")
        assertEq(PublishStatus.published, outcome.status)
        val body = h.posted.single()
        assertEq("mainnode/demo", body["forkedFrom"])
        val record = LinkedHashMap(body).apply {
            remove("path")
            remove("catalogSig")
            remove("catalogSigVersion")
            remove("publishIntent")
            remove("signature")
        }
        expect(SeededIdentity.verify(catalogV2Canonical(record).toByteArray(), body["catalogSig"] as String, h.identity.publicKeyB64url))
    }

    fun testPublishingUnderTheParentAddressPostsNoEdge() = withScratch("fm-publish-") { temp ->
        val h = harness(temp)
        h.publisher.publish(mobileNodeSession(), h.workspace, "demo", "main", isPublic = true, forkedFrom = "alice/demo")
        expect(!h.posted.single().containsKey("forkedFrom"))
    }
}
