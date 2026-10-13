package com.forkmesh.nativeapp.localrepos

import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.api.FakeTransport
import com.forkmesh.nativeapp.api.MemoryPrefStore
import com.forkmesh.nativeapp.api.TestSettings
import com.forkmesh.nativeapp.api.jsonResponse
import com.forkmesh.nativeapp.git.SystemGit
import com.forkmesh.nativeapp.model.Repository
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import java.io.File

class LocalSyncTest {
    private val relayOrigin = "https://forkmesh.test"
    private val author = GitAuthor(name = "Phone", email = "phone@example.test")

    private fun credentials(repo: String = "demo") = ForkMeshRemoteCredentials(origin = relayOrigin, owner = "forkmesh", repo = repo)

    private class ServedStore(root: File, val server: LocalGitSmartServer) : LocalRepoStore(root) {
        override fun workspace(record: LocalRepoRecord): GitWorkspace =
            super.workspace(record).also { it.transportFactory = { server } }

        override fun workspace0(directory: File): GitWorkspace =
            super.workspace0(directory).also { it.transportFactory = { server } }
    }

    private class Fixture(val origin: LocalGitOrigin, val store: ServedStore, val publisher: PublishService)

    private fun withFixture(test: String, repos: List<String> = listOf("demo"), block: (Fixture) -> Unit) {
        if (SystemGit.skipped("LocalSyncTest.$test")) return
        withScratch("fm-sync-") { temp ->
            val origin = LocalGitOrigin.make(File(temp, "origin").also { it.mkdirs() }, commits = 2)
            val server = LocalGitSmartServer()
            for (repo in repos) server.serve("$relayOrigin/forkmesh/$repo", origin.bare)
            val api = ApiService(TestSettings(serverUrl = "wss://forkmesh.test/ws"), FakeTransport { jsonResponse(emptyMap<String, Any?>()) })
            val publisher = PublishService(api, identity = { SeededIdentity() }, prefs = MemoryPrefStore())
            block(Fixture(origin, ServedStore(File(temp, "repos"), server), publisher))
        }
    }

    private fun result(outcome: LocalPullOutcome) =
        LocalPullResult("main", LocalFetchResult(updatedRefs = emptyList(), packBytes = 0, objects = 0, remoteHead = null), outcome)

    fun testPullResultsSpeakOfSyncing() {
        assertEq("Up to date", result(LocalPullOutcome.UpToDate).title)
        assertEq("Up to date", result(LocalPullOutcome.Ahead).title)
        val forwarded = result(LocalPullOutcome.FastForwarded("a", "b"))
        assertEq("Synced", forwarded.title)
        assertEq("Synced main with ForkMesh.", forwarded.message)
        expect(forwarded.changedWorktree)
        assertEq("Sync needs attention", result(LocalPullOutcome.Diverged).title)
        expect(result(LocalPullOutcome.Diverged).needsAttention)
        expect(result(LocalPullOutcome.Undetermined).needsAttention)
        expect(!result(LocalPullOutcome.Ahead).needsAttention)
        assertEq("Nothing to sync", result(LocalPullOutcome.NoRemoteBranch).title)
        assertEq(
            "Nothing to sync: main here already contains ForkMesh's. Push to share your commits.",
            result(LocalPullOutcome.Ahead).message,
        )
        expect(result(LocalPullOutcome.Diverged).message.contains("this device only syncs fast-forwards"))
        assertEq("Commit or discard your changes before syncing main.", syncGuardMessage("main"))
        assertEq("Synced main with ForkMesh.", pullResultMessage(forwarded))
        val mirrored = forwarded.copy(fetch = forwarded.fetch.copy(servedBy = "mirror35.test"))
        assertEq("Synced main with ForkMesh. (from mirror35.test)", pullResultMessage(mirrored))
    }

