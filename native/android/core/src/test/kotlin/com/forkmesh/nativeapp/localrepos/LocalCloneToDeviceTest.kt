package com.forkmesh.nativeapp.localrepos

import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.api.FakeTransport
import com.forkmesh.nativeapp.api.MemoryPrefStore
import com.forkmesh.nativeapp.api.TestSettings
import com.forkmesh.nativeapp.api.jsonResponse
import com.forkmesh.nativeapp.git.GitErrorCode
import com.forkmesh.nativeapp.git.GitException
import com.forkmesh.nativeapp.git.security.GitResourceLimits
import com.forkmesh.nativeapp.model.auth.AuthSession
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import java.io.File
import java.time.Instant
import java.util.Collections
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit

class LocalCloneToDeviceTest {
    private class ScriptedRunner : GitIsolateRunner() {
        val ops: MutableList<String> = Collections.synchronizedList(ArrayList())
        var cloneProgress: List<GitTransferProgress> = emptyList()
        var cloneResult = LocalCloneResult(branch = "main", packBytes = 4096, objects = 12)

        var cloneError: Throwable? = null

        var holdClone = false
        private val held = CountDownLatch(1)

        @Volatile
        private var cancelledWhileHeld = false

        val summaryResult = LocalRepoSummary(
            branch = "main",
            headId = "a".repeat(40),
            lastCommitSubject = "Initial commit",
            lastCommitAt = Instant.parse("2026-08-20T12:00:00Z").toEpochMilli(),
        )

        @Volatile
        var cancelled = false

        @Volatile
        var disposed = false

        val holding = CountDownLatch(1)

        fun release() = held.countDown()

        @Suppress("UNCHECKED_CAST")
        override fun <T> runWithProgress(name: String, body: (GitWorkContext) -> T, onProgress: ((Any) -> Unit)?): T {
            ops.add(name)
            return when (name) {
                "clone" -> {
                    for (sample in cloneProgress) onProgress?.invoke(sample)
                    if (holdClone) {
                        holding.countDown()
                        held.await(20, TimeUnit.SECONDS)
                        if (cancelledWhileHeld) throw GitException(GitErrorCode.cancelled, "cancelled by the user")
                    }
                    cloneError?.let { throw it }
                    cloneResult as T
                }
                "init" -> null as T
                "summary" -> summaryResult as T
                else -> throw IllegalStateException("unexpected engine op: $name")
            }
        }

        override fun cancel() {
            cancelled = true
            cancelledWhileHeld = true
            held.countDown()
        }

        override fun dispose() {
            disposed = true
        }
    }

    private class ScriptedWorkspace(worktree: File, runner: ScriptedRunner) : GitWorkspace(worktree, runner = runner) {
        var cloneCredentials: ForkMeshRemoteCredentials? = null
        var cloneMaxPackBytes: Int? = null
        var cloneBranch: String? = "unset"
        var cloneDepth: Int? = null

        override fun clone(
            credentials: ForkMeshRemoteCredentials,
            branch: String?,
            maxPackBytes: Int,
            onProgress: ((GitTransferProgress) -> Unit)?,
            depth: Int?,
        ): LocalCloneResult {
            cloneCredentials = credentials
            cloneMaxPackBytes = maxPackBytes
            cloneBranch = branch
            cloneDepth = depth
            return super.clone(credentials, branch, maxPackBytes, onProgress, depth)
        }
    }

    private class ScriptedStore(root: File, val runner: ScriptedRunner) : LocalRepoStore(root) {
        @Volatile
        var lastWorkspace: ScriptedWorkspace? = null

        override fun workspace0(directory: File): GitWorkspace = ScriptedWorkspace(directory, runner).also { lastWorkspace = it }
    }

    private class Harness(val store: ScriptedStore, val runner: ScriptedRunner, val publisher: PublishService, val identity: SeededIdentity)

    private inline fun withHarness(block: (Harness) -> Unit) = withScratch("fm-clone-screen-") { temp ->
        val runner = ScriptedRunner()
        val identity = SeededIdentity()
        val api = ApiService(TestSettings(serverUrl = "wss://forkmesh.test/ws"), FakeTransport { jsonResponse(emptyMap<String, Any?>()) })
        block(
            Harness(
                store = ScriptedStore(File(temp, "repos"), runner),
                runner = runner,
                publisher = PublishService(api, identity = { identity }, prefs = MemoryPrefStore()),
                identity = identity,
            ),
        )
    }

