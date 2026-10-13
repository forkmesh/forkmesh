package com.forkmesh.nativeapp.localrepos

import com.forkmesh.nativeapp.git.SystemGit
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import java.io.File
import java.util.Collections

class LocalCheckoutTest {
    private val relayBase = "https://forkmesh.test/forkmesh/demo"

    private fun credentials() = ForkMeshRemoteCredentials(
        origin = "https://forkmesh.test",
        owner = "forkmesh",
        repo = "demo",
        signer = SeededIdentity(),
        pubkeyB64url = "device-key",
    )

    fun testACloneReportsIndexingAndCheckoutCounts() {
        if (SystemGit.skipped("LocalCheckoutTest.testACloneReportsIndexingAndCheckoutCounts")) return
        withScratch("fm-checkout-") { temp ->
            val origin = LocalGitOrigin.make(File(temp, "origin").also { it.mkdirs() }, commits = 1)
            File(origin.work, "src").mkdirs()
            for (index in 0 until 70) File(origin.work, "src/file$index.txt").writeText("file $index\n")
            SystemGit.git(origin.work, "add", "-A")
            SystemGit.git(
                origin.work,
                "-c", "user.name=Origin", "-c", "user.email=origin@example.test",
                "commit", "-q", "-m", "Many files",
            )
            origin.publish("main")
            val server = LocalGitSmartServer()
            server.serve(relayBase, origin.bare)
            val workspace = GitWorkspace(File(temp, "demo"))
            workspace.transportFactory = { server }
            try {
                val labels: MutableList<String> = Collections.synchronizedList(ArrayList())

                val result = workspace.clone(credentials(), onProgress = { labels.add(it.label) })

                assertEq("main", result.branch)
                expect(labels.contains("Indexing ${result.objects} objects"), labels.toString())
                expect(labels.contains("Checking out 64 of 71 files"), labels.toString())
                expect(labels.contains("Checking out 71 files"), labels.toString())
                expect(!labels.contains("Indexing 0 objects"), labels.toString())
                expect(workspace.status().isClean, "a fresh clone is clean")
                assertEq("file 7\n", workspace.readFile("src/file7.txt").text)
            } finally {
                workspace.dispose()
            }
        }
    }

    fun testProgressLabelsCountObjectsAndFiles() {
        assertEq("Indexing 5 of 10 objects", GitTransferProgress(phase = "indexingPack", objects = 5, total = 10).label)
        assertEq("Indexing 10 objects", GitTransferProgress(phase = "indexingPack", objects = 10, total = 10).label)
        assertEq("Indexing 10 objects", GitTransferProgress(phase = "indexingPack", total = 10).label)
        assertEq("Indexing 12 objects", GitTransferProgress(phase = "indexingPack", objects = 12).label)
        assertEq("Indexing 1 object", GitTransferProgress(phase = "indexingPack", total = 1).label)
        assertEq("Indexing objects", GitTransferProgress(phase = "indexingPack").label)
        assertEq("Checking out 3 of 9 files", GitTransferProgress(phase = "checkingOut", objects = 3, total = 9).label)
        assertEq("Checking out 1 file", GitTransferProgress(phase = "checkingOut", total = 1).label)
        assertEq("Checking out files", GitTransferProgress(phase = "checkingOut").label)
    }
}
