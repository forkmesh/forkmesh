package com.forkmesh.nativeapp.git

import com.forkmesh.nativeapp.git.repository.isShallow
import com.forkmesh.nativeapp.git.repository.shallowCommits
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import java.io.File

class GitFetchOrchestrationTest {
    private val identity: GitIdentity
        get() = GitIdentity(name = "Ada", email = "ada@example.test", timestampSeconds = 0, timezoneOffsetMinutes = 0)

    private fun commit(repository: GitRepository, file: String, content: String, message: String): GitObjectId {
        repository.worktree.child(file).writeText(content)
        repository.add(listOf(GitWorktreePath(file)))
        return repository.commit(GitCommitRequest(message = message, author = identity, committer = identity))
    }

    private inline fun withServer(block: (serverRoot: File, serverRepository: GitRepository) -> Unit) =
        withTempDir("forkmesh-fake-remote-") { serverRoot ->
            block(serverRoot, GitRepository.init(GitRepositoryInit(serverRoot)))
        }

    private val notesUrl: GitRemoteUrl get() = GitRemoteUrl.parse("https://forkmesh.test/ada/notes")

    fun testCloneFetchesThePackUpdatesRefsHeadAndTheWorktree() = withServer { serverRoot, serverRepository ->
        val tip = commit(serverRepository, "hello.txt", "hello\n", "one\n")
        val pack = serverRepository.writePack(listOf(tip))
        val transport = FakeTransport { request ->
            if (request.method == "GET") return@FakeTransport okResponse(request, uploadPackAdvertisementBody())
            val body = latin1Text(request.body)
            if (body.contains("command=ls-refs")) {
                return@FakeTransport okResponse(
                    request,
                    lsRefsBody(listOf("${tip.hex} HEAD symref-target:refs/heads/main", "${tip.hex} refs/heads/main")),
                )
            }
            expect(body.contains("want ${tip.hex}"), "fetch wants the tip")
            expect(body.contains("done"), "a fresh clone sends done")
            okResponse(request, packfileBody(pack.packBytes))
        }

        val destination = serverRoot.child("clone-target")
        val operation = GitRepository.clone(
            GitCloneRequest(
                url = notesUrl,
                worktree = destination,
                access = GitRemoteAccess(transport = transport),
                detector = ClearDetector(),
            ),
        )
        val result = operation.value

        assertEq("main", result.checkedOutBranch)
        assertEq("refs/remotes/origin/main", result.fetch.updates.single().ref)
        assertEq(3, result.fetch.objects)
        val cloned = result.repository
        assertEq(GitSymbolicRef("refs/heads/main"), cloned.readHead())
        assertEq(tip, cloned.readHeadObjectId())
        assertEq("hello\n", destination.child("hello.txt").readText())
        val config = cloned.readConfig()
        assertEq("forkmesh.test", config.remotes.getValue("origin").url.uri.host)
        assertEq("origin", config.branches.getValue("main").remote)
        assertEq(3, transport.requests.size)
        assertEq("version=2", transport.requests.first().request.headers["git-protocol"])
        expect(!transport.closed)
    }

    fun testFetchNegotiatesWithHavesAndInstallsOnlyTheDeltaOfHistory() = withServer { serverRoot, serverRepository ->
        val base = commit(serverRepository, "a.txt", "a\n", "base\n")

        val localRoot = serverRoot.child("local")
        val basePack = serverRepository.writePack(listOf(base))
        val seedTransport = FakeTransport { request ->
            if (request.method == "GET") return@FakeTransport okResponse(request, uploadPackAdvertisementBody())
            val body = latin1Text(request.body)
            if (body.contains("command=ls-refs")) {
                return@FakeTransport okResponse(
                    request,
                    lsRefsBody(listOf("${base.hex} HEAD symref-target:refs/heads/main", "${base.hex} refs/heads/main")),
                )
            }
            okResponse(request, packfileBody(basePack.packBytes))
        }
        val local = GitRepository.clone(
            GitCloneRequest(
                url = notesUrl,
                worktree = localRoot,
                access = GitRemoteAccess(transport = seedTransport),
                detector = ClearDetector(),
            ),
        ).value

        val tip = commit(serverRepository, "b.txt", "b\n", "next\n")
        val baseObjects = HashSet<GitObjectId>()
        val reader = GitPackReader.parse(basePack.packBytes, GitResourceLimits())
        for (packObject in reader.objects) {
            baseObjects.add(packObject.objectId)
        }
        val deltaPack = serverRepository.writePack(listOf(tip), exclude = baseObjects)

        var sawAckRound = false
        val transport = FakeTransport { request ->
            if (request.method == "GET") return@FakeTransport okResponse(request, uploadPackAdvertisementBody())
            val body = latin1Text(request.body)
            if (body.contains("command=ls-refs")) {
                return@FakeTransport okResponse(
                    request,
                    lsRefsBody(listOf("${tip.hex} HEAD symref-target:refs/heads/main", "${tip.hex} refs/heads/main")),
                )
            }
            expect(body.contains("want ${tip.hex}"), "fetch wants the tip")
            if (body.contains("have ${base.hex}") && !body.contains("done")) {
                sawAckRound = true
                return@FakeTransport okResponse(
                    request,
                    packfileBody(deltaPack.packBytes, acknowledgments = listOf("ACK ${base.hex}", "ready")),
                )
            }
            okResponse(request, packfileBody(deltaPack.packBytes))
        }

        val result = local.repository.fetch(GitFetchRequest(access = GitRemoteAccess(transport = transport))).value

        expect(sawAckRound, "the first round acknowledged the common base")
        assertEq(1, result.negotiationRounds)
        assertEq(3, result.objects)
        assertEq("refs/remotes/origin/main", result.updates.single().ref)
        assertEq(tip, result.updates.single().newId)
        assertEq(base, result.updates.single().oldId)
        val tree = local.repository.readTree(local.repository.readCommit(tip).tree)
        assertEq(
            "b\n",
            utf8Text(local.repository.readBlob(tree.entries.first { utf8Text(it.name) == "b.txt" }.objectId)),
        )
        val fetchHead = local.repository.gitDirectory.child("FETCH_HEAD").readText()
        expect(fetchHead.contains(tip.hex), "FETCH_HEAD names the tip")
        expect(fetchHead.contains("branch 'main' of https://forkmesh.test"), "FETCH_HEAD names the branch: $fetchHead")
    }

