package com.forkmesh.nativeapp.api

import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.assertFails
import com.forkmesh.nativeapp.testing.expect
import java.security.MessageDigest

class InboxContractTest {
    private class Fixture {
        val identity = TestIdentity()
        val transport = FakeTransport { jsonResponse(mapOf("ok" to true)) }
        val inbox = InboxService(TestSettings(serverUrl = "wss://app.forkmesh.com/ws"), { identity }, transport) { 1_770_000_000_000L }

        fun verify(canonical: String, sig: Any?) {
            expect(TestIdentity.verify(canonical.toByteArray(), sig as String, identity.publicKeyB64url), "signature over:\n$canonical")
        }
    }

    private fun sha256Hex(content: String): String =
        MessageDigest.getInstance("SHA-256").digest(content.toByteArray()).joinToString("") { "%02x".format(it.toInt() and 0xff) }

    @Suppress("UNCHECKED_CAST")
    private fun Map<String, Any?>.map(key: String) = this[key] as Map<String, Any?>

    fun testIssueEventContentHashesMatchTheQtCrossLanguageVectors() = with(Fixture()) {
        assertEq(
            "7b66aa5f19006b73598497bef2f5d6ed94700c48d479279df3ff5eb147fb0513",
            sha256Hex(inbox.issueContent("open", mapOf("title" to "Hello", "body" to "World", "attachments" to listOf("attachments/x.png")))),
        )
        assertEq(
            "37f2b6516c608aafe45958eabd5bb4c20b9c6765d1eb39a5c27b4588e566ec97",
            sha256Hex(inbox.issueContent("title", mapOf("title" to "Renamed issue"))),
        )
    }

    fun testPullCommentContentHashesMatchTheQtCrossLanguageVectors() = with(Fixture()) {
        assertEq(
            "5fc87d339144090b0ad2e192e6a6fe58e98d3a5a062467c7e62049c7d8c3db01",
            sha256Hex(inbox.pullCommentContent("comment", mapOf("body" to "Looks good"))),
        )
        assertEq(
            "b863bbc11dd8fea92da94a7da47f815aceeaa9418483992d0a952273894a0731",
            sha256Hex(inbox.pullCommentContent("review", mapOf("state" to "approved", "body" to "LGTM"))),
        )
        assertEq(
            "a2574c2b392fd0db6b7e4b0d025d4094bb410691dca7686ddf095d63881f4985",
            sha256Hex(inbox.pullCommentContent("line-comment", mapOf("path" to "src/x.cpp", "side" to "new", "line" to 42, "body" to "needs a guard"))),
        )
        assertEq(
            "de9b901105f5216175574abb59e8dd1e28e192c85864c7cd07e28a629adebdfa",
            sha256Hex(
                inbox.pullCommentContent(
                    "line-comment",
                    mapOf(
                        "path" to "src/x.cpp", "side" to "new", "line" to 7, "body" to "needs a guard",
                        "patchset" to "0123456789abcdef0123456789abcdef01234567", "anchorLines" to "if (!p)",
                    ),
                ),
            ),
        )
        assertEq(
            "3e917b7d28e0e5ac2f551e30dc6d30f986325bc015c7b2dd6d1095cd11ebd894",
            sha256Hex(
                inbox.pullCommentContent(
                    "thread-comment",
                    mapOf(
                        "threadId" to "thread-1", "path" to "src/x.cpp", "side" to "new", "lineStart" to 42, "lineEnd" to 44,
                        "body" to "Use guard", "suggestionPatch" to "@@ -1 +1 @@\n-old\n+new\n",
                        "patchset" to "0123456789abcdef0123456789abcdef01234567", "anchorLines" to "int x = 1;\nreturn x;\n}",
                    ),
                ),
            ),
        )
    }

    fun testContentLayoutsPerType() = with(Fixture()) {
        assertEq("b\u0000", inbox.issueContent("comment", mapOf("body" to "b")))
        assertEq("a,b", inbox.issueContent("labels", mapOf("labels" to listOf("a", "b"))))
        assertEq("3", inbox.issueContent("priority", mapOf("priority" to 3.9)))
        assertEq("0", inbox.issueContent("progress", emptyMap()))
        assertEq("", inbox.issueContent("vote", emptyMap()))
        assertEq("t1\u0000p1\u0000b", inbox.pullCommentContent("thread-reply", mapOf("threadId" to "t1", "parentId" to "p1", "body" to "b")))
        assertEq("t1\u0000applied\u0000abc\u0000", inbox.pullCommentContent("suggestion-state", mapOf("threadId" to "t1", "state" to "applied", "appliedCommit" to "abc")))
        assertEq("T\u0000B\u0000ideas", inbox.discussionContent("open", mapOf("title" to "T", "body" to "B", "category" to "ideas")))
    }

