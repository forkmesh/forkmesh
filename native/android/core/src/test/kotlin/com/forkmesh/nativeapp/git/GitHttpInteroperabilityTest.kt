package com.forkmesh.nativeapp.git

import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import com.sun.net.httpserver.HttpExchange
import java.io.File
import java.util.Collections

private class GitHttpBackendServer private constructor(val projectRoot: File, val certificateDer: ByteArray) {
    private lateinit var https: TestHttpsServer

    val requestHeaders: MutableList<Map<String, String>> = Collections.synchronizedList(ArrayList())

    fun repositoryUrl(name: String): String = "https://127.0.0.1:${https.port}/$name"

    private fun handle(exchange: HttpExchange) {
        try {
            val headers = LinkedHashMap<String, String>()
            for ((name, values) in exchange.requestHeaders) {
                headers[name.lowercase()] = values.joinToString(", ")
            }
            requestHeaders.add(headers)
            val bodyBytes = exchange.requestBody.use { it.readBytes() }
            val environment = LinkedHashMap<String, String>()
            environment["GATEWAY_INTERFACE"] = "CGI/1.1"
            environment["REQUEST_METHOD"] = exchange.requestMethod
            environment["PATH_INFO"] = exchange.requestURI.path
            environment["QUERY_STRING"] = exchange.requestURI.rawQuery ?: ""
            environment["REMOTE_ADDR"] = "127.0.0.1"
            environment["GIT_PROJECT_ROOT"] = projectRoot.path
            environment["GIT_HTTP_EXPORT_ALL"] = "1"
            environment["CONTENT_LENGTH"] = bodyBytes.size.toString()
            headers["content-type"]?.let { environment["CONTENT_TYPE"] = it }
            headers["git-protocol"]?.let { environment["HTTP_GIT_PROTOCOL"] = it }
            environment.putAll(SystemGit.environment)
            val result = runProcess(
                listOf(SystemGit.executable!!, "http-backend"),
                stdin = bodyBytes,
                environment = environment,
                removeEnvironment = SystemGit.removedEnvironment,
            )
            if (result.exitCode != 0) {
                exchange.sendResponseHeaders(500, -1)
                System.err.println("git http-backend failed: ${result.stderr}")
                return
            }
            val output = result.stdout
            var separator = -1
            var index = 0
            while (index + 3 < output.size) {
                if (output[index] == 13.toByte() && output[index + 1] == 10.toByte() &&
                    output[index + 2] == 13.toByte() && output[index + 3] == 10.toByte()
                ) {
                    separator = index
                    break
                }
                index += 1
            }
            if (separator == -1) {
                exchange.sendResponseHeaders(500, -1)
                System.err.println("CGI output lacks a header block")
                return
            }
            val headerText = String(output, 0, separator, Charsets.US_ASCII)
            val responseBody = output.copyOfRange(separator + 4, output.size)
            var status = 200
            for (line in headerText.split("\r\n")) {
                val colon = line.indexOf(':')
                if (colon == -1) continue
                val name = line.substring(0, colon).trim().lowercase()
                val value = line.substring(colon + 1).trim()
                if (name == "status") {
                    status = value.split(" ").first().toInt()
                } else if (name == "content-type") {
                    exchange.responseHeaders.set("Content-Type", value)
                }
            }
            exchange.sendResponseHeaders(status, if (responseBody.isEmpty()) -1 else responseBody.size.toLong())
            if (responseBody.isNotEmpty()) exchange.responseBody.use { it.write(responseBody) }
        } finally {
            exchange.close()
        }
    }

    fun close() = https.close()

    companion object {
        fun start(tls: SelfSignedTls, projectRoot: File): GitHttpBackendServer {
            val backend = GitHttpBackendServer(projectRoot, tls.certificateDer)
            backend.https = TestHttpsServer.start(tls) { server ->
                server.createContext("/") { exchange -> backend.handle(exchange) }
            }
            return backend
        }
    }
}

class GitHttpInteroperabilityTest {
    private val identity: GitIdentity
        get() = GitIdentity(name = "Ada", email = "ada@example.test", timestampSeconds = 1700000000, timezoneOffsetMinutes = 0)