    fun testFetchIsANoOpWhenTheRemoteHasNothingNew() = withServer { serverRoot, serverRepository ->
        val tip = commit(serverRepository, "a.txt", "a\n", "base\n")
        val pack = serverRepository.writePack(listOf(tip))
        val localRoot = serverRoot.child("local2")
        val transport = FakeTransport { request ->
            if (request.method == "GET") return@FakeTransport okResponse(request, uploadPackAdvertisementBody())
            val body = latin1Text(request.body)
            if (body.contains("command=ls-refs")) {
                return@FakeTransport okResponse(
                    request,
                    lsRefsBody(listOf("${tip.hex} HEAD symref-target:refs/heads/main", "${tip.hex} refs/heads/main")),
                )
            }
            okResponse(request, packfileBody(pack.packBytes))
        }
        val local = GitRepository.clone(
            GitCloneRequest(
                url = notesUrl,
                worktree = localRoot,
                access = GitRemoteAccess(transport = transport),
                detector = ClearDetector(),
            ),
        ).value
        val requestsAfterClone = transport.requests.size

        val result = local.repository.fetch(GitFetchRequest(access = GitRemoteAccess(transport = transport))).value

        assertEq(0, result.packBytes)
        assertEq(0, result.objects)
        assertEq(requestsAfterClone + 2, transport.requests.size)
    }

    fun testAPackThatOmitsAWantedObjectIsRejectedAndNothingIsInstalled() = withServer { serverRoot, serverRepository ->
        val tip = commit(serverRepository, "a.txt", "a\n", "base\n")
        val bogusPack = GitPackWriter.write(listOf(GitPackWriteObject(GitObjectType.blob, utf8("unrelated\n"))))
        val transport = FakeTransport { request ->
            if (request.method == "GET") return@FakeTransport okResponse(request, uploadPackAdvertisementBody())
            val body = latin1Text(request.body)
            if (body.contains("command=ls-refs")) {
                return@FakeTransport okResponse(request, lsRefsBody(listOf("${tip.hex} refs/heads/main")))
            }
            okResponse(request, packfileBody(bogusPack.packBytes))
        }
        val destination = serverRoot.child("broken-clone")

        val operation = GitRepository.clone(
            GitCloneRequest(
                url = notesUrl,
                worktree = destination,
                access = GitRemoteAccess(transport = transport),
                detector = ClearDetector(),
            ),
        )
        val result = operation.result

        expect(result is GitFailure, "clone failed")
        assertEq(GitErrorCode.packCorrupt, (result as GitFailure).code)
        expect(!destination.exists())
    }

    fun testShallowCloneDepthOutsideTheWalkBoundIsRejectedWithATypedError() {
        assertGitError(GitErrorCode.unsupportedRepositoryFormat) {
            GitCloneRequest(url = notesUrl, worktree = File("unused"), depth = 0)
        }
    }