    fun testNewIssueIsSignedWithTheIssueEventTemplate() = with(Fixture()) {
        inbox.submitNewIssue("owner", "repo", title = "Crash", body = "Steps", labels = listOf("bug"), milestone = "v2", priority = 2, assignees = listOf("mona"))
        val request = transport.requests.single()
        assertEq("https://app.forkmesh.com/api/repo/owner/repo/issues", request.url)
        assertEq("application/json", request.headerValue("Content-Type"))
        val body = request.bodyJson()
        assertEq(0L, body["number"])
        assertEq("Crash", body["titleIfNew"])
        assertEq(mapOf("labels" to listOf("bug"), "milestone" to "v2", "priority" to 2L, "assignees" to listOf("mona")), body["meta"])
        val event = body.map("event")
        assertEq(listOf("type", "title", "body", "attachments", "author", "ts", "sig"), event.keys.toList())
        assertEq(identity.publicKeyB64url, event["author"])
        assertEq(1_770_000_000_000L, event["ts"])
        val hash = sha256Hex("Crash\u0000Steps\u0000")
        verify("forkmesh-issue-event-v2\nowner/repo\nopen\n0\n${identity.publicKeyB64url}\n1770000000000\n$hash", event["sig"])
    }

    fun testIssueCommentVoteAndStatusAreSigned() = with(Fixture()) {
        inbox.commentOnIssue("o", "r", 12, "me too")
        inbox.voteOnIssue("o", "r", 12)
        inbox.setIssueStatus("o", "r", 12, "closed")
        val author = identity.publicKeyB64url
        val (comment, vote, status) = transport.requests.map { it.bodyJson() }
        assertEq(12L, comment["number"])
        verify("forkmesh-issue-event-v2\no/r\ncomment\n12\n$author\n1770000000000\n${sha256Hex("me too\u0000")}", comment.map("event")["sig"])
        verify("forkmesh-issue-event-v2\no/r\nvote\n12\n$author\n1770000000000\n${sha256Hex("")}", vote.map("event")["sig"])
        assertEq("closed", status.map("event")["status"])
        verify("forkmesh-issue-event-v2\no/r\nstatus\n12\n$author\n1770000000000\n${sha256Hex("closed")}", status.map("event")["sig"])
    }

    fun testNewPullIsSignedOverTitleBaseHeadPatch() = with(Fixture()) {
        inbox.submitNewPull("o", "r", title = "Fix", base = "main", head = "fix/x", patch = "diff --git a b")
        val request = transport.requests.single()
        assertEq("/api/repo/o/r/pulls", request.path())
        val pull = request.bodyJson().map("pull")
        assertEq(listOf("title", "base", "head", "patch", "author", "ts", "sig"), pull.keys.toList())
        val hash = sha256Hex("Fix\u0000main\u0000fix/x\u0000diff --git a b")
        verify("forkmesh-pull-event-v2\no/r\n${identity.publicKeyB64url}\n1770000000000\n$hash", pull["sig"])
    }

    fun testNewPullWithCommitsSignsThemAndReturnsTheNumber() {
        val identity = TestIdentity()
        val transport = FakeTransport { jsonResponse(mapOf("ok" to true, "number" to 31)) }
        val inbox = InboxService(TestSettings(serverUrl = "wss://app.forkmesh.com/ws"), { identity }, transport) { 1_770_000_000_000L }
        val number = inbox.submitNewPull("o", "r", "Fix", "main", "agent/x", "diff --git a b", description = "Why", authorName = "ann", commits = "From abc")
        assertEq(31, number)
        val pull = transport.requests.single().bodyJson().map("pull")
        assertEq(listOf("title", "base", "head", "patch", "commits", "description", "author", "authorName", "ts", "sig"), pull.keys.toList())
        val hash = sha256Hex("Fix\u0000main\u0000agent/x\u0000diff --git a b\u0000From abc")
        expect(TestIdentity.verify("forkmesh-pull-event-v2\no/r\n${identity.publicKeyB64url}\n1770000000000\n$hash".toByteArray(), pull["sig"] as String, identity.publicKeyB64url))
        assertEq(0, Fixture().inbox.submitNewPull("o", "r", "t", "b", "h", "p"))
    }