    fun testSyncSummariesAndLabels() {
        val demo = LocalRepoRecord(name = "demo", path = "/demo", createdAt = 0, publishedAs = "forkmesh/demo", origin = LocalRepoOrigin.cloned)
        val web = LocalRepoRecord(name = "web", path = "/web", createdAt = 0, publishedAs = "alice/web")
        val scratch = LocalRepoRecord(name = "scratch", path = "/scratch", createdAt = 0)
        assertEq(
            "Synced 2 of 3 repositories · 1 updated · check web · failed: scratch",
            localSyncSummary(
                results = listOf(demo to result(LocalPullOutcome.FastForwarded("a", "b")), web to result(LocalPullOutcome.Diverged)),
                failures = listOf(scratch to "offline"),
            ),
        )
        assertEq("demo: main is already up to date with ForkMesh.", localSyncSummary(listOf(demo to result(LocalPullOutcome.UpToDate)), emptyList()))
        assertEq("scratch: offline", localSyncSummary(emptyList(), listOf(scratch to "offline")))
        assertEq(listOf("demo", "web"), syncableRecords(listOf(demo, web, scratch)).map { it.name })
        assertEq("Sync all", syncAllLabel(listOf(demo, web, scratch)))
        assertEq("Sync clones", syncAllLabel(listOf(demo, scratch)))
        assertEq("forkmesh" to "demo", demo.meshRoute)
        assertEq(null, scratch.meshRoute)
        assertEq(null, scratch.copyWith(publishedAs = "/demo").meshRoute)
        assertEq(null, scratch.copyWith(publishedAs = "forkmesh/").meshRoute)
    }

    fun testARepositoryOnTheDeviceIsFoundAndNeverClonedTwice() = withFixture("testARepositoryOnTheDeviceIsFoundAndNeverClonedTwice") { f ->
        val record = f.store.cloneFromMesh(name = "demo", credentials = credentials())
        assertEq("forkmesh/demo", record.publishedAs)
        assertEq("demo", f.store.record(" ForkMesh/Demo ")?.name)
        assertEq("demo", f.store.recordFor(Repository(owner = "forkmesh", name = "demo"))?.name)
        assertEq("demo", f.store.recordFor(Repository(owner = "mirror-node", name = "demo", publicOwner = "forkmesh"))?.name)
        assertEq(null, f.store.recordFor(Repository(owner = "forkmesh", name = "other")))
        assertEq(null, f.store.record(""))

        val refusal = try {
            f.store.cloneFromMesh(name = "demo-2", credentials = credentials())
            null
        } catch (error: LocalRepoStoreException) {
            error.message
        }
        assertEq("forkmesh/demo is already on this device as demo. Open it and sync instead.", refusal)
        assertEq(listOf("demo"), f.store.list().map { it.name })
        expect(!File(f.store.root, "demo-2").exists())
    }

    fun testStoreSyncPullsTheCurrentBranchAndRefreshesTheRecord() = withFixture("testStoreSyncPullsTheCurrentBranchAndRefreshesTheRecord") { f ->
        val record = f.store.cloneFromMesh(name = "demo", credentials = credentials())
        val tip = f.origin.commit("later.txt", "later\n", "Later upstream")
        f.origin.publish("main")

        val labels = ArrayList<String>()
        val synced = f.store.sync(record, f.publisher) { labels.add(it.label) }

        expect(labels.contains("Checking out 1 file"), labels.toString())
        val outcome = synced.outcome
        expect(outcome is LocalPullOutcome.FastForwarded, "$outcome")
        assertEq(tip, (outcome as LocalPullOutcome.FastForwarded).to)
        assertEq("Synced main with ForkMesh.", synced.message)
        assertEq("later\n", File(record.path, "later.txt").readText())
        assertEq("Later upstream", f.store.records.single().lastCommitSubject)
        expect(!f.store.isBusy)
        val workspace = f.store.workspace(record)
        try {
            expect(workspace.status().isClean)
            assertEq(tip, workspace.summary().headId)
        } finally {
            workspace.dispose()
        }

        assertEq(LocalPullOutcome.UpToDate, f.store.sync(record, f.publisher).outcome)

        val solo = LocalRepoRecord(name = "solo", path = "/tmp/solo", createdAt = 0)
        val refusal = try {
            f.store.sync(solo, f.publisher)
            null
        } catch (error: LocalRepoStoreException) {
            error.message
        }
        assertEq("solo is only on this device. Publish it before syncing.", refusal)
    }

    fun testAShallowCloneSyncsAcrossItsBoundary() = withFixture("testAShallowCloneSyncsAcrossItsBoundary") { f ->
        val record = f.store.cloneFromMesh(name = "demo", credentials = credentials(), depth = 1)
        f.origin.commit("later.txt", "later\n", "Later upstream")
        val tip = f.origin.commit("step.txt", "step 3\n", "Step 3")
        f.origin.publish("main")

        val synced = f.store.sync(record, f.publisher)

        assertEq(LocalPullOutcome.FastForwarded(f.origin.revParse("HEAD~2"), tip), synced.outcome)
        assertEq("step 3\n", File(record.path, "step.txt").readText())
        assertEq("later\n", File(record.path, "later.txt").readText())
    }