    fun testAShallowSingleBranchCloneDeepensWritesTheShallowFileAndSkipsTags() = withServer { serverRoot, serverRepository ->
        commit(serverRepository, "a.txt", "one\n", "one\n")
        val middle = commit(serverRepository, "a.txt", "two\n", "two\n")
        val tip = commit(serverRepository, "a.txt", "three\n", "three\n")
        val pack = serverRepository.writePack(listOf(tip), exclude = setOf(middle))
        val transport = FakeTransport { request ->
            if (request.method == "GET") return@FakeTransport okResponse(request, uploadPackAdvertisementBody())
            val body = latin1Text(request.body)
            if (body.contains("command=ls-refs")) {
                return@FakeTransport okResponse(
                    request,
                    lsRefsBody(
                        listOf(
                            "${tip.hex} HEAD symref-target:refs/heads/main",
                            "${tip.hex} refs/heads/main",
                            "${middle.hex} refs/heads/old",
                            "${middle.hex} refs/tags/v1",
                        ),
                    ),
                )
            }
            expect(body.contains("deepen 1\n"), "fetch asks for depth 1")
            expect(body.contains("want ${tip.hex}"), "fetch wants the branch tip")
            expect(!body.contains("want ${middle.hex}"), "no other branch or tag is wanted")
            okResponse(
                request,
                packets(
                    listOf(dataLine("shallow-info\n"), GitPktLine.data(utf8("shallow ${tip.hex}")), GitPktLine.delimiter()) +
                        GitPktLine.decodeAll(packfileBody(pack.packBytes)),
                ),
            )
        }
        val destination = serverRoot.child("shallow-clone")

        val result = GitRepository.clone(
            GitCloneRequest(
                url = notesUrl,
                worktree = destination,
                depth = 1,
                singleBranch = true,
                access = GitRemoteAccess(transport = transport),
                detector = ClearDetector(),
            ),
        ).value

        val cloned = result.repository
        assertEq("main", result.checkedOutBranch)
        assertEq(setOf(tip), cloned.shallowCommits())
        expect(cloned.isShallow)
        assertEq("${tip.hex}\n", destination.child(".git").child("shallow").readText())
        assertEq(listOf(tip), cloned.log(GitLogRequest()).map { it.id })
        assertEq(listOf("refs/remotes/origin/main"), result.fetch.updates.map { it.ref })
        assertEq(
            "+refs/heads/main:refs/remotes/origin/main",
            cloned.readConfig().remotes.getValue("origin").fetch.value,
        )
        assertEq("three\n", destination.child("a.txt").readText())
        cloned.writePack(listOf(tip))
    }

    fun testAFetchRequestCarriesShallowAndDeepenLines() {
        val id = GitObjectId.parseSha1("a".repeat(40))
        val text = latin1Text(
            packets(
                GitProtocolV2.fetchRequest(
                    wants = listOf(id),
                    done = true,
                    shallow = listOf(id),
                    deepen = 25,
                ),
            ),
        )
        expect(text.contains("shallow ${id.hex}\n"), "shallow line")
        expect(text.contains("deepen 25\n"), "deepen line")
        assertGitError(GitErrorCode.protocolUnsupported) {
            GitProtocolV2.fetchRequest(wants = listOf(id), done = true, deepen = 0)
        }
    }

    fun testShallowInfoLinesMayOmitTheTrailingNewline() {
        val one = GitObjectId.parseSha1("1".repeat(40))
        val two = GitObjectId.parseSha1("2".repeat(40))
        val response = GitFetchResponse.parse(
            listOf(
                dataLine("shallow-info\n"),
                dataLine("shallow ${one.hex}\n"),
                GitPktLine.data(utf8("unshallow ${two.hex}")),
                GitPktLine.delimiter(),
                dataLine("packfile\n"),
                GitPktLine.data(byteArrayOf(1) + utf8("PACK")),
                GitPktLine.flush(),
            ),
        )
        assertEq(listOf(one), response.shallow)
        assertEq(listOf(two), response.unshallow)
    }

    fun testUntrustedPacksWithoutACollisionDetectorFailClosed() = withServer { serverRoot, serverRepository ->
        val tip = commit(serverRepository, "a.txt", "a\n", "base\n")
        val pack = serverRepository.writePack(listOf(tip))
        val transport = FakeTransport { request ->
            if (request.method == "GET") return@FakeTransport okResponse(request, uploadPackAdvertisementBody())
            val body = latin1Text(request.body)
            if (body.contains("command=ls-refs")) {
                return@FakeTransport okResponse(request, lsRefsBody(listOf("${tip.hex} refs/heads/main")))
            }
            okResponse(request, packfileBody(pack.packBytes))
        }
        val destination = serverRoot.child("unscreened-clone")

        val result = GitRepository.clone(
            GitCloneRequest(url = notesUrl, worktree = destination, access = GitRemoteAccess(transport = transport)),
        ).result

        expect(result is GitFailure, "clone failed")
        assertEq(GitErrorCode.hashScreeningUnavailable, (result as GitFailure).code)
        expect(!destination.exists())
    }
}
