package com.forkmesh.nativeapp.git

import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.assertFails
import com.forkmesh.nativeapp.testing.expect
import com.sun.net.httpserver.HttpExchange
import java.net.URI
import kotlin.time.Duration.Companion.seconds

class GitIoHttpTransportTest {
    private val advertisementType = "application/x-git-upload-pack-advertisement"

    private fun tls(test: String): SelfSignedTls? {
        val tls = SelfSignedTls.shared
        if (tls == null) println("SKIP GitIoHttpTransportTest.$test: the JDK has no keytool for a test certificate")
        return tls
    }

    private fun request(url: URI, maxResponseBytes: Int = 1024 * 1024, deadline: Long? = null) = GitHttpRequest(
        method = "GET",
        url = url,
        headers = mapOf("accept" to "*/*"),
        maxResponseBytes = maxResponseBytes,
        expectedContentType = advertisementType,
        deadline = deadline,
    )

    private fun trusting(tls: SelfSignedTls) = GitIoHttpTransport(
        certificateChallenge = { challenge ->
            if (challenge.certificateDer.contentEquals(tls.certificateDer)) {
                GitCertificateDecision.accept
            } else {
                GitCertificateDecision.reject
            }
        },
    )

    private fun respond(exchange: HttpExchange, status: Int, contentType: String?, body: ByteArray) {
        if (contentType != null) exchange.responseHeaders.set("Content-Type", contentType)
        exchange.sendResponseHeaders(status, if (body.isEmpty()) -1 else body.size.toLong())
        if (body.isNotEmpty()) exchange.responseBody.write(body)
        exchange.close()
    }

    private fun stallAfterFirstChunk(exchange: HttpExchange) {
        exchange.responseHeaders.set("Content-Type", advertisementType)
        exchange.sendResponseHeaders(200, 0)
        exchange.responseBody.write(ByteArray(10))
        exchange.responseBody.flush()
        try {
            Thread.sleep(30_000)
        } catch (error: InterruptedException) {
        }
        exchange.close()
    }

    private fun elapsedMs(started: Long): Long = (System.nanoTime() - started) / 1_000_000

    fun testTrustedRootsAndAcceptedChallengesReachTheServer() {
        val tls = tls("testTrustedRootsAndAcceptedChallengesReachTheServer") ?: return
        TestHttpsServer.start(tls) { server ->
            server.createContext("/ok") { respond(it, 200, "$advertisementType; charset=utf-8", utf8("hello")) }
        }.use { server ->
            val pinned = GitIoHttpTransport(trustedRootCertificatesPem = tls.certificatePem)
            val response = pinned.send(request(server.url("/ok")))
            assertEq(200, response.statusCode)
            assertEq("hello", utf8Text(response.body))
            expect(response.contentType!!.startsWith(advertisementType))

            assertEq("hello", utf8Text(trusting(tls).send(request(server.url("/ok"))).body))
        }
    }

    fun testUntrustedCertificatesAreRejectedUnlessTheCallerAccepts() {
        val tls = tls("testUntrustedCertificatesAreRejectedUnlessTheCallerAccepts") ?: return
        TestHttpsServer.start(tls) { server ->
            server.createContext("/ok") { respond(it, 200, advertisementType, utf8("hello")) }
        }.use { server ->
            assertGitError(GitErrorCode.tlsRejected) { GitIoHttpTransport().send(request(server.url("/ok"))) }

            val challenges = ArrayList<GitCertificateChallenge>()
            val refusing = GitIoHttpTransport(
                certificateChallenge = { challenge ->
                    challenges.add(challenge)
                    GitCertificateDecision.reject
                },
            )
            assertGitError(GitErrorCode.tlsRejected) { refusing.send(request(server.url("/ok"))) }
            expect(challenges.isNotEmpty(), "the handler saw the challenge")
            assertEq("127.0.0.1", challenges.first().host)
            assertEq(server.port, challenges.first().port)
            expect(challenges.first().certificateDer.contentEquals(tls.certificateDer))
            expect(Regex("[0-9a-f]{40}").matches(challenges.first().sha1Fingerprint))
            expect(!challenges.first().defaultValidationPassed)

            val throwing = GitIoHttpTransport(certificateChallenge = { throw IllegalStateException("boom") })
            assertGitError(GitErrorCode.tlsRejected) { throwing.send(request(server.url("/ok"))) }
        }
    }