    private fun clone(
        harness: Harness,
        session: AuthSession? = mobileNodeSession(),
        localName: String = suggestCloneName(harness.store.records, "catalog-repo"),
        progress: MutableList<String>? = null,
    ): CloneToDeviceOutcome {
        harness.store.ensureLoaded()
        return cloneToDevice(
            store = harness.store,
            publisher = harness.publisher,
            session = session,
            owner = "net",
            name = "catalog-repo",
            localName = localName,
            description = "A network repository",
            onProgress = { sample -> progress?.add(sample.label) },
        )
    }

    fun testClonesFromTheAppOriginAndLandsInTheLocalStore() = withHarness { harness ->
        harness.runner.cloneProgress = listOf(
            GitTransferProgress(phase = "connecting"),
            GitTransferProgress(phase = "receivingPack", bytes = 2L * 1024 * 1024),
        )
        assertEq("catalog-repo", suggestCloneName(harness.store.records, "catalog-repo"))
        expect(cloneToDeviceNote.contains("latest commit of the default branch"))
        expect(!cloneToDeviceNote.contains("refused"))
        assertEq(1, GitWorkspace.meshCloneDepth)
        assertEq(GitResourceLimits.hardMaxCompressedPackBytes, GitWorkspace.maxCloneBytes)
        assertEq(GitResourceLimits.hardMaxInflatedBytes, GitWorkspace.deviceLimits().maxInflatedBytes)
        assertEq(GitResourceLimits.hardMaxObjectBytes, GitWorkspace.deviceLimits().maxObjectBytes)
        val labels = Collections.synchronizedList(ArrayList<String>())
        val outcome = clone(harness, progress = labels)

        val workspace = harness.store.lastWorkspace!!
        val credentials = workspace.cloneCredentials!!
        assertEq("https://forkmesh.test/net/catalog-repo", credentials.remoteUrl)
        assertEq("net", credentials.owner)
        assertEq("catalog-repo", credentials.repo)
        expect(credentials.canSign)
        assertEq(harness.identity.publicKeyB64url, credentials.pubkeyB64url)
        expect(credentials.signer === harness.identity)
        assertEq(GitWorkspace.maxCloneBytes, workspace.cloneMaxPackBytes)
        assertEq(null, workspace.cloneBranch)
        assertEq(GitWorkspace.meshCloneDepth, workspace.cloneDepth)
        expect(harness.runner.ops.contains("clone"))
        assertEq(listOf("Connecting to ForkMesh", "Downloading 2.0 MB"), labels.toList())

        val record = outcome.record!!
        assertEq("catalog-repo", record.name)
        assertEq("net/catalog-repo", record.publishedAs)
        assertEq(LocalRepoOrigin.cloned, record.origin)
        expect(record.isCloned)
        assertEq("Cloned", record.syncLabel)
        assertEq("A network repository", record.description)

        val stored = harness.store.list()
        assertEq(1, stored.size)
        assertEq("catalog-repo", stored.single().name)
        assertEq("net/catalog-repo", stored.single().publishedAs)
        assertEq(LocalRepoOrigin.cloned, stored.single().origin)

        assertEq("Cloned net/catalog-repo to catalog-repo from the relay (forkmesh.test).", outcome.message)
        expect(harness.runner.disposed)
        expect(!harness.store.isBusy)
    }

    fun testShowsEngineProgressWhileTheCloneIsInFlight() = withHarness { harness ->
        harness.runner.holdClone = true
        harness.runner.cloneProgress = listOf(GitTransferProgress(phase = "receivingPack", bytes = 3L * 1024 * 1024))
        val labels = Collections.synchronizedList(ArrayList<String>())
        var outcome: CloneToDeviceOutcome? = null
        val caller = Thread { outcome = clone(harness, progress = labels) }
        caller.start()
        expect(harness.runner.holding.await(20, TimeUnit.SECONDS))
        assertEq(listOf("Downloading 3.0 MB"), labels.toList())
        expect(harness.store.isBusy)
        harness.runner.release()
        caller.join(20_000)
        expect(outcome?.record != null)
        expect(!harness.store.isBusy)
    }