    fun testSyncRefusesLocalChangesAndNeverRewritesLocalCommits() = withFixture("testSyncRefusesLocalChangesAndNeverRewritesLocalCommits") { f ->
        val record = f.store.cloneFromMesh(name = "demo", credentials = credentials())
        f.origin.commit("later.txt", "later\n", "Later upstream")
        f.origin.publish("main")
        File(record.path, "step.txt").writeText("edited here\n")
        val refusal = try {
            f.store.sync(record, f.publisher)
            null
        } catch (error: GitWorkspaceException) {
            error.message
        }
        assertEq("Commit or discard your changes before syncing main.", refusal)
        assertEq("edited here\n", File(record.path, "step.txt").readText())
        expect(!File(record.path, "later.txt").exists())

        val workspace = f.store.workspace(record)
        try {
            workspace.stageAll()
            workspace.commit("Local work", author)
        } finally {
            workspace.dispose()
        }
        val diverged = f.store.sync(record, f.publisher)
        assertEq(LocalPullOutcome.Diverged, diverged.outcome)
        expect(diverged.needsAttention)
        assertEq("Local work", f.store.records.single().lastCommitSubject)
        assertEq("edited here\n", File(record.path, "step.txt").readText())
    }

    fun testALocalCommitOnTopOfForkMeshIsAhead() = withFixture("testALocalCommitOnTopOfForkMeshIsAhead") { f ->
        val record = f.store.cloneFromMesh(name = "demo", credentials = credentials())
        File(record.path, "mine.txt").writeText("mine\n")
        val workspace = f.store.workspace(record)
        try {
            workspace.stageAll()
            workspace.commit("Mine", author)
        } finally {
            workspace.dispose()
        }
        val synced = f.store.sync(record, f.publisher)
        assertEq(LocalPullOutcome.Ahead, synced.outcome)
        assertEq("Up to date", synced.title)
    }

    fun testSyncAllRunsEveryPublishedRepositoryAndSummarises() =
        withFixture("testSyncAllRunsEveryPublishedRepositoryAndSummarises", repos = listOf("demo", "second")) { f ->
            f.store.cloneFromMesh(name = "demo", credentials = credentials())
            f.store.cloneFromMesh(name = "second", credentials = credentials("second"))
            f.store.create(name = "scratch", author = author)
            val records = f.store.list()
            assertEq(setOf("demo", "second"), syncableRecords(records).map { it.name }.toSet())
            assertEq("Sync clones", syncAllLabel(records))
            f.origin.commit("later.txt", "later\n", "Later upstream")
            f.origin.publish("main")
            val statuses = ArrayList<String>()
            val run = LocalRepoSyncRun(f.store, f.publisher)

            assertEq("Synced 2 of 2 repositories · 2 updated", run.run(syncableRecords(records)) { statuses.add(it) })

            expect(statuses.first().startsWith("Syncing "), statuses.first())
            expect(statuses.any { it.endsWith("(2 of 2)") }, statuses.toString())
            val demo = f.store.records.first { it.name == "demo" }
            assertEq("demo: main is already up to date with ForkMesh.", run.run(listOf(demo)))
            expect(!f.store.isBusy)
        }

    fun testCloneToDeviceRefusesASecondCopyBeforeLookingForMirrors() =
        withFixture("testCloneToDeviceRefusesASecondCopyBeforeLookingForMirrors") { f ->
            var lookups = 0
            fun clone(localName: String) = cloneToDevice(
                store = f.store,
                publisher = f.publisher,
                session = mobileNodeSession(),
                owner = "forkmesh",
                name = "demo",
                localName = localName,
                mirrors = { _, _ ->
                    lookups++
                    emptyList()
                },
            )
            val first = clone("demo")
            expect(first.record != null, first.message)
            assertEq(1, lookups)

            val again = clone("demo-2")

            assertEq(null, again.record)
            assertEq("demo", again.existing?.name)
            assertEq("forkmesh/demo is already on this device as demo. Open it and sync instead.", again.message)
            assertEq(1, lookups)
            assertEq(listOf("demo"), f.store.list().map { it.name })
        }
}
