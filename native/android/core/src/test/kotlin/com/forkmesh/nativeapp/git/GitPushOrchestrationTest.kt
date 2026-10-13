package com.forkmesh.nativeapp.git

import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.assertFails
import com.forkmesh.nativeapp.testing.expect

class GitPushOrchestrationTest {
    private val identity: GitIdentity
        get() = GitIdentity(name = "Ada", email = "ada@example.test", timestampSeconds = 0, timezoneOffsetMinutes = 0)

    private fun commit(repository: GitRepository, file: String, content: String, message: String): GitObjectId {
        repository.worktree.child(file).writeText(content)
        repository.add(listOf(GitWorktreePath(file)))
        return repository.commit(GitCommitRequest(message = message, author = identity, committer = identity))
    }

    private inline fun withRepository(block: (repository: GitRepository) -> Unit) =
        withTempDir("forkmesh-push-") { root ->
            val repository = GitRepository.init(GitRepositoryInit(root))
            repository.setRemote("origin", GitRemoteUrl.parse("https://forkmesh.test/ada/notes"))
            block(repository)
        }

    fun testPushesAFastForwardWithCompareAndSwapCommandsAndADeltaPack() = withRepository { repository ->
        val base = commit(repository, "a.txt", "a\n", "base\n")
        val tip = commit(repository, "b.txt", "b\n", "next\n")

        var postedBody: ByteArray? = null
        val transport = FakeTransport { request ->
            if (request.method == "GET") {
                return@FakeTransport okResponse(request, receivePackAdvertisementBody(mapOf("refs/heads/main" to base)))
            }
            postedBody = request.body
            okResponse(request, sidebandReportBody(listOf("unpack ok", "ok refs/heads/main")))
        }

        val result = repository.push(GitPushRequest(access = GitRemoteAccess(transport = transport))).value

        assertEq(GitPushRefOutcome.updated, result.refs.single().outcome)
        assertEq(base, result.refs.single().oldId)
        assertEq(tip, result.refs.single().newId)
        val posted = postedBody!!
        val packets = GitPktLine.decodeAll(posted.copyOfRange(0, posted.size - result.packBytes))
        val command = latin1Text(packets.first().payload)
        expect(command.startsWith("${base.hex} ${tip.hex} refs/heads/main"), command)
        expect(command.contains("report-status-v2"), command)
        expect(!command.contains("delete-refs"), command)
        val pack = GitPackReader.parse(posted.copyOfRange(posted.size - result.packBytes, posted.size), GitResourceLimits())
        assertEq(pack.objects.size, result.objects)
        expect(pack.objects.map { it.objectId }.contains(tip))
        expect(!pack.objects.map { it.objectId }.contains(base))
        assertEq(1, pack.objects.count { it.type == GitObjectType.blob })
        val tracking = RefStore(repository.gitDirectory).readDirect(GitRefName.parse("refs/remotes/origin/main"))
        assertEq(tip, tracking)
    }

    fun testANonFastForwardPushIsRefusedBeforeAnyBytesAreSent() = withRepository { repository ->
        commit(repository, "a.txt", "a\n", "base\n")
        repository.createBranch(GitBranchName("divergent"))
        val localTip = commit(repository, "b.txt", "b\n", "local\n")
        repository.checkout(GitCheckoutRequest.branch(GitBranchName("divergent")))
        val divergent = commit(repository, "c.txt", "c\n", "remote\n")
        repository.checkout(GitCheckoutRequest.branch(GitBranchName("main")))

        val transport = FakeTransport { request ->
            if (request.method == "GET") {
                return@FakeTransport okResponse(request, receivePackAdvertisementBody(mapOf("refs/heads/main" to divergent)))
            }
            throw AssertionError("a non-fast-forward push must not POST")
        }

        val result = repository.push(GitPushRequest(access = GitRemoteAccess(transport = transport))).result

        expect(result is GitFailure, "push failed")
        assertEq(GitErrorCode.nonFastForward, (result as GitFailure).code)
        assertEq(1, transport.requests.size)
        assertEq(localTip, repository.readHeadObjectId())
    }

    fun testAnUnknownRemoteTipIsTreatedAsNonFastForward() = withRepository { repository ->
        commit(repository, "a.txt", "a\n", "base\n")
        val unknown = GitObjectId.parseSha1("9".repeat(40))
        val transport = FakeTransport { request ->
            if (request.method == "GET") {
                return@FakeTransport okResponse(request, receivePackAdvertisementBody(mapOf("refs/heads/main" to unknown)))
            }
            throw AssertionError("a push with an unknown remote tip must not POST")
        }

        val result = repository.push(GitPushRequest(access = GitRemoteAccess(transport = transport))).result

        expect(result is GitFailure, "push failed")
        assertEq(GitErrorCode.nonFastForward, (result as GitFailure).code)
        assertEq(1, transport.requests.size)
    }

    fun testAnUpToDatePushSendsNothing() = withRepository { repository ->
        val tip = commit(repository, "a.txt", "a\n", "base\n")
        val transport = FakeTransport { request ->
            if (request.method == "GET") {
                return@FakeTransport okResponse(request, receivePackAdvertisementBody(mapOf("refs/heads/main" to tip)))
            }
            throw AssertionError("an up-to-date push must not POST")
        }

        val result = repository.push(GitPushRequest(access = GitRemoteAccess(transport = transport))).value

        assertEq(GitPushRefOutcome.upToDate, result.refs.single().outcome)
        assertEq(0, result.packBytes)
    }

    fun testARemoteNgReportBecomesATypedRejection() = withRepository { repository ->
        commit(repository, "a.txt", "a\n", "base\n")
        val transport = FakeTransport { request ->
            if (request.method == "GET") {
                return@FakeTransport okResponse(request, receivePackAdvertisementBody(emptyMap()))
            }
            okResponse(request, sidebandReportBody(listOf("unpack ok", "ng refs/heads/main hook declined")))
        }

        val result = repository.push(GitPushRequest(access = GitRemoteAccess(transport = transport))).result

        expect(result is GitFailure, "push failed")
        assertEq(GitErrorCode.remoteRejected, (result as GitFailure).code)
    }

    fun testThePublicApiCannotExpressARefDeletion() = withRepository { repository ->
        commit(repository, "a.txt", "a\n", "base\n")
        assertFails<GitException> { GitPushRefspec.parse(":refs/heads/main") }
        assertFails<GitException> { GitPushRefspec(localBranch = "") }
        assertFails<GitException> {
            GitReceivePack.commandPackets(
                listOf(
                    GitPushRefUpdate(
                        refName = "refs/heads/main",
                        expectedOld = GitObjectId.parseSha1("1".repeat(40)),
                        next = GitReceivePack.zeroId,
                    ),
                ),
            )
        }
    }
}
