package com.forkmesh.nativeapp.codeinsight

import com.forkmesh.nativeapp.git.SystemGit
import com.forkmesh.nativeapp.git.repository.isShallow
import com.forkmesh.nativeapp.git.withTempDir
import com.forkmesh.nativeapp.localrepos.ForkMeshRemoteCredentials
import com.forkmesh.nativeapp.localrepos.LocalGitOrigin
import com.forkmesh.nativeapp.localrepos.LocalGitSmartServer
import com.forkmesh.nativeapp.localrepos.SeededIdentity
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.assertFails
import com.forkmesh.nativeapp.testing.expect
import java.io.File

class InsightAnalysisCopyTest {
    private fun credentials(repo: String) = ForkMeshRemoteCredentials(
        origin = "https://forkmesh.test",
        owner = "forkmesh",
        repo = repo,
        signer = SeededIdentity(),
        pubkeyB64url = "device-key",
    )

    fun testCopyIsClonedShallowThenFetchedAndDeepened() = withTempDir("forkmesh-insight-copy-") { temp ->
        if (SystemGit.skipped("InsightAnalysisCopyTest.testCopyIsClonedShallowThenFetchedAndDeepened")) return@withTempDir
        val origin = LocalGitOrigin.make(File(temp, "origin").also { it.mkdirs() }, commits = 6)
        val server = LocalGitSmartServer()
        server.serve("https://forkmesh.test/forkmesh/demo", origin.bare)
        val copies = InsightAnalysisCopy(File(temp, "cache"), transport = { server })
        val stages = ArrayList<String>()

        val first = copies.sync("forkmesh", "demo", credentials("demo"), emptyList(), depth = 2) { stages.add(it) }
        assertEq(File(File(File(temp, "cache"), "forkmesh"), "demo"), first.directory)
        assertEq("main", first.branch)
        assertEq(origin.tip("main"), first.tip)
        expect(first.message.startsWith("Downloaded forkmesh/demo"), first.message)
        expect(stages.isNotEmpty(), "progress was reported")
        val shallow = InsightGitSource.open(first.directory, "demo", "forkmesh/demo")
        expect(shallow.repository.isShallow, "the first copy is shallow")
        assertEq(2, InsightAnalysisCopy.reachable(shallow.repository, InsightAnalysisCopy.tipOf(shallow.repository)!!, 100))
        assertEq(1, shallow.history(InsightOptions(historyDays = 100000), { false })!!.size)

        origin.commit("step.txt", "step 7\n", "Step 7")
        origin.publish("main")
        val second = copies.sync("forkmesh", "demo", credentials("demo"), emptyList(), depth = 5)
        assertEq(origin.tip("main"), second.tip)
        expect(second.message.startsWith("Fetched forkmesh/demo"), second.message)
        val deeper = InsightGitSource.open(second.directory, "demo", "forkmesh/demo")
        val tip = InsightAnalysisCopy.tipOf(deeper.repository)!!
        assertEq(origin.tip("main"), tip.hex)
        expect(InsightAnalysisCopy.reachable(deeper.repository, tip, 100) >= 5, "the copy was deepened")
        val again = copies.sync("forkmesh", "demo", credentials("demo"), emptyList(), depth = 5)
        assertEq(second.tip, again.tip)
    }

    fun testAnUnusableFolderIsMovedAsideNotDeleted() = withTempDir("forkmesh-insight-aside-") { temp ->
        if (SystemGit.skipped("InsightAnalysisCopyTest.testAnUnusableFolderIsMovedAsideNotDeleted")) return@withTempDir
        val origin = LocalGitOrigin.make(File(temp, "origin").also { it.mkdirs() }, commits = 2)
        val server = LocalGitSmartServer()
        server.serve("https://forkmesh.test/forkmesh/other", origin.bare)
        val copies = InsightAnalysisCopy(File(temp, "cache"), transport = { server }, clock = { 42 })
        val junk = copies.directoryFor("forkmesh", "other")
        junk.mkdirs()
        File(junk, "note.txt").writeText("keep me\n")

        val state = copies.sync("forkmesh", "other", credentials("other"), emptyList(), depth = 1)
        assertEq(origin.tip("main"), state.tip)
        assertEq("keep me\n", File(File(junk.parentFile, "other.unusable-42"), "note.txt").readText())
    }

    fun testFailuresAreReportedAndUnsafeNamesRefused() = withTempDir("forkmesh-insight-fail-") { temp ->
        val server = LocalGitSmartServer()
        val copies = InsightAnalysisCopy(File(temp, "cache"), transport = { server })
        val failure = assertFails<InsightCopyException> {
            copies.sync("forkmesh", "missing", credentials("missing"), emptyList(), depth = 1)
        }
        expect(failure.message.contains("forkmesh/missing"), failure.message)
        expect(!copies.exists("forkmesh", "missing"), "nothing was left behind")
        assertFails<InsightCopyException> { copies.directoryFor("..", "x") }
    }
}