    fun testClonesAnonymouslyWhenSignedOut() = withHarness { harness ->
        val outcome = clone(harness, session = null)
        val credentials = harness.store.lastWorkspace!!.cloneCredentials!!
        expect(!credentials.canSign)
        assertEq(null, credentials.signer)
        assertEq("", credentials.pubkeyB64url)
        assertEq("https://forkmesh.test/net/catalog-repo", credentials.remoteUrl)
        expect(outcome.record != null)
    }

    fun testCancellingMidCloneLeavesNothingBehind() = withHarness { harness ->
        harness.runner.holdClone = true
        harness.runner.cloneProgress = listOf(GitTransferProgress(phase = "receivingPack", bytes = 2L * 1024 * 1024))
        var outcome: CloneToDeviceOutcome? = null
        val caller = Thread { outcome = clone(harness) }
        caller.start()
        expect(harness.runner.holding.await(20, TimeUnit.SECONDS))
        harness.store.cancelActive()
        caller.join(20_000)

        expect(harness.runner.cancelled)
        assertEq(null, outcome!!.record)
        assertEq("Operation cancelled.", outcome!!.message)
        expect(harness.store.list().isEmpty())
        expect(!File(harness.store.root, "catalog-repo").exists())
    }

    fun testRefusesAnOversizedRepositoryWithAClearMessage() = withHarness { harness ->
        harness.runner.cloneError = GitException(GitErrorCode.resourceLimitExceeded, "pack exceeds the 96 MB clone limit")
        val outcome = clone(harness)
        assertEq("This repository is too large to clone on this device.", outcome.message)
        assertEq(null, outcome.record)
        expect(harness.store.list().isEmpty())
    }

    fun testExplainsAnUnreachableNetworkReadably() = withHarness { harness ->
        harness.runner.cloneError = GitException(GitErrorCode.networkUnavailable, "connection closed")
        val outcome = clone(harness)
        assertEq(
            "No mirror is serving net/catalog-repo right now.\n" +
                "• the relay (forkmesh.test): Could not reach ForkMesh. Check your connection and try again.",
            outcome.message,
        )
        expect(!outcome.message.contains("GitException"))
        assertEq(null, outcome.record)
    }

    fun testExplainsARefusedDeviceInsteadOfAnHttpStatus() = withHarness { harness ->
        harness.runner.cloneError = GitException(GitErrorCode.remoteRejected, "remote returned HTTP 401")
        val outcome = clone(harness)
        expect(outcome.message.contains("ForkMesh refused this device"))
        expect(!outcome.message.contains("HTTP 401"))
        expect(!outcome.message.contains("GitException"))
    }

    fun testSurfacesAStoreRefusalReadably() = withHarness { harness ->
        harness.store.create(name = "catalog-repo", author = GitAuthor(name = "Alice", email = "alice@example.com"))
        assertEq("catalog-repo-2", suggestCloneName(harness.store.records, "catalog-repo"))
        val outcome = clone(harness, localName = "catalog-repo")
        assertEq("A local repository with that name already exists.", outcome.message)
        assertEq(1, harness.store.list().size)
    }

    fun testRefusesASecondCopyOfTheSameRepository() = withHarness { harness ->
        expect(clone(harness).record != null)
        val again = clone(harness, localName = "catalog-repo-2")
        assertEq(null, again.record)
        assertEq("catalog-repo", again.existing?.name)
        assertEq("net/catalog-repo is already on this device as catalog-repo. Open it and sync instead.", again.message)
        assertEq(1, harness.runner.ops.count { it == "clone" })
        assertEq(1, harness.store.list().size)
    }

    fun testRejectsAnInvalidFolderNameWithoutTouchingTheEngine()= withHarness { harness ->
        assertEq("Use letters, digits, dots, dashes, or underscores only.", LocalRepoStore.validateName("has space"))
        expect(harness.runner.ops.isEmpty())
        val outcome = clone(harness, localName = "has space")
        assertEq("Use letters, digits, dots, dashes, or underscores only.", outcome.message)
        expect(harness.runner.ops.isEmpty())
    }

    fun testCloneNameSuggestionsSanitizeAndCount() {
        val taken = (listOf("x") + (2 until 100).map { "x-$it" }).map { LocalRepoRecord(name = it, path = "/tmp/$it", createdAt = 0) }
        assertEq("x", suggestCloneName(taken, "x"))
        assertEq("my-repo-", suggestCloneName(emptyList(), "my repo!"))
    }
}