    private class Fixture(
        val scratch: File,
        val work: File,
        val serverRepo: File,
        val backend: GitHttpBackendServer,
        val access: GitRemoteAccess,
        val signedRequests: MutableList<String>,
    ) {
        fun url(): GitRemoteUrl = GitRemoteUrl.parse(backend.repositoryUrl("repo.git"))
    }

    private fun skipped(test: String): Boolean {
        if (SystemGit.skipped("GitHttpInteroperabilityTest.$test")) return true
        if (SelfSignedTls.shared == null) {
            println("SKIP GitHttpInteroperabilityTest.$test: the JDK has no keytool for a test certificate")
            return true
        }
        return false
    }

    private inline fun withFixture(block: (Fixture) -> Unit) = withTempDir("forkmesh-g5g6-") { scratch ->
        val work = scratch.child("work").also { it.mkdir() }
        val serverRoot = scratch.child("srv").also { it.mkdir() }
        val serverRepo = serverRoot.child("repo.git")
        SystemGit.git(work, "init", "--initial-branch=main")
        SystemGit.git(work, "config", "user.name", "Oracle")
        SystemGit.git(work, "config", "user.email", "oracle@example.test")
        work.child("readme.txt").writeText("seed\n")
        SystemGit.git(work, "add", ".")
        SystemGit.git(work, "commit", "-m", "seed")
        SystemGit.git(serverRoot, "init", "--bare", "--initial-branch=main", serverRepo.path)
        SystemGit.git(serverRepo, "config", "http.receivepack", "true")
        SystemGit.git(work, "push", serverRepo.path, "main:main")
        val backend = GitHttpBackendServer.start(SelfSignedTls.shared!!, serverRoot)
        try {
            val signedRequests = Collections.synchronizedList(ArrayList<String>())
            val access = GitRemoteAccess(
                certificateChallenge = { challenge ->
                    if (challenge.certificateDer.contentEquals(backend.certificateDer)) {
                        GitCertificateDecision.accept
                    } else {
                        GitCertificateDecision.reject
                    }
                },
                headerProvider = { method, url, body ->
                    val digest = GitObjectId.sha1Of(body).hex
                    signedRequests.add("$method ${url.path} $digest")
                    mapOf("x-test-signature" to "$method:$digest")
                },
            )
            block(Fixture(scratch, work, serverRepo, backend, access, signedRequests))
        } finally {
            backend.close()
        }
    }

    fun testClonesFetchesAndPushesAgainstRealGitOverSmartHttps() {
        if (skipped("testClonesFetchesAndPushesAgainstRealGitOverSmartHttps")) return
        withFixture { fixture ->
            val cloneTarget = fixture.scratch.child("clone")
            val clone = GitRepository.clone(
                GitCloneRequest(
                    url = fixture.url(),
                    worktree = cloneTarget,
                    access = fixture.access,
                    detector = ClearDetector(),
                ),
            ).value
            val repository = clone.repository

            assertEq("main", clone.checkedOutBranch)
            assertEq("seed\n", cloneTarget.child("readme.txt").readText())
            val serverTip = SystemGit.git(fixture.serverRepo, "rev-parse", "main")
            assertEq(serverTip, repository.readHeadObjectId()!!.hex)
            SystemGit.git(cloneTarget, "fsck", "--strict")
            assertEq(serverTip, SystemGit.git(cloneTarget, "rev-parse", "refs/remotes/origin/main"))
            expect(fixture.signedRequests.isNotEmpty(), "the header provider saw requests")
            expect(
                fixture.backend.requestHeaders.all { it.containsKey("x-test-signature") },
                "every request carried the signature header",
            )

            cloneTarget.child("from-engine.txt").writeText("engine\n")
            repository.add(listOf(GitWorktreePath("from-engine.txt")))
            val pushedCommit = repository.commit(
                GitCommitRequest(message = "engine commit\n", author = identity, committer = identity),
            )

            val push = repository.push(GitPushRequest(access = fixture.access)).value

            assertEq(GitPushRefOutcome.updated, push.refs.single().outcome)
            assertEq(serverTip, push.refs.single().oldId?.hex)
            assertEq(pushedCommit, push.refs.single().newId)
            assertEq(pushedCommit.hex, SystemGit.git(fixture.serverRepo, "rev-parse", "main"))
            assertEq("engine commit", SystemGit.git(fixture.serverRepo, "log", "-1", "--format=%s"))
            SystemGit.git(fixture.serverRepo, "fsck", "--strict")
            assertEq(pushedCommit.hex, SystemGit.git(cloneTarget, "rev-parse", "refs/remotes/origin/main"))

            SystemGit.git(fixture.work, "fetch", fixture.serverRepo.path, "main")
            SystemGit.git(fixture.work, "merge", "--ff-only", "FETCH_HEAD")
            fixture.work.child("second.txt").writeText("second\n")
            SystemGit.git(fixture.work, "add", ".")
            SystemGit.git(fixture.work, "commit", "-m", "second")
            SystemGit.git(fixture.work, "push", fixture.serverRepo.path, "main:main")
            val newTip = SystemGit.git(fixture.serverRepo, "rev-parse", "main")

            val fetch = repository.fetch(GitFetchRequest(access = fixture.access)).value

            assertEq("refs/remotes/origin/main", fetch.updates.single().ref)
            assertEq(newTip, fetch.updates.single().newId.hex)
            expect(fetch.negotiationRounds >= 1, "negotiated at least one round")
            assertEq(newTip, SystemGit.git(cloneTarget, "rev-parse", "refs/remotes/origin/main"))
            SystemGit.git(cloneTarget, "fsck", "--strict")
            val fetchedCommit = repository.readCommit(GitObjectId.parseSha1(newTip))
            assertEq(pushedCommit, fetchedCommit.parents.single())
        }
    }