    fun testRedirectsAreNeverFollowed() {
        val tls = tls("testRedirectsAreNeverFollowed") ?: return
        TestHttpsServer.start(tls) { server ->
            server.createContext("/elsewhere") { exchange ->
                exchange.responseHeaders.set("Location", "https://elsewhere.test/repo")
                respond(exchange, 302, null, ByteArray(0))
            }
            server.createContext("/same") { exchange ->
                exchange.responseHeaders.set("Location", "https://127.0.0.1/other")
                respond(exchange, 301, null, ByteArray(0))
            }
            server.createContext("/target") { respond(it, 200, advertisementType, utf8("followed")) }
        }.use { server ->
            val transport = trusting(tls)
            val different = assertGitError(GitErrorCode.remoteRejected) { transport.send(request(server.url("/elsewhere"))) }
            expect(different.message.contains("different host"), different.message)
            val same = assertGitError(GitErrorCode.remoteRejected) { transport.send(request(server.url("/same"))) }
            expect(same.message.contains("same host"), same.message)
        }
    }

    fun testSurfacedRedirectsReturnTheStatusAndLocationWithoutFollowing() {
        val tls = tls("testSurfacedRedirectsReturnTheStatusAndLocationWithoutFollowing") ?: return
        var followed = false
        TestHttpsServer.start(tls) { server ->
            server.createContext("/moved") { exchange ->
                exchange.responseHeaders.set("Location", "https://mirror.test/repo.git/info/refs?service=git-upload-pack")
                respond(exchange, 307, null, ByteArray(0))
            }
            server.createContext("/target") {
                followed = true
                respond(it, 200, advertisementType, utf8("followed"))
            }
        }.use { server ->
            val transport = GitIoHttpTransport(
                certificateChallenge = { GitCertificateDecision.accept },
                surfaceRedirects = true,
            )
            val response = transport.send(request(server.url("/moved")))
            assertEq(307, response.statusCode)
            assertEq("https://mirror.test/repo.git/info/refs?service=git-upload-pack", response.headers["location"])
            assertEq(0, response.body.size)
            expect(!followed)
        }
    }

    fun testStatusesAndContentTypesMapToTypedErrors() {
        val tls = tls("testStatusesAndContentTypesMapToTypedErrors") ?: return
        TestHttpsServer.start(tls) { server ->
            server.createContext("/auth") { respond(it, 401, "text/plain", utf8("no")) }
            server.createContext("/broken") { respond(it, 500, "text/plain", utf8("no")) }
            server.createContext("/html") { respond(it, 200, "text/html", utf8("<html>")) }
        }.use { server ->
            val transport = trusting(tls)
            assertGitError(GitErrorCode.authenticationFailed) { transport.send(request(server.url("/auth"))) }
            val broken = assertGitError(GitErrorCode.remoteRejected) { transport.send(request(server.url("/broken"))) }
            assertEq("remote returned HTTP 500", broken.message)
            assertGitError(GitErrorCode.protocolUnsupported) { transport.send(request(server.url("/html"))) }
        }
    }

    fun testResponseHeadersAndBodiesAreBounded() {
        val tls = tls("testResponseHeadersAndBodiesAreBounded") ?: return
        TestHttpsServer.start(tls) { server ->
            server.createContext("/fixed") { respond(it, 200, advertisementType, ByteArray(2048)) }
            server.createContext("/chunked") { exchange ->
                exchange.responseHeaders.set("Content-Type", advertisementType)
                exchange.sendResponseHeaders(200, 0)
                exchange.responseBody.write(ByteArray(2048))
                exchange.close()
            }
        }.use { server ->
            val transport = trusting(tls)
            assertGitError(GitErrorCode.resourceLimitExceeded) {
                transport.send(request(server.url("/fixed"), maxResponseBytes = 1024))
            }
            assertGitError(GitErrorCode.resourceLimitExceeded) {
                transport.send(request(server.url("/chunked"), maxResponseBytes = 1024))
            }
            assertGitError(GitErrorCode.resourceLimitExceeded) {
                transport.send(request(server.url("/fixed")), limits = GitResourceLimits(maxResponseHeaderBytes = 16))
            }
        }
    }

