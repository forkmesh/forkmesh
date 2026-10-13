package com.forkmesh.nativeapp.localrepos

import com.forkmesh.nativeapp.git.GitCancellationToken
import com.forkmesh.nativeapp.git.repository.GitDeepen
import com.forkmesh.nativeapp.git.GitErrorCode
import com.forkmesh.nativeapp.git.GitException
import com.forkmesh.nativeapp.git.GitHttpRequest
import com.forkmesh.nativeapp.git.GitRepository
import com.forkmesh.nativeapp.git.GitRepositoryOpen
import com.forkmesh.nativeapp.git.SystemGit
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.assertFails
import com.forkmesh.nativeapp.testing.expect
import java.io.File
import java.net.URI

class LocalRemoteRedirectTest {
    private val relayBase = "https://forkmesh.test/forkmesh/demo"
    private val author = GitAuthor(name = "Phone", email = "phone@example.test")

    private fun mirrorBase(node: String) = "https://$node.test/forkmesh/demo.git"

    private fun credentials() = ForkMeshRemoteCredentials(
        origin = "https://forkmesh.test",
        owner = "forkmesh",
        repo = "demo",
        signer = SeededIdentity(),
        pubkeyB64url = "device-key",
    )

    private class Fixture(val temp: File, val origin: LocalGitOrigin, val server: LocalGitSmartServer) {
        val workspaces = ArrayList<GitWorkspace>()

        fun workspace(name: String = "demo"): GitWorkspace =
            GitWorkspace(File(temp, name)).also {
                it.transportFactory = { server }
                workspaces.add(it)
            }

        fun config(workspace: GitWorkspace): String = File(workspace.worktree, ".git/config").readText()
    }

    private fun withFixture(test: String, commits: Int, block: (Fixture) -> Unit) {
        if (SystemGit.skipped("LocalRemoteRedirectTest.$test")) return
        withScratch("fm-redirect-") { temp ->
            val fixture = Fixture(temp, LocalGitOrigin.make(File(temp, "origin").also { it.mkdirs() }, commits), LocalGitSmartServer())
            try {
                block(fixture)
            } finally {
                fixture.workspaces.forEach { it.dispose() }
            }
        }
    }

    fun testFetchFollowsTheRelaysRedirectToAMirrorUnsignedAndKeepsTheRelayAsOrigin() =
        withFixture("testFetchFollowsTheRelaysRedirectToAMirrorUnsignedAndKeepsTheRelayAsOrigin", commits = 3) { f ->
            f.server.serve(mirrorBase("mirror35"), f.origin.bare)
            f.server.redirect(relayBase, mirrorBase("mirror35"))
            val workspace = f.workspace()
            workspace.clone(credentials())
            expect(f.config(workspace).contains("url = $relayBase"), f.config(workspace))
            val tip = f.origin.commit("next.txt", "next\n", "Next")
            f.origin.publish("main")
            val before = f.server.requests.size

            val result = workspace.fetch(credentials())

            assertEq(listOf("refs/remotes/origin/main"), result.updatedRefs)
            assertEq("mirror35.test", result.servedBy)
            assertEq("Fetched 1 branch from mirror35.test.", fetchResultMessage(result))
            val fetchRequests = f.server.requests.drop(before)
            assertEq("$relayBase/info/refs?service=git-upload-pack", fetchRequests.first().url.toString())
            expect(fetchRequests.first().headers["x-forkmesh-sig"] != null, "the relay request is signed")
            val mirrored = fetchRequests.drop(1)
            expect(mirrored.isNotEmpty())
            expect(mirrored.all { it.url.host == "mirror35.test" })
            expect(mirrored.all { it.headers["x-forkmesh-sig"] == null && it.headers["x-forkmesh-pubkey"] == null })
            assertEq(tip, workspace.syncState("main").remoteTip)
            expect(f.config(workspace).contains("url = $relayBase"))
        }

    fun testAFetchWithoutARedirectNamesNoMirror() = withFixture("testAFetchWithoutARedirectNamesNoMirror", commits = 1) { f ->
        f.server.serve(relayBase, f.origin.bare)
        val workspace = f.workspace()
        workspace.clone(credentials())

        val result = workspace.fetch(credentials())

        assertEq(null, result.servedBy)
        assertEq("Already up to date with ForkMesh.", fetchResultMessage(result))
    }

    fun testRedirectTargetsMustBeCredentialFreeHttpsUploadPackAdvertisements() {
        val request = GitHttpRequest(
            method = "GET",
            url = URI("$relayBase/info/refs?service=git-upload-pack"),
            maxResponseBytes = 1024,
            expectedContentType = "application/x-git-upload-pack-advertisement",
        )
        val good = GitRedirectTarget.resolve(
            request,
            "https://mirror35.test/forkmesh/demo.git/info/refs?service=git-upload-pack",
            relayBase,
        )
        assertEq("https://mirror35.test/forkmesh/demo.git", good?.base)
        val relative = GitRedirectTarget.resolve(request, "/mirror/forkmesh/demo/info/refs?service=git-upload-pack", relayBase)
        assertEq("https://forkmesh.test/mirror/forkmesh/demo", relative?.base)
        for (location in listOf(
            "http://mirror35.test/forkmesh/demo.git/info/refs?service=git-upload-pack",
            "https://user:secret@mirror35.test/forkmesh/demo.git/info/refs?service=git-upload-pack",
            "https://mirror35.test/forkmesh/demo.git/info/refs?service=git-receive-pack",
            "https://mirror35.test/forkmesh/demo.git/git-upload-pack",
            "$relayBase/info/refs?service=git-upload-pack",
            "",
            null,
        )) {
            assertEq(null, GitRedirectTarget.resolve(request, location, relayBase), location ?: "null")
        }
        val push = GitHttpRequest(
            method = "GET",
            url = URI("$relayBase/info/refs?service=git-receive-pack"),
            maxResponseBytes = 1024,
            expectedContentType = "application/x-git-receive-pack-advertisement",
        )
        assertEq(
            null,
            GitRedirectTarget.resolve(push, "https://mirror35.test/forkmesh/demo.git/info/refs?service=git-upload-pack", relayBase),
        )
    }