    fun testPullCommentsAndReviewsUseThePullCommentTemplate() = with(Fixture()) {
        inbox.commentOnPull("o", "r", 7, "Looks good")
        inbox.reviewPull("o", "r", 7, "approve", "LGTM")
        val author = identity.publicKeyB64url
        val comment = transport.requests[0].bodyJson().map("event")
        verify("forkmesh-pull-comment-v2\no/r\ncomment\n7\n$author\n1770000000000\n${sha256Hex("Looks good")}", comment["sig"])
        val review = transport.requests[1].bodyJson().map("event")
        assertEq("approve", review["state"])
        verify("forkmesh-pull-comment-v2\no/r\nreview\n7\n$author\n1770000000000\n${sha256Hex("approve\u0000LGTM")}", review["sig"])
    }

    fun testDiscussionsUseTheDiscussionEventTemplate() = with(Fixture()) {
        inbox.submitNewDiscussion("o", "r", title = "Idea", body = "Body", category = "ideas")
        inbox.commentOnDiscussion("o", "r", 3, "Reply")
        val author = identity.publicKeyB64url
        val open = transport.requests[0].bodyJson()
        assertEq("/api/repo/o/r/discussions", transport.requests[0].path())
        assertEq(0L, open["number"])
        assertEq("Idea", open["titleIfNew"])
        verify("forkmesh-discussion-event-v2\no/r\nopen\n0\n$author\n1770000000000\n${sha256Hex("Idea\u0000Body\u0000ideas")}", open.map("event")["sig"])
        val reply = transport.requests[1].bodyJson()
        verify("forkmesh-discussion-event-v2\no/r\ncomment\n3\n$author\n1770000000000\n${sha256Hex("Reply")}", reply.map("event")["sig"])
    }

    fun testThreadSubscriptionSignsTheSubscribeTemplate() = with(Fixture()) {
        inbox.setThreadSubscription("owner", "repo", node = " Mona ", source = "pull", number = 7, subscribed = true)
        inbox.setThreadSubscription("owner", "repo", node = "mona", source = "issue", number = 12, subscribed = false)
        val on = transport.requests[0].bodyJson()
        assertEq("/api/repo/owner/repo/subscribe", transport.requests[0].path())
        assertEq(listOf("node", "source", "number", "subscribed", "ts", "sig"), on.keys.toList())
        assertEq("mona", on["node"])
        assertEq(true, on["subscribed"])
        assertEq("1770000000000", on["ts"])
        verify("forkmesh-subscribe-v1\nowner/repo\npull\n7\n1\n1770000000000", on["sig"])
        val off = transport.requests[1].bodyJson()
        verify("forkmesh-subscribe-v1\nowner/repo\nissue\n12\n0\n1770000000000", off["sig"])

        assertFails<IllegalArgumentException> { inbox.setThreadSubscription("o", "r", node = " ", source = "issue", number = 1, subscribed = true) }
        assertFails<IllegalArgumentException> { inbox.setThreadSubscription("o", "r", node = "a", source = "commit", number = 1, subscribed = true) }
        assertFails<IllegalArgumentException> { inbox.setThreadSubscription("o", "r", node = "a", source = "issue", number = 0, subscribed = true) }
        assertEq(2, transport.requests.size)
    }

    fun testRelayRefusalsSurfaceTheErrorCode() = with(Fixture()) {
        transport.handler = { jsonResponse(mapOf("error" to "bad_signature"), 401) }
        assertEq("Exception: bad_signature", assertFails<DartException> { inbox.commentOnIssue("o", "r", 1, "x") }.toString())
        transport.handler = { textResponse("nope", 500) }
        assertEq("HTTP 500", assertFails<DartException> { inbox.voteOnIssue("o", "r", 1) }.message)
        transport.handler = { jsonResponse(mapOf("ok" to true), 202) }
        assertEq("HTTP 202", assertFails<DartException> { inbox.voteOnIssue("o", "r", 1) }.message)
        transport.handler = { jsonResponse(mapOf("ok" to true), 201) }
        inbox.voteOnIssue("o", "r", 1)
    }

    fun testWithoutASigningIdentityNothingIsSent() = with(Fixture()) {
        val quiet = FakeTransport { error("must not send") }
        val unsigned = InboxService(TestSettings(), { null }, quiet)
        assertFails<DartException> { unsigned.commentOnIssue("o", "r", 1, "x") }
        assertFails<DartException> { unsigned.submitNewPull("o", "r", "t", "b", "h", "p") }
        assertFails<DartException> { unsigned.setThreadSubscription("o", "r", node = "a", source = "issue", number = 1, subscribed = true) }
        assertEq(0, quiet.requests.size)
    }
}
