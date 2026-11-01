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

    fun testTheHomeCountAsksForOneReportOnly() {
        val wired = Wired(
            status = 200,
            body = mapOf("ok" to true, "status" to "new", "counts" to mapOf("new" to 7, "triaged" to 2, "closed" to 4), "reports" to emptyList<Any>()),
        )
        wired.api.sessionTokenProvider = { "session-token" }
        val inbox = wired.api.bugReportInbox(status = "new", limit = 1)
        assertEq(7, inbox.counts["new"])
        assertEq(mapOf("status" to "new", "limit" to "1"), wired.transport.requests.single().query())
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

    fun testACancelledInboxLoadIsNotReportedAsUnreachable() {
        for (error in listOf(java.io.InterruptedIOException("interrupted"), java.nio.channels.ClosedByInterruptException())) {
            val wired = Wired()
            wired.offline = error
            val inbox = wired.api.bugReportInbox(status = "new")
            expect(inbox.cancelled)
            expect(!inbox.ok)
            assertEq("", inbox.message)
        }
        for (error in listOf(IOException("no route to host"), SocketTimeoutException("slow"))) {
            val wired = Wired()
            wired.offline = error
            val offline = wired.api.bugReportInbox()
            expect(!offline.cancelled)
            expect(offline.message.contains("Could not reach ForkMesh"), offline.message)
        }
    }

    fun testARelayWithoutTheInboxSaysToUpdateTheRelay() {
        val notFound = mapOf("error" to "not_found")
        val inbox = Wired(status = 404, body = notFound).api.bugReportInbox()
        expect(inbox.message.contains("Update the relay"), inbox.message)
        val change = Wired(status = 404, body = notFound).api.setBugReportStatus("r1", "closed")
        expect(change.message.contains("Update the relay"), change.message)
        val file = Wired(status = 404, body = notFound).api.bugReportAttachment("r1", "a1")
        expect(file.message.contains("Update the relay"), file.message)
        val method = Wired(status = 405, body = mapOf("error" to "method_not_allowed")).api.bugReportInbox()
        expect(method.message.contains("Update the relay"), method.message)
        val missing = Wired(status = 404, body = mapOf("error" to "attachment_missing")).api.bugReportAttachment("r1", "a1")
        expect(missing.message.contains("no longer available"), missing.message)
    }

    fun testASignedInUserListsTheBugsTheyReported() {
        val wired = Wired(
            status = 200,
            body = mapOf(
                "ok" to true, "offset" to 50, "nextOffset" to 100,
                "counts" to mapOf("new" to 1, "triaged" to 2, "closed" to 3),
                "reports" to listOf(
                    mapOf(
                        "id" to "b".repeat(32), "status" to "triaged", "severity" to "high", "platform" to "android",
                        "appVersion" to "0.1.0+1", "createdAt" to 1_783_463_371_000, "message" to "Repos tab spins.",
                        "screen" to "code", "package" to "notes", "confirmedFixedAt" to 0,
                        "attachments" to listOf(mapOf("name" to "screen.png", "size" to 6)),
                    ),
                ),
            ),
        )
        wired.api.sessionTokenProvider = { "session-token" }
        val page = wired.api.myBugReports(offset = 50)
        expect(page.ok)
        assertEq(2, page.counts["triaged"])
        assertEq(100, page.nextOffset)
        val report = page.reports.single()
        assertEq("b".repeat(32), report.id)
        assertEq("triaged", report.status)
        assertEq("high", report.severity)
        assertEq("0.1.0+1", report.appVersion)
        assertEq("code", report.screen)
        assertEq("notes", report.packageId)
        assertEq(1_783_463_371_000L, report.createdAt)
        assertEq(0L, report.confirmedFixedAt)
        expect(!report.confirmedFixed)
        assertEq("screen.png", report.attachments.single().name)
        assertEq(6L, report.attachments.single().size)
        val request = wired.transport.requests.single()
        assertEq("GET", request.method)
        assertEq("/api/bug-reports/mine", request.path())
        assertEq(mapOf("offset" to "50"), request.query())
        assertEq("Bearer session-token", request.headers["Authorization"])
        val first = Wired(status = 200, body = mapOf("ok" to true, "reports" to emptyList<Any>(), "nextOffset" to null))
        val empty = first.api.myBugReports()
        expect(empty.ok)
        expect(empty.reports.isEmpty())
        assertEq(null, empty.nextOffset)
        assertEq(emptyMap<String, String>(), first.transport.requests.single().query())
    }

    fun testMyReportsRefusalsAreSentences() {
        val signedOut = Wired(status = 401, body = mapOf("error" to "sign_in_required")).api.myBugReports()
        expect(!signedOut.ok)
        expect(signedOut.message.contains("Sign in"), signedOut.message)
        val old = Wired(status = 404, body = mapOf("error" to "not_found")).api.myBugReports()
        expect(old.message.contains("Update the relay"), old.message)
        val offline = Wired().apply { offline = IOException("no route to host") }.api.myBugReports()
        expect(offline.message.contains("Could not reach ForkMesh"), offline.message)
        val cancelled = Wired().apply { this.offline = java.io.InterruptedIOException("interrupted") }.api.myBugReports()
        expect(cancelled.cancelled)
        assertEq("", cancelled.message)
    }

    fun testAReporterConfirmsTheirBugIsFixed() {
        val id = "c".repeat(32)
        val wired = Wired(
            status = 200,
            body = mapOf(
                "ok" to true, "previous" to "triaged",
                "counts" to mapOf("new" to 0, "triaged" to 0, "closed" to 1),
                "report" to mapOf("id" to id, "status" to "closed", "message" to "Repos tab spins.", "confirmedFixedAt" to 1_783_463_999_000),
            ),
        )
        wired.api.sessionTokenProvider = { "session-token" }
        val result = wired.api.confirmBugReportFixed(id)
        expect(result.ok)
        assertEq("triaged", result.previous)
        assertEq(1, result.counts["closed"])
        val report = result.report!!
        assertEq("closed", report.status)
        assertEq(1_783_463_999_000L, report.confirmedFixedAt)
        expect(report.confirmedFixed)
        val request = wired.transport.requests.single()
        assertEq("POST", request.method)
        assertEq("/api/bug-reports/mine", request.path())
        assertEq(mapOf("report" to id, "fixed" to true), request.bodyJson())
        assertEq("Bearer session-token", request.headers["Authorization"])
    }

    fun testConfirmingSomeoneElsesReportIsRefused() {
        val missing = Wired(status = 404, body = mapOf("error" to "report_missing")).api.confirmBugReportFixed("r1")
        expect(!missing.ok)
        assertEq(null, missing.report)
        assertEq("That report is no longer in your list.", missing.message)
        val signedOut = Wired(status = 401, body = mapOf("error" to "sign_in_required")).api.confirmBugReportFixed("r1")
        expect(signedOut.message.contains("Sign in"), signedOut.message)
        val old = Wired(status = 404, body = mapOf("error" to "not_found")).api.confirmBugReportFixed("r1")
        expect(old.message.contains("Update the relay"), old.message)
        val noReport = Wired(status = 200, body = mapOf("ok" to true)).api.confirmBugReportFixed("r1")
        expect(!noReport.ok)
        expect(noReport.message.contains("could not be confirmed"), noReport.message)
    }
}