    fun testASecondRedirectIsNeverFollowed() = withFixture("testASecondRedirectIsNeverFollowed", commits = 1) { f ->
        f.server.redirect(relayBase, mirrorBase("mirror35"))
        f.server.redirect(mirrorBase("mirror35"), mirrorBase("mirror36"))
        f.server.serve(mirrorBase("mirror36"), f.origin.bare)
        val workspace = f.workspace()

        val error = assertFails<GitWorkspaceException> { workspace.clone(credentials()) }

        assertEq(GitErrorCode.remoteRejected, error.code)
        expect(f.server.requested("mirror36.test").isEmpty())
    }

    fun testAShallowPushDeepensUntilTheFastForwardIsProven() =
        withFixture("testAShallowPushDeepensUntilTheFastForwardIsProven", commits = 40) { f ->
            f.origin.branch("old", f.origin.revParse("HEAD~30"))
            f.origin.publish("old")
            f.server.serve(relayBase, f.origin.bare)
            val workspace = f.workspace()
            workspace.clone(credentials(), depth = 5)
            workspace.createBranch("old", checkout = true)
            workspace.writeFile("phone.txt", "from the phone\n")
            workspace.stageAll()
            val local = workspace.commit("Phone work\n", author)

            val result = workspace.push(credentials(), branch = "old")

            assertEq(listOf("updated"), result.refs.map { it.outcome })
            assertEq(local, f.origin.tip("refs/heads/old"))
            expect(f.server.fetchBodies.any { it.contains("deepen 50\n") && it.contains("deepen-relative\n") })
        }

    fun testAShallowPushOntoADivergedBranchSaysWhatToDoWithoutDeepening() =
        withFixture("testAShallowPushOntoADivergedBranchSaysWhatToDoWithoutDeepening", commits = 10) { f ->
            f.server.serve(relayBase, f.origin.bare)
            val workspace = f.workspace()
            workspace.clone(credentials(), depth = 3)
            f.origin.commit("step.txt", "theirs\n", "Upstream change")
            f.origin.publish("main")
            workspace.writeFile("phone.txt", "mine\n")
            workspace.stageAll()
            workspace.commit("Phone work\n", author)
            val pushedBefore = f.origin.tip("refs/heads/main")
            val cloneFetches = f.server.fetchBodies.size

            val error = assertFails<GitWorkspaceException> { workspace.push(credentials(), branch = "main") }

            assertEq("push_diverged", error.detail)
            assertEq(GitErrorCode.nonFastForward, error.code)
            expect(error.message.contains("Push your work to a new branch"), error.message)
            assertEq(pushedBefore, f.origin.tip("refs/heads/main"))
            val proofFetches = f.server.fetchBodies.drop(cloneFetches)
            expect(proofFetches.isNotEmpty())
            expect(proofFetches.none { it.contains("deepen") })
        }

    fun testTheProofStopsAtItsBoundWithAClearError() = withFixture("testTheProofStopsAtItsBoundWithAClearError", commits = 30) { f ->
        f.origin.branch("old", f.origin.revParse("HEAD~20"))
        f.origin.publish("old")
        f.server.serve(relayBase, f.origin.bare)
        val workspace = f.workspace()
        workspace.clone(credentials(), depth = 2)
        workspace.createBranch("old", checkout = true)
        val repo = GitRepository.open(GitRepositoryOpen(workspace.worktree, detector = workspace.detector))
        val ops = GitWorkspaceOps(workspace.worktree, workspace.detector) { f.server }
        val token = GitCancellationToken()
        val context = GitWorkContext(token) {}
        val steps = ArrayList<GitDeepen?>()

        val bounded = assertFails<GitWorkspaceException> {
            ShallowPushProof.prove(
                repo,
                branch = "old",
                remote = "origin",
                schedule = listOf(GitDeepen.Relative(1)),
                fetch = { refspec, deepen ->
                    steps.add(deepen)
                    ops.fetchInto(context, repo, credentials(), "origin", refspec, deepen, route = null)
                },
                token = token,
            )
        }
        assertEq("shallow_history_limit", bounded.detail)
        expect(bounded.message.contains("push from a full clone on a computer"), bounded.message)
        assertEq(listOf(null, GitDeepen.Relative(1)), steps)

        val limited = assertFails<GitWorkspaceException> {
            ShallowPushProof.prove(
                repo,
                branch = "old",
                remote = "origin",
                fetch = { _, _ -> throw GitException(GitErrorCode.resourceLimitExceeded, "pack exceeds the compressed byte limit") },
                token = token,
            )
        }
        assertEq("shallow_history_limit", limited.detail)
    }
}
