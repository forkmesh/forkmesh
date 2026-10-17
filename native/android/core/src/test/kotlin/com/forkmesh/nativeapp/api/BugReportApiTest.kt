package com.forkmesh.nativeapp.api

import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import java.io.IOException
import java.net.SocketTimeoutException

class BugReportApiTest {
    private class Wired(status: Int = 201, body: Any? = mapOf("ok" to true, "id" to "a".repeat(32))) {
        var offline: IOException? = null
        val transport = FakeTransport {
            offline?.let { throw it }
            jsonResponse(body, status)
        }
        val api = ApiService(TestSettings(serverUrl = "ws://relay.test/ws"), transport)
    }

    fun testAReportPostsToBugReportsWithItsBuildContext() {
        val wired = Wired()
        val result = wired.api.submitBugReport(
            message = "  Repos tab spins forever.  ",
            contact = "reporter@example.com",
            screen = "code",
            severity = "high",
            platform = "android",
            appVersion = "0.1.0+1",
            osVersion = "Android 16",
        )
        expect(result.ok)
        assertEq("a".repeat(32), result.id)
        val request = wired.transport.requests.single()
        assertEq("/api/bug-reports", request.path())
        assertEq("http://relay.test/api/bug-reports", request.url)
        assertEq("POST", request.method)
        assertEq(
            mapOf(
                "message" to "Repos tab spins forever.",
                "contact" to "reporter@example.com",
                "screen" to "code",
                "severity" to "high",
                "source" to "mobile",
                "platform" to "android",
                "appVersion" to "0.1.0+1",
                "osVersion" to "Android 16",
            ),
            request.bodyJson(),
        )
    }

    fun testAnOmittedContactIsLeftOutRatherThanSentEmpty() {
        val wired = Wired()
        wired.api.submitBugReport(message = "Something is broken.", contact = "   ")
        val sent = wired.transport.requests.single().bodyJson()
        expect(!sent.containsKey("contact"))
        assertEq("normal", sent["severity"])
    }

    fun testFeedbackOnAPluginNamesItsMarketplacePackage() {
        val wired = Wired()
        expect(wired.api.submitBugReport(message = "The face changer loses my outfit.", screen = "Face Changer", packageId = " face-changer ").ok)
        val sent = wired.transport.requests.single().bodyJson()
        assertEq("face-changer", sent["package"])
        assertEq("Face Changer", sent["screen"])
    }

    fun testAReportWithoutAPluginLeavesThePackageOut() {
        val wired = Wired()
        wired.api.submitBugReport(message = "Something is broken.")
        wired.api.submitBugReport(message = "Something is broken.", packageId = "  ")
        wired.transport.requests.forEach { expect(!it.bodyJson().containsKey("package")) }
    }

    fun testAnUnauthenticatedReportIsSentWithNoAuthorizationHeader() {
        val wired = Wired()
        expect(wired.api.submitBugReport(message = "Login loops.").ok)
        assertEq(null, wired.transport.requests.single().headerValue("Authorization"))
    }

    fun testASignedInReportCarriesTheSessionBearer() {
        val wired = Wired()
        wired.api.sessionTokenProvider = { "session-token" }
        wired.api.submitBugReport(message = "Chat badge never clears.")
        assertEq("Bearer session-token", wired.transport.requests.single().headerValue("Authorization"))
    }

    fun testAnEmptyMessageNeverReachesTheNetwork() {
        val wired = Wired()
        val result = wired.api.submitBugReport(message = "   ")
        expect(!result.ok)
        expect(result.message.contains("Describe the problem"))
        assertEq(0, wired.transport.requests.size)
    }

    fun testRefusalsAreSentences() {
        val limited = Wired(status = 429, body = mapOf("error" to "rate_limited")).api.submitBugReport(message = "Still broken.")
        expect(!limited.ok)
        expect(limited.message.contains("several reports"))
        expect(!limited.message.contains("429"))
        expect(Wired(status = 413).api.submitBugReport(message = "x".repeat(40)).message.contains("too long"))
        expect(Wired(status = 400).api.submitBugReport(message = "x").message.contains("more detail"))
        expect(Wired(status = 500).api.submitBugReport(message = "x").message.contains("could not accept"))
        val noId = Wired(status = 200, body = "ok").api.submitBugReport(message = "x")
        expect(noId.ok)
        assertEq("", noId.id)
    }

    fun testAnUnreachableRelayFailsWithoutThrowing() {
        val wired = Wired()
        wired.offline = IOException("no route to host")
        val result = wired.api.submitBugReport(message = "Nothing loads.")
        expect(!result.ok)
        expect(result.message.contains("connection"))
        wired.offline = SocketTimeoutException("Read timed out")
        expect(wired.api.submitBugReport(message = "Nothing loads.").message.contains("timed out"))
    }