    fun testAStalePushIsRejectedClientSideBeforeContactingReceivePack() {
        if (skipped("testAStalePushIsRejectedClientSideBeforeContactingReceivePack")) return
        withFixture { fixture ->
            val cloneTarget = fixture.scratch.child("stale-clone")
            val clone = GitRepository.clone(
                GitCloneRequest(
                    url = fixture.url(),
                    worktree = cloneTarget,
                    access = fixture.access,
                    detector = ClearDetector(),
                ),
            ).value
            val repository = clone.repository

            fixture.work.child("race.txt").writeText("race\n")
            SystemGit.git(fixture.work, "add", ".")
            SystemGit.git(fixture.work, "commit", "-m", "race")
            SystemGit.git(fixture.work, "push", fixture.serverRepo.path, "main:main")
            val serverTip = SystemGit.git(fixture.serverRepo, "rev-parse", "main")

            cloneTarget.child("stale.txt").writeText("stale\n")
            repository.add(listOf(GitWorktreePath("stale.txt")))
            repository.commit(GitCommitRequest(message = "stale commit\n", author = identity, committer = identity))

            val requestCountBefore = fixture.backend.requestHeaders.size
            val result = repository.push(GitPushRequest(access = fixture.access)).result

            expect(result is GitFailure, "push failed")
            assertEq(GitErrorCode.nonFastForward, (result as GitFailure).code)
            assertEq(requestCountBefore + 1, fixture.backend.requestHeaders.size)
            assertEq(serverTip, SystemGit.git(fixture.serverRepo, "rev-parse", "main"))
        }
    }

    fun testAnEmptyRemoteClonesIntoAnUnbornMainBranch() {
        if (skipped("testAnEmptyRemoteClonesIntoAnUnbornMainBranch")) return
        withFixture { fixture ->
            val emptyRepo = fixture.backend.projectRoot.child("empty.git")
            SystemGit.git(fixture.backend.projectRoot, "init", "--bare", "--initial-branch=main", emptyRepo.path)
            SystemGit.git(emptyRepo, "config", "http.receivepack", "true")
            val cloneTarget = fixture.scratch.child("empty-clone")

            val clone = GitRepository.clone(
                GitCloneRequest(
                    url = GitRemoteUrl.parse(fixture.backend.repositoryUrl("empty.git")),
                    worktree = cloneTarget,
                    access = fixture.access,
                    detector = ClearDetector(),
                ),
            ).value

            assertEq(null, clone.checkedOutBranch)
            expect(clone.fetch.updates.isEmpty())
            assertEq(GitSymbolicRef("refs/heads/main"), clone.repository.readHead())
        }
    }
}