    fun testCancellationAbortsARequestStalledMidBody() {
        val tls = tls("testCancellationAbortsARequestStalledMidBody") ?: return
        TestHttpsServer.start(tls) { server ->
            server.createContext("/stall") { stallAfterFirstChunk(it) }
        }.use { server ->
            val cancellation = GitCancellationToken()
            Thread {
                Thread.sleep(300)
                cancellation.cancel()
            }.also { it.isDaemon = true }.start()
            val started = System.nanoTime()

            assertGitError(GitErrorCode.cancelled) {
                trusting(tls).send(request(server.url("/stall")), cancellation = cancellation)
            }
            expect(elapsedMs(started) < 5_000, "cancellation took ${elapsedMs(started)} ms")
        }
    }

    fun testAStalledBodyFailsAfterTheIdleReadTimeoutOrTheDeadline() {
        val tls = tls("testAStalledBodyFailsAfterTheIdleReadTimeoutOrTheDeadline") ?: return
        TestHttpsServer.start(tls) { server ->
            server.createContext("/stall") { stallAfterFirstChunk(it) }
        }.use { server ->
            var started = System.nanoTime()
            val idle = assertGitError(GitErrorCode.networkUnavailable) {
                trusting(tls).send(request(server.url("/stall")), limits = GitResourceLimits(idleReadTimeout = 1.seconds))
            }
            expect(idle.message.contains("idle read timeout"), idle.message)
            expect(elapsedMs(started) < 8_000, "idle timeout took ${elapsedMs(started)} ms")

            started = System.nanoTime()
            val deadline = assertGitError(GitErrorCode.networkUnavailable) {
                trusting(tls).send(request(server.url("/stall"), deadline = System.currentTimeMillis() + 800))
            }
            expect(deadline.message.contains("deadline"), deadline.message)
            expect(elapsedMs(started) < 8_000, "deadline took ${elapsedMs(started)} ms")
        }
    }

    fun testHeaderProvidersRequestsAndClosedTransportsFailClosedBeforeConnecting() {
        val unreachable = URI("https://127.0.0.1:1/repo.git/info/refs")
        assertGitError(GitErrorCode.authenticationFailed) {
            GitIoHttpTransport(headerProvider = { _, _, _ -> mapOf("Content-Type" to "text/plain") })
                .send(request(unreachable))
        }
        val failed = assertGitError(GitErrorCode.authenticationFailed) {
            GitIoHttpTransport(headerProvider = { _, _, _ -> throw IllegalStateException("signer offline") })
                .send(request(unreachable))
        }
        assertEq("header provider failed", failed.message)
        assertGitError(GitErrorCode.authenticationFailed) { GitHttpHeaders.bearer("") }
        assertGitError(GitErrorCode.authenticationFailed) { GitHttpHeaders.basic("ada:x", "secret") }
        assertEq(
            "Basic YWRhOnNlY3JldA==",
            GitHttpHeaders.basic("ada", "secret").headers("GET", unreachable, ByteArray(0))["authorization"],
        )

        val closed = GitIoHttpTransport()
        closed.close()
        assertGitError(GitErrorCode.networkUnavailable) { closed.send(request(unreachable)) }

        assertGitError(GitErrorCode.protocolUnsupported) { request(URI("http://127.0.0.1/repo.git")) }
        assertGitError(GitErrorCode.protocolUnsupported) { request(URI("https://ada@127.0.0.1/repo.git")) }
        assertFails<IllegalArgumentException> {
            GitHttpRequest(
                method = "PUT",
                url = unreachable,
                maxResponseBytes = 1,
                expectedContentType = advertisementType,
            )
        }
    }
}