    fun testABugReportNeverBouncesAValidSessionToTheLoginScreen() {
        for (status in listOf(429, 401)) {
            val wired = Wired(status = status, body = mapOf("error" to "rate_limited"))
            wired.api.sessionTokenProvider = { "session-token" }
            var signedOut = false
            wired.api.onUnauthorized = { signedOut = true }
            wired.api.onSessionInvalid = { signedOut = true }
            wired.api.submitBugReport(message = "Still broken.")
            expect(!signedOut, "status $status")
        }
    }

    fun testTheInboxListsReportsForAnAdministrator() {
        val wired = Wired(
            status = 200,
            body = mapOf(
                "ok" to true, "status" to "new", "nextOffset" to 50,
                "counts" to mapOf("new" to 2, "triaged" to 1, "closed" to 0),
                "reports" to listOf(
                    mapOf(
                        "id" to "r1", "status" to "new", "severity" to "crash", "platform" to "android",
                        "createdAt" to 1_783_463_371_000, "account" to "alice", "message" to "Repos tab spins.",
                        "contact" to "reply@example.test", "package" to "notes",
                        "attachments" to listOf(mapOf("id" to "a1", "name" to "screen.png", "contentType" to "image/png", "size" to 6)),
                    ),
                ),
            ),
        )
        wired.api.sessionTokenProvider = { "session-token" }
        val inbox = wired.api.bugReportInbox(status = "new", offset = 50)
        expect(inbox.ok)
        assertEq(2, inbox.counts["new"])
        assertEq(50, inbox.nextOffset)
        val report = inbox.reports.single()
        assertEq("Repos tab spins.", report.message)
        assertEq("reply@example.test", report.contact)
        assertEq("notes", report.packageId)
        assertEq(1_783_463_371_000L, report.createdAt)
        assertEq("screen.png", report.attachments.single().name)
        val request = wired.transport.requests.single()
        assertEq("GET", request.method)
        assertEq("/api/bug-reports/inbox", request.path())
        assertEq(mapOf("status" to "new", "offset" to "50"), request.query())
        assertEq("Bearer session-token", request.headers["Authorization"])
    }

    fun testTheInboxRefusalSaysItIsForAdministrators() {
        val wired = Wired(status = 403, body = mapOf("error" to "admin_required"))
        val inbox = wired.api.bugReportInbox()
        expect(!inbox.ok)
        expect(inbox.reports.isEmpty())
        expect(inbox.message.contains("administrators"), inbox.message)
    }

    fun testAnAdministratorSetsAReportStatus() {
        val wired = Wired(status = 200, body = mapOf("ok" to true, "status" to "closed", "counts" to mapOf("new" to 0, "closed" to 1)))
        wired.api.sessionTokenProvider = { "session-token" }
        val change = wired.api.setBugReportStatus("r1", "closed")
        expect(change.ok)
        assertEq("closed", change.status)
        assertEq(1, change.counts["closed"])
        val request = wired.transport.requests.single()
        assertEq("POST", request.method)
        assertEq("/api/bug-reports/inbox", request.path())
        assertEq(mapOf("report" to "r1", "status" to "closed"), request.bodyJson())
        val refused = Wired(status = 404, body = mapOf("error" to "report_missing")).api.setBugReportStatus("r1", "closed")
        expect(!refused.ok)
        assertEq("That report no longer exists.", refused.message)
    }

    fun testAnAdministratorDownloadsAnAttachment() {
        val transport = FakeTransport {
            com.forkmesh.nativeapp.net.HttpClient.Response(200, mapOf("Content-Type" to listOf("image/png")), "pixels".toByteArray())
        }
        val api = ApiService(TestSettings(serverUrl = "ws://relay.test/ws"), transport)
        api.sessionTokenProvider = { "session-token" }
        val file = api.bugReportAttachment("r1", "a1")
        expect(file.ok)
        assertEq("pixels", String(file.data))
        assertEq("image/png", file.contentType)
        val request = transport.requests.single()
        assertEq("/api/bug-reports/inbox/attachment", request.path())
        assertEq(mapOf("report" to "r1", "attachment" to "a1"), request.query())
        val gone = Wired(status = 410, body = mapOf("error" to "attachment_unavailable")).api.bugReportAttachment("r1", "a1")
        expect(!gone.ok)
        expect(gone.message.contains("no longer available"), gone.message)
    }
}
