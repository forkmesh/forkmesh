package com.forkmesh.nativeapp.collab

import com.forkmesh.nativeapp.api.ApiJsonResult
import com.forkmesh.nativeapp.api.EventSigning
import com.forkmesh.nativeapp.api.MemoryPrefStore
import com.forkmesh.nativeapp.ci.RepoRef
import com.forkmesh.nativeapp.code.diff.parseUnifiedDiff
import com.forkmesh.nativeapp.control.LocalControlException
import com.forkmesh.nativeapp.model.Issue
import com.forkmesh.nativeapp.model.PublishedPull
import com.forkmesh.nativeapp.model.RepoBlob
import com.forkmesh.nativeapp.model.RepoBranch
import com.forkmesh.nativeapp.model.RepoComparison
import com.forkmesh.nativeapp.model.RepoTree
import com.forkmesh.nativeapp.model.RepoTreeEntry
import com.forkmesh.nativeapp.security.DeployApproval
import com.forkmesh.nativeapp.security.DeployApprover
import com.forkmesh.nativeapp.security.DeviceIdentity
import com.forkmesh.nativeapp.security.Identity
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.assertFails
import com.forkmesh.nativeapp.testing.expect

class EchoIdentity : DeviceIdentity {
    val signed = ArrayList<String>()
    override val publicKeyBytes = ByteArray(32)
    override val publicKeyB64url = "AUTHOR"
    override val nodeId = "AUTHOR"
    override val shortKey = "AUTH"
    override fun sign(data: ByteArray): String {
        val text = String(data, Charsets.UTF_8)
        signed.add(text)
        return "sig:${text.hashCode()}"
    }
    override fun exportSeed() = ByteArray(32)
    override fun rotate() {}
}

class RecordingCall {
    val calls = ArrayList<Pair<String, Map<String, Any?>?>>()
    var answer = ApiJsonResult(200, mapOf("ok" to true))
    val call: CollabJsonCall = { method, path, _, body ->
        calls.add("$method $path" to body)
        answer
    }
}

class ReviewToolsTest {
    private val patch = """
        diff --git a/src/app.kt b/src/app.kt
        --- a/src/app.kt
        +++ b/src/app.kt
        @@ -1,4 +1,5 @@
         line one
        -old two
        +new two
        +added three
         line four
         line five
    """.trimIndent()

    fun testSignedPullEventMatchesTheRelayCanonical() {
        val identity = EchoIdentity()
        val signer = CollabSigner(identity, now = { 1234L }, authorName = "alice", random = { "r" })
        val ev = signer.sign(CollabRecordKind.Pull, "Owner", "Repo", 7, "review", linkedMapOf("state" to "approve", "body" to "lgtm"))
        val hash = CollabSigner.sha256Hex("approve\u0000lgtm")
        assertEq("forkmesh-pull-comment-v2\nowner/repo\nreview\n7\nAUTHOR\n1234\n$hash", identity.signed.single())
        assertEq("review-android-1234-r", ev["id"])
        assertEq("AUTHOR", ev["author"])
        assertEq("alice", ev["authorName"])
        assertEq(1234L, ev["ts"])
        assertFails<CollabException> { signer.sign(CollabRecordKind.Discussion, "o", "r", 1, "edit", emptyMap()) }
        assertEq(
            "forkmesh-issue-event-v2\no/r\nvote\n2\nAUTHOR\n5\n${CollabSigner.sha256Hex("")}",
            signer.canonical(CollabRecordKind.Issue, "o", "r", "vote", 2, 5L, emptyMap()),
        )
        assertEq(
            "forkmesh-discussion-event-v1\ncomment\n3\nAUTHOR\n5\n${CollabSigner.sha256Hex("x")}",
            signer.canonical(CollabRecordKind.Discussion, "a/b", "r", "comment", 3, 5L, mapOf("body" to "x")),
        )
        assertEq("0\u00005", CollabContent.issue("dates", mapOf("startDate" to null, "endDate" to 5.9)))
        assertEq("a\u0000thumbs", CollabContent.issue("reaction", mapOf("target" to "a", "reaction" to "thumbs")))
        val anchored = CollabContent.pull("line-comment", mapOf("path" to "p", "side" to "new", "line" to 3L, "body" to "b", "patchset" to "ps"))
        assertEq("p\u0000new\u00003\u0000b\u0000ps\u0000", anchored)
    }

    fun testWriterPostsTheSignedEventToTheInbox() {
        val identity = EchoIdentity()
        val rec = RecordingCall()
        val writer = CollabRecordWriter(rec.call, CollabSigner(identity, now = { 5L }), "alice", "tool")
        writer.applyIssue(3, listOf(IssueEdit.status(false), IssueEdit.priority(9)))
        expect(identity.signed.all { it.startsWith("forkmesh-issue-event-v2\nalice/tool\n") })
        assertEq("forkmesh-pull-event-v2\nann/tool\n", EventSigning.prefix("pull-event", " Ann ", "TOOL"))
        assertEq("forkmesh-pull-event-v1\n", EventSigning.prefix("pull-event", "a/b", "c"))
        assertEq(listOf("POST /api/repo/alice/tool/issues", "POST /api/repo/alice/tool/issues"), rec.calls.map { it.first })
        @Suppress("UNCHECKED_CAST")
        val event = rec.calls[1].second!!["event"] as Map<String, Any?>
        assertEq(4L, event["priority"])
        rec.answer = ApiJsonResult(401, mapOf("error" to "bad_signature"))
        val failure = assertFails<CollabException> { writer.openDiscussion("t", "b", "general") }
        assertEq("The relay could not verify this device's signature.", failure.message)
        val guest = CollabRecordWriter(rec.call, null, "alice", "tool")
        assertFails<CollabException> { guest.sendIssue(1, IssueEdit.comment("x")) }
    }

    fun testAnchorsAndPlacement() {
        val side = DiffSideIndexes(patch).side("src/app.kt", DiffSide.New)!!
        assertEq(listOf(1, 2, 3, 4, 5), side.lines)
        assertEq("new two\nadded three", ReviewAnchoring.anchorLines(side, 2, 3))
        val old = DiffSideIndexes(patch).side("src/app.kt", DiffSide.Old)!!
        assertEq(listOf(1, 2, 3, 4), old.lines)
        val same = ReviewAnchoring.place(ReviewAnchoring.Anchor("src/app.kt", DiffSide.New, 2, 3, "ABC"), side, "abc")
        assertEq(ReviewThreadPlacement.Current, same.placement)
        val moved = ReviewAnchoring.place(ReviewAnchoring.Anchor("src/app.kt", DiffSide.New, 1, 1, "old", "line four"), side, "new")
        assertEq(ReviewAnchoring.Placement(ReviewThreadPlacement.Moved, 4, 4), moved)
        val gone = ReviewAnchoring.place(ReviewAnchoring.Anchor("src/app.kt", DiffSide.New, 1, 1, "old", "nowhere"), side, "new")
        expect(gone.isOutdated)
        val legacy = ReviewAnchoring.place(ReviewAnchoring.Anchor("src/app.kt", DiffSide.New, 9, 9), side, "")
        expect(legacy.isOutdated)
        assertEq("Moved from lines 1–3", ReviewAnchoring.label(ReviewThreadPlacement.Moved, 1, 3))
        assertEq(-1 to 5, (DiffSideIndexes.hunkStarts("@@ -1,4 +5 @@")?.let { -1 to it.second }))
    }

    fun testSnapshotThreadsAndReviewGate() {
        val events = listOf(
            ReviewEvent("line-comment", id = "c1", author = "bob", path = "src/app.kt", side = "new", line = 2, body = "why?"),
            ReviewEvent("thread-reply", threadId = "c1", author = "alice", body = "because"),
            ReviewEvent("thread-state", threadId = "c1", state = "resolved"),
            ReviewEvent("thread-comment", id = "t2", threadId = "t2", author = "bob", path = "src/app.kt", side = "new",
                lineStart = 1, lineEnd = 1, anchorLines = "gone", patchset = "old", suggestionPatch = "x"),
            ReviewEvent("review", author = "bob", authorName = "Bob", state = "request-changes"),
            ReviewEvent("review", author = "carol", state = "approve"),
            ReviewEvent("comment", author = "alice", body = "hi"),
        )
        val snapshot = ReviewSnapshotBuilder.build(events, patch, "new")
        assertEq(2, snapshot.totalThreads)
        assertEq(3, snapshot.topLevelItems)
        assertEq("changes_requested", snapshot.reviewSummary)
        val first = snapshot.thread("c1")!!
        expect(first.resolved)
        assertEq(2, first.paintedLine)
        assertEq(3, first.events.size)
        val second = snapshot.thread("t2")!!
        expect(second.isOutdated && second.hasSuggestion)
        assertEq(1, snapshot.outdatedThreads("src/app.kt").size)
        assertEq(ReviewFileSummary("src/app.kt", 2, 1, 1, 1, 1), snapshot.files["src/app.kt"])
        assertEq(listOf("Bob"), ReviewGate.changesRequestedBy(events, "alice"))
        assertEq(1, ReviewGate.independentApprovals(events, "alice"))
        expect(!ReviewGate.satisfied(events, "alice"))
        val withdrawn = events + ReviewEvent("review", author = "bob", state = "comment")
        expect(ReviewGate.satisfied(withdrawn, "alice"))
    }

    fun testComposerBuildsInlineThreadsAndReviews() {
        var n = 0
        val id = { "id${++n}" }
        val single = ReviewComposer.inlineComment(DiffLineSelection("src/app.kt", DiffSide.New, 3), "note\n\n", patch, "ps", newId = id)
        assertEq("line-comment", single.type)
        assertEq(linkedMapOf<String, Any?>(
            "id" to "id1", "path" to "src/app.kt", "side" to "new", "line" to 3L, "body" to "note",
            "patchset" to "ps", "anchorLines" to "added three",
        ), single.fields)
        val range = ReviewComposer.inlineComment(DiffLineSelection("src/app.kt", DiffSide.New, 4, 2), "r", patch, "", "s", id)
        assertEq("thread-comment", range.type)
        assertEq(2L, range.fields["lineStart"])
        assertEq("id2", range.fields["threadId"])
        assertEq("new two\nadded three\nline four", range.fields["anchorLines"])
        assertEq(ReviewComposer.REQUEST_CHANGES_NEEDS_NOTE, ReviewComposer.validateReview(ReviewVerdict.RequestChanges, " ", emptyList()))
        assertEq(null, ReviewComposer.validateReview(ReviewVerdict.Approve, "", emptyList()))
        val drafts = listOf(ReviewDraft("d", DiffLineSelection("src/app.kt", DiffSide.Old, 2), "old line"))
        val events = ReviewComposer.submission(ReviewVerdict.Comment, "", drafts, patch, "", id)
        assertEq(listOf("line-comment"), events.map { it.type })
        val book = ReviewDraftBook()
        assertEq(ReviewComposer.EMPTY_BODY, book.add(DiffLineSelection("a", DiffSide.New, 1), " ", "x"))
        assertEq(null, book.add(DiffLineSelection("a", DiffSide.New, 1), "ok", "x"))
        assertEq("1 pending comment", book.summary)
        val lines = parseUnifiedDiff(patch).single().hunks.single().lines
        val picked = DiffLineSelection.of("src/app.kt", lines[1])!!
        assertEq(DiffSide.Old, picked.side)
        val extended = DiffLineSelection.of("src/app.kt", lines[0])!!.extended(lines[4])
        assertEq(1 to 4, extended.lineStart to extended.lineEnd)
        expect(extended.contains(lines[3]))
        assertEq("src/app.kt · new lines 1–4", extended.label)
    }

    private class FakeLedger(val files: Map<String, String>, val comparePatch: String = "") : ReviewLedgerReader {
        override fun tree(owner: String, name: String, path: String, ref: String): RepoTree {
            assertEq(ReviewLedger.REF, ref)
            val entries = files.keys.filter { it.startsWith("$path/") }.map { RepoTreeEntry(it.substringAfterLast('/'), it, "file") }
            if (entries.isEmpty()) throw IllegalStateException("missing")
            return RepoTree(path, entries)
        }
        override fun blob(owner: String, name: String, path: String, ref: String) = RepoBlob(path, files.getValue(path))
        override fun compare(owner: String, name: String, base: String, head: String) = RepoComparison(patch = comparePatch)
    }

    fun testLedgerAndSessionSendAndRebuild() {
        val head = "a".repeat(40)
        val files = mapOf(
            ".forkmesh/pulls/4/pull.md" to "---\ntitle: Add thing\nauthor: alice\nbase: main\nhead: feat\ncreationBaseOid: ${"b".repeat(40)}\ncreationHeadOid: $head\n---\nBody text",
            ".forkmesh/pulls/4/0002-review.md" to "---\ntype: review\nauthor: bob\nstate: approve\n---\n",
            ".forkmesh/pulls/4/0001-line.md" to "---\ntype: line-comment\nid: c1\nauthor: bob\npath: src/app.kt\nside: new\nline: 2\nanchorLinesB64: ${ReviewEvent.encodeBlob("new two")}\n---\nwhy",
        )
        val rec = RecordingCall()
        val session = PullReviewSession("alice", "tool", 4, FakeLedger(files, patch), CollabRecordWriter(rec.call, CollabSigner(EchoIdentity()), "alice", "tool"))
        session.load()
        assertEq(null, session.loadError)
        val record = session.record!!
        assertEq("Add thing", record.title)
        assertEq(head, record.currentPatchset)
        assertEq(listOf("line-comment", "review"), record.events.map { it.type })
        assertEq("new two", record.events[0].anchorLines)
        assertEq(patch, record.patch)
        assertEq(1, session.threadCount("src/app.kt"))
        assertEq("approved", session.snapshot.reviewSummary)
        val lines = session.files.single().hunks.single().lines
        session.tap("src/app.kt", lines[2])
        assertEq(DiffLineSelection("src/app.kt", DiffSide.New, 2), session.selection)
        val notice = session.commentNow("looks off")
        assertEq(false, notice.isError)
        assertEq(null, session.selection)
        assertEq(1, session.pending.size)
        assertEq(2, session.snapshot.totalThreads)
        session.tap("src/app.kt", lines[3])
        assertEq(null, session.addDraft("draft one"))
        val submitted = session.submit(ReviewVerdict.Approve, "")
        assertEq("Approve sent.", submitted.message)
        assertEq(0, session.drafts.count)
        assertEq(3, rec.calls.size)
        session.toggleViewed("src/app.kt")
        assertEq("1 of 1 viewed", session.viewedSummary)
        val missing = PullReviewSession("alice", "tool", 9, FakeLedger(files), CollabRecordWriter(rec.call, null, "alice", "tool"))
        missing.load()
        assertEq("Pull request #9 could not be read from the pull ledger.", missing.loadError)
    }

    fun testBranchCommentSigningVector() {
        val identity = object : DeviceIdentity {
            val signed = ArrayList<String>()
            override val publicKeyBytes = ByteArray(32)
            override val publicKeyB64url = "AUTHORKEY"
            override val nodeId = "AUTHORKEY"
            override val shortKey = "AUTH"
            override fun sign(data: ByteArray): String {
                signed.add(String(data, Charsets.UTF_8))
                return "sig"
            }
            override fun exportSeed() = ByteArray(32)
            override fun rotate() {}
        }
        val event = linkedMapOf<String, Any?>(
            "threadId" to "thread-comment-ios-1", "path" to "src/app.txt", "side" to "new", "lineStart" to 3L, "lineEnd" to 5L,
            "body" to "Rename these", "suggestionPatch" to "", "patchset" to "", "anchorLines" to "alpha\nbeta\ngamma",
        )
        val hash = "02d13720c8a1e971d05805561c7378eba454f51998fd5c8f447bf6569efb64ce"
        assertEq(hash, CollabSigner.sha256Hex(CollabContent.pull("thread-comment", event)))
        val signer = CollabSigner(identity, now = { 1790000000000L }, random = { "r" })
        assertEq(
            "forkmesh-branch-comment-v2\nalice/demo\nthread-comment\nfeature/review\nAUTHORKEY\n1790000000000\n$hash",
            signer.canonicalBranch("alice", "demo", "feature/review", "thread-comment", 1790000000000L, event),
        )
        assertEq(
            "d9c05bb661d5f22bce769db007e4fa6831fb49282b9e041ead75f52ef517468a",
            CollabSigner.sha256Hex(CollabContent.pull("thread-reply", mapOf("threadId" to "thread-comment-ios-1", "parentId" to "thread-comment-ios-1", "body" to "Done"))),
        )
        assertEq(
            "ce3bf1a2fd60719b415f48680b213edf965f71ecc6154233b02dc3c82c6c2c5f",
            CollabSigner.sha256Hex(CollabContent.pull("thread-state", mapOf("threadId" to "thread-comment-ios-1", "state" to "resolved", "body" to ""))),
        )
        val withoutSuggestion = event.filterKeys { it != "suggestionPatch" }
        assertEq(hash, CollabSigner.sha256Hex(CollabContent.pull("thread-comment", withoutSuggestion)))
        val ev = signer.signBranch("alice", "demo", "feature/review", "thread-comment", event)
        assertEq("$hash", identity.signed.single().substringAfterLast('\n'))
        assertEq("AUTHORKEY", ev["author"])
        assertEq(1790000000000L, ev["ts"])
        assertEq("sig", ev["sig"])
        assertFails<CollabException> { signer.signBranch("alice", "demo", "feature/review", "line-comment", event) }
        assertFails<CollabException> { signer.signBranch("alice", "demo", "feature/review", "review", emptyMap()) }
        assertFails<CollabException> { signer.canonicalBranch("a/b", "demo", "feature/review", "thread-comment", 1L, event) }
    }

    fun testBranchLedgerKeyAndLoad() {
        assertEq("306395bddc1b0cac0297da67", BranchReviewLedger.key("feature/review"))
        assertEq(".forkmesh/branch-reviews/306395bddc1b0cac0297da67", BranchReviewLedger.folder("feature/review"))
        val folder = BranchReviewLedger.folder("feat/x")
        val head = "a".repeat(40)
        val files = mapOf(
            "$folder/branch.md" to "---\nbranch: feat/x\n---\n",
            "$folder/0002-thread-reply.md" to "---\ntype: thread-reply\nid: r1\nauthor: alice\nts: 2\nthreadId: t1\nparentId: t1\nsig: s\n---\nok",
            "$folder/0001-thread-comment.md" to "---\ntype: thread-comment\nid: t1\nauthor: bob\nauthorName: Bob\nts: 1\nthreadId: t1\npath: src/app.kt\n" +
                "side: new\nlineStart: 2\nlineEnd: 3\npatchset: $head\nanchorLinesB64: ${ReviewEvent.encodeBlob("new two\nadded three")}\nsig: s\n---\nRename these",
            "$folder/0003-thread-state.md" to "---\ntype: thread-state\nid: s1\nauthor: bob\nts: 3\nthreadId: t1\nstate: resolved\nsig: s\n---\n",
            "$folder/0004-review.md" to "---\ntype: review\nauthor: bob\nstate: approve\n---\n",
            "$folder/0005-thread-comment.md" to "---\ntype: thread-comment\nid: f1\nauthor: bob\nts: 5\nthreadId: f1\npath: src/app.kt\nside: new\nlineStart: 0\nlineEnd: 0\nsig: s\n---\nWhole file",
            "$folder/notes.txt" to "x",
            "$folder/12-thread-reply.md" to "---\ntype: thread-reply\nthreadId: t1\n---\nbad name",
        )
        val events = BranchReviewLedger.load(FakeLedger(files), "alice", "tool", "feat/x")
        assertEq(listOf("t1", "r1", "s1", "f1"), events.map { it.id })
        assertEq("new two\nadded three", events[0].anchorLines)
        assertEq(emptyList<ReviewEvent>(), BranchReviewLedger.load(FakeLedger(files), "alice", "tool", "other"))
        expect(BranchReviewLedger.canComment("feat/x", "main"))
        expect(!BranchReviewLedger.canComment("main", "main"))
        expect(!BranchReviewLedger.canComment(" ", "main"))
        expect(!BranchReviewLedger.canComment(head, "main"))

        val session = BranchReviewSession("alice", "tool", "feat/x", FakeLedger(files), CollabRecordWriter(RecordingCall().call, null, "alice", "tool"))
        session.attach(patch, head.uppercase())
        session.load()
        assertEq(head, session.patchset)
        val thread = session.snapshot.thread("t1")!!
        expect(thread.resolved)
        assertEq(ReviewThreadPlacement.Current, thread.placement)
        assertEq(3, thread.events.size)
        val lines = parseUnifiedDiff(patch).single().hunks.single().lines
        assertEq(listOf("t1"), session.threadsUnder("src/app.kt", lines[3]).map { it.id })
        assertEq(emptyList<ReviewThread>(), session.threadsUnder("src/app.kt", lines[2]))
        assertEq(listOf("t1"), session.threadsUnder("src/app.kt", null, lines[3]).map { it.id })
        assertEq(listOf("f1"), session.fileThreads("src/app.kt").map { it.id })
        expect(!session.canWrite)
        session.tap("src/app.kt", lines[2])
        assertEq(null, session.selection)
        session.attach(patch, "")
        assertEq(ReviewThreadPlacement.Current, session.snapshot.thread("t1")!!.placement)
        session.attach(patch.replace(" line one\n", " line one\n zero\n").replace("@@ -1,4 +1,5 @@", "@@ -1,5 +1,6 @@"), "b".repeat(40))
        assertEq(ReviewThreadPlacement.Moved, session.snapshot.thread("t1")!!.placement)
        assertEq(4, session.snapshot.thread("t1")!!.paintedLine)
    }

    fun testBranchSessionSelectsSignsAndPosts() {
        val head = "c".repeat(40)
        val identity = EchoIdentity()
        val rec = RecordingCall()
        rec.answer = ApiJsonResult(201, mapOf("ok" to true))
        val writer = CollabRecordWriter(rec.call, CollabSigner(identity, now = { 77L }, authorName = "carol"), "alice", "tool")
        val session = BranchReviewSession("alice", "tool", "feat/x", FakeLedger(emptyMap()), writer, now = { 77L })
        session.attach(patch, head)
        session.load()
        expect(session.loaded && session.snapshot.threads.isEmpty())
        assertEq(CollabNotice("Pick a line first.", true), session.commentNow("x"))
        val lines = parseUnifiedDiff(patch).single().hunks.single().lines
        session.tap("src/app.kt", lines[2])
        session.tap("src/app.kt", lines[4])
        assertEq(DiffLineSelection("src/app.kt", DiffSide.New, 2, 4), session.selection)
        expect(session.isSelected("src/app.kt", lines[3]) && !session.isSelected("src/app.kt", lines[1]))
        session.tap("src/app.kt", lines[1])
        assertEq(DiffLineSelection("src/app.kt", DiffSide.Old, 2), session.selection)
        session.tap("src/app.kt", lines[1])
        assertEq(null, session.selection)
        session.tap("src/app.kt", lines[2])
        session.tap("src/app.kt", lines[3])
        assertEq(ReviewComposer.EMPTY_BODY, session.commentNow("  ").message)
        val sent = session.commentNow("Rename these\n")
        expect(!sent.isError)
        assertEq(null, session.selection)
        val (target, body) = rec.calls.single()
        assertEq("POST /api/repo/alice/tool/pulls", target)
        assertEq(listOf("branch", "event"), body!!.keys.toList())
        assertEq("feat/x", body["branch"])
        @Suppress("UNCHECKED_CAST")
        val event = body["event"] as Map<String, Any?>
        assertEq("thread-comment", event["type"])
        assertEq(event["id"], event["threadId"])
        assertEq(2L, event["lineStart"])
        assertEq(3L, event["lineEnd"])
        assertEq("new", event["side"])
        assertEq("Rename these", event["body"])
        assertEq(head, event["patchset"])
        assertEq("new two\nadded three", event["anchorLines"])
        assertEq("carol", event["authorName"])
        assertEq(77L, event["ts"])
        expect(event["line"] == null && event["suggestionPatch"] == null)
        val hash = CollabSigner.sha256Hex(CollabContent.pull("thread-comment", event))
        assertEq("forkmesh-branch-comment-v2\nalice/tool\nthread-comment\nfeat/x\nAUTHOR\n77\n$hash", identity.signed.single())
        val thread = session.snapshot.threads.single()
        expect(session.isPending(thread.events.single()))
        assertEq(listOf(thread.id), session.threadsUnder("src/app.kt", lines[3]).map { it.id })
        expect(!session.reply(thread, "Done").isError)
        expect(!session.setResolved(thread, true).isError)
        val reopened = session.snapshot.thread(thread.id)!!
        expect(reopened.resolved)
        expect(!session.setResolved(reopened, false).isError)
        @Suppress("UNCHECKED_CAST")
        val states = rec.calls.map { (it.second!!["event"] as Map<String, Any?>) }
        assertEq(listOf("thread-comment", "thread-reply", "thread-state", "thread-state"), states.map { it["type"] })
        assertEq(thread.id, states[1]["parentId"])
        assertEq(listOf("resolved", "unresolved"), states.drop(2).map { it["state"] })
        expect(!session.snapshot.thread(thread.id)!!.resolved)
        rec.answer = ApiJsonResult(200, mapOf("ok" to true))
        session.tap("src/app.kt", lines[0])
        val refused = session.commentNow("again")
        expect(refused.isError)
        expect(session.selection != null)
        assertEq(4, session.pending.size)
        val guest = CollabRecordWriter(rec.call, null, "alice", "tool")
        assertFails<CollabException> { guest.sendBranch("feat/x", ReviewComposer.reply(thread, "x")) }
    }

    fun testMergeRulesAndClient() {
        val plan = PullMergePlan(3, "b".repeat(40), "c".repeat(40), "d".repeat(40))
        val approve = listOf(ReviewEvent("review", author = "bob", state = "approve"))
        assertEq(PullMergeState.Checking, PullMergeRules.state(PullMergeInputs("open", "alice", approve, null, plan, true)))
        assertEq(PullMergeState.NotOwner, PullMergeRules.state(PullMergeInputs("open", "alice", approve, false, plan, true)))
        assertEq(PullMergeState.NotPinned, PullMergeRules.state(PullMergeInputs("open", "alice", approve, true, plan.copy(expectedHeadOid = "short"), true)))
        assertEq(PullMergeState.Ready, PullMergeRules.state(PullMergeInputs("open", "alice", approve, true, plan, true)))
        assertEq(PullMergeState.NeedsIndependentReview, PullMergeRules.state(PullMergeInputs("open", "alice", emptyList(), true, plan, true)))
        val conflicts = PullMergeState.Conflicts((1..9).map { "f$it" })
        assertEq("9 files conflict with main: f1, f2, f3, f4, f5, f6, f7, f8 and 1 more.", PullMergeRules.headline(conflicts, "main"))
        assertEq("Merged.", PullMergeRules.headline(PullMergeRules.state(PullMergeInputs("Merged", "", emptyList(), null, null, false)), ""))

        class Reader : PullMergeReader {
            val answers = ArrayDeque(listOf(202 to mapOf<String, Any?>(), 200 to mapOf<String, Any?>("status" to "merged")))
            val bodies = ArrayList<Map<String, Any?>>()
            val cleared = ArrayList<String>()
            override fun merged(owner: String, name: String) {
                cleared.add("$owner/$name")
            }
            override fun treeCommit(owner: String, name: String, path: String, ref: String) = "D".repeat(40)
            override fun branches(owner: String, name: String) = listOf(RepoBranch("main", "B".repeat(40)))
            override fun mergePreview(owner: String, name: String, base: String, head: String) = mapOf<String, Any?>(
                "baseOid" to "b".repeat(40), "headOid" to "c".repeat(40), "clean" to false, "conflicts" to listOf("x.kt"),
            )
            override fun postMerge(owner: String, name: String, number: Int, body: Map<String, Any?>): Pair<Int, Map<String, Any?>> {
                bodies.add(body)
                return answers.removeFirst()
            }
        }
        val reader = Reader()
        val client = PullMergeClient(reader, sleep = {}, requestId = { "mobile_x" })
        val built = client.plan("alice", "tool", PublishedPull(3, "t", base = "main", head = "feat", branchBacked = true))!!
        assertEq(PullMergePlan(3, "b".repeat(40), "c".repeat(40), "d".repeat(40), clean = false, conflicts = listOf("x.kt")), built)
        val patchPlan = client.plan("alice", "tool", PublishedPull(3, "t", base = "main"))!!
        expect(!patchPlan.branchBacked && patchPlan.expectedHeadOid == "d".repeat(40))
        val contributed = client.plan("alice", "tool", PublishedPull(3, "t", base = "main", head = "contributor/fix"))!!
        expect(!contributed.branchBacked && contributed.expectedHeadOid == "d".repeat(40) && contributed.expectedBaseOid == "b".repeat(40))
        assertEq(null, client.plan("alice", "tool", PublishedPull(3, "t", status = "closed", base = "main")))
        val outcome = client.merge("alice", "tool", plan, overrideReview = true)
        expect(outcome.merged)
        assertEq(2, reader.bodies.size)
        assertEq(true, reader.bodies[0]["overrideReview"])
        assertEq("forkmesh.pull-merge-v1", reader.bodies[0]["type"])
        assertEq(
            listOf("schemaVersion", "type", "pullNumber", "requestId", "expectedBaseOid", "expectedHeadOid", "expectedPullsOid", "overrideReview"),
            reader.bodies[0].keys.toList(),
        )
        assertEq(listOf("alice/tool"), reader.cleared)
        val identity = Identity.fromSeed(ByteArray(32) { it.toByte() })
        val signingReader = Reader()
        val signing = PullMergeClient(signingReader, sleep = {}, requestId = { "mobile_y" }, approver = DeployApprover({ identity }, clock = { 1791500000000L }))
        expect(signing.merge("Alice", "tool", plan).merged)
        assertEq(2, signingReader.bodies.size)
        assertEq(
            listOf("schemaVersion", "type", "pullNumber", "requestId", "expectedBaseOid", "expectedHeadOid", "expectedPullsOid", "deployApproval"),
            signingReader.bodies[0].keys.toList(),
        )
        val expected = DeployApproval.sign(identity, "pull", "alice", "tool", "3", "b".repeat(40), "c".repeat(40), "mobile_y", 1791500000000L)
        assertEq(expected, signingReader.bodies[0]["deployApproval"])
        assertEq(expected, signingReader.bodies[1]["deployApproval"])
        val keylessReader = Reader()
        expect(PullMergeClient(keylessReader, sleep = {}, requestId = { "mobile_z" }, approver = DeployApprover({ null })).merge("alice", "tool", plan).merged)
        expect(keylessReader.bodies.none { "deployApproval" in it })
        reader.answers.add(409 to mapOf("error" to "stale_head"))
        assertEq(
            "The repository moved while this was being prepared. Refresh and merge again.",
            assertFails<CollabException> { client.merge("alice", "tool", plan) }.message,
        )
        repeat(PullMergeClient.POLL_ATTEMPTS) { reader.answers.add(202 to emptyMap()) }
        expect(client.merge("alice", "tool", plan).pending)
        assertEq(listOf("alice/tool"), reader.cleared)
    }

    private fun issue(number: Int, labels: List<String> = emptyList(), status: String = "open", priority: Int = 0) =
        Issue(number, "Issue $number", status = status, labels = labels, priority = priority)

    fun testIssueBoardLayoutMovesAndSession() {
        val columns = IssueBoard.DEFAULT_COLUMNS
        val issues = listOf(issue(1), issue(2, listOf("status:todo"), priority = 3), issue(3, status = "closed"), issue(4, listOf("Status:Todo")))
        val layout = IssueBoard.layout(issues, columns)
        assertEq(listOf(listOf(1), listOf(2, 4), emptyList(), listOf(3)), layout.map { col -> col.issues.map { it.number } })
        val toProgress = IssueBoard.move(issues[1], "in progress", columns)!!
        assertEq(listOf(IssueEdit.labels(listOf("status:in progress"))), toProgress.edits)
        val toDone = IssueBoard.move(issues[1], "Done", columns)!!
        assertEq(listOf("labels", "status"), toDone.edits.map { it.type })
        val reopen = IssueBoard.move(issues[2], "Backlog", columns)!!
        assertEq(listOf(IssueEdit.labels(listOf("status:backlog")), IssueEdit.status(true)), reopen.edits)
        assertEq(null, IssueBoard.move(issues[0], "Backlog", columns))
        assertEq(IssueBoard.TOO_FEW_COLUMNS, IssueBoard.validate(listOf("A", "a", " ")))
        assertEq("Todo" to "Done", IssueBoard.neighbours("in progress", columns))
        val store = MemoryPrefStore()
        val rec = RecordingCall()
        val session = IssueBoardSession("alice", "tool", store, CollabRecordWriter(rec.call, CollabSigner(EchoIdentity()), "alice", "tool")) { issues }
        session.refresh()
        assertEq(null, session.saveColumns(listOf("Open", "Closed")))
        assertEq(listOf("Open", "Closed"), IssueBoard.load(store, "alice", "tool"))
        assertEq("Moved #1 to Closed.", session.move(issues[0], "Closed")!!.message)
        assertEq("closed", session.issues.first { it.number == 1 }.status)
        session.query = "#2"
        assertEq(1, session.layout.sumOf { it.issues.size })
    }

    fun testIssueEditsChecklistLinksAndText() {
        val base = Issue(5, "Title", labels = listOf("bug"), priority = 1, assignees = listOf("bob"))
        val edits = IssueFields.diff(base, title = "Title ", labels = listOf("bug", "Bug", "ui"), assignees = listOf("Bob"), priority = 7, open = false)
        assertEq(listOf("labels", "priority", "status"), edits.map { it.type })
        assertEq(listOf("bug", "ui"), edits[0].fields["labels"])
        assertEq("Urgent", IssueFields.priorityLabel(9))
        assertEq(listOf("a", "b"), IssueFields.parseList("a, b\n,"))
        val body = "Intro\n- [ ] one\n  * [x] two\n```\n- [ ] fenced\n```\n- [] bad"
        assertEq(listOf(1 to false, 2 to true), IssueChecklist.items(body).map { it.lineIndex to it.done })
        assertEq(1 to 2, IssueChecklist.progress(body))
        assertEq("Intro\n- [x] one\n  * [x] two\n```\n- [ ] fenced\n```\n- [] bad", IssueChecklist.toggled(body, 1))
        assertEq(null, IssueChecklist.toggled(body, 4))
        assertEq(listOf(12, 4), IssueLinks.linkedIssueNumbers("This fixes #12. Also Closes: #4, resolves #12"))
        assertEq("Body\n\nCloses #3", IssueLinks.bodyLinking(" Body ", 3))
        assertEq("fixes #3", IssueLinks.bodyLinking("fixes #3", 3))
    }

    fun testModelMentions() {
        val mentions = IssueModelMentions.parse("Hey @opus can you fix the crash? @Codex. what do you think")
        assertEq(listOf("opus", "codex"), mentions.map { it.alias })
        assertEq("claude-opus-5", mentions[0].model)
        assertEq("claude-code", mentions[0].provider)
        assertEq("can you fix the crash?", mentions[0].request)
        assertEq(ModelMention.Intent.Work, mentions[0].intent)
        assertEq(ModelMention.Intent.Answer, mentions[1].intent)
        assertEq("claude-foo", IssueModelMentions.modelForAlias("claude-foo."))
        expect(IssueModelMentions.parse("mail me at a@opus.com").isEmpty())
        expect(!IssueModelMentions.requestsWork("how would you fix this"))
        expect(!IssueModelMentions.requestsWork("does this fix it?"))
        expect(IssueModelMentions.requestsWork("could you fix it?"))
        expect(IssueModelMentions.requestsWork("please start an agent"))
        val solo = IssueModelMentions.parse("@sonnet")
        assertEq("", solo.single().request)
    }

    fun testReferenceLinks() {
        val repo = RepoRef("alice", "tool")
        val refs = ReferenceLinks.references("See #12, bob/lib#3 and PR #7 at deadbeef1. `#9` https://x.test/a.")
        assertEq(listOf("#12", "bob/lib#3", "PR #7", "deadbeef1", "https://x.test/a"), refs.map { it.label })
        assertEq(RepoRef("bob", "lib"), refs[1].repo)
        assertEq(ReferenceKind.Pull, refs[2].kind)
        assertEq("forkmesh://alice/tool/pulls/7", refs[2].link(repo))
        assertEq("[#2](forkmesh://alice/tool/issues/2) ok", ReferenceLinks.markdown("#2 ok", repo))
        expect(ReferenceLinks.references("```\n#4\n```").isEmpty())
        expect(ReferenceLinks.references("abc#4 cafe12").isEmpty())
    }

    fun testReferenceRuns() {
        val repo = RepoRef("alice", "tool")
        assertEq(
            listOf(
                ReferenceRun("Fixes ", null),
                ReferenceRun("#7", "forkmesh://alice/tool/issues/7"),
                ReferenceRun(" via ", null),
                ReferenceRun("PR #8", "forkmesh://alice/tool/pulls/8"),
                ReferenceRun(" (", null),
                ReferenceRun("abc1234", "forkmesh://alice/tool/commit/abc1234"),
                ReferenceRun("), see ", null),
                ReferenceRun("https://x.test/a", "https://x.test/a"),
                ReferenceRun(".", null),
            ),
            ReferenceLinks.runs("Fixes #7 via PR #8 (abc1234), see https://x.test/a.", repo),
        )
        assertEq(
            listOf(ReferenceRun("abc1234 ", null), ReferenceRun("#2", "forkmesh://alice/tool/issues/2")),
            ReferenceLinks.runs("abc1234 #2", repo, commits = false),
        )
        assertEq(
            listOf(
                ReferenceRun("#7, PR #8, abc1234 and ", null),
                ReferenceRun("bob/lib#3", "forkmesh://bob/lib/issues/3"),
                ReferenceRun(" ", null),
                ReferenceRun("forkmesh://tasks/ab12", "forkmesh://tasks/ab12"),
            ),
            ReferenceLinks.runs("#7, PR #8, abc1234 and bob/lib#3 forkmesh://tasks/ab12", null),
        )
        assertEq(listOf(ReferenceRun("plain words", null)), ReferenceLinks.runs("plain words", null))
        assertEq(RepoRef("bob", "lib"), ReferenceLinks.repoFor("forkmesh://alice/tool/issues/2", " bob/lib "))
        assertEq(RepoRef("alice", "tool"), ReferenceLinks.repoFor("https://app.forkmesh.com/alice/tool/pulls/3"))
        assertEq(RepoRef("carol", "app"), ReferenceLinks.repoFor("/carol/app/issues/4"))
        assertEq(ReferenceLinks.CHAT_REPO, ReferenceLinks.repoFor("/inbox", "nope"))
    }

    fun testRepoSettingsAboutPatchAndClient() {
        val old = RepoAbout(description = "a", website = "https://a.test")
        assertEq(emptyMap<String, Any?>(), RepoSettingsRules.aboutPatch(old, old.copy(website = " https://a.test ")))
        val patch = RepoSettingsRules.aboutPatch(old, old.copy(description = "b", acceptComments = false))
        assertEq(setOf("description", "fediverse"), patch.keys)
        assertEq("Enter a full http(s) address.", RepoSettingsRules.validateWebsite("ftp://x"))
        assertEq(null, RepoSettingsRules.validateWebsite(""))
        expect(RepoSettingsRules.canEdit("Alice", "alice", false))
        expect(!RepoSettingsRules.canEdit("alice", "bob", false))
        expect(RepoSettingsRules.confirmsName(" tool ", "tool"))
        val rec = RecordingCall()
        rec.answer = ApiJsonResult(200, mapOf("fork" to mapOf("owner" to "bob", "name" to "tool"), "queued" to true))
        val client = RepoSettingsClient(rec.call, "alice", "tool") { "tok" }
        assertEq("Fork recorded. Your managed mirror creates it the next time it is online.", client.fork(" Mirror ").message)
        assertEq(mapOf<String, Any?>("targetNode" to "mirror", "sessionToken" to "tok"), rec.calls.single().second)
        rec.answer = ApiJsonResult(200, mapOf("about" to "x"))
        expect(!client.saveAbout(old, old))
        assertEq(1, rec.calls.size)
        assertEq(RepoAbout(followers = 3, fediverseHandle = "@t", acceptComments = false), RepoAbout.fromJson(
            mapOf("fediverse" to mapOf("handle" to "@t", "followers" to 3L, "settings" to mapOf("acceptComments" to false))),
        ))
    }

    fun testMergeQueueRulesAndRemote() {
        val repo = MergeQueueRepo.fromJson(
            mapOf(
                "owner" to "alice", "name" to "tool", "enabled" to true,
                "entries" to listOf(
                    mapOf("number" to 4L, "state" to "merging"),
                    mapOf("number" to 5L, "state" to "blocked"),
                    mapOf("number" to 6L),
                ),
            ),
        )!!
        assertEq("3 queued · 1 in progress · 1 blocked", repo.summary)
        assertEq(listOf(MergeQueueAction.Pause, MergeQueueAction.Process, MergeQueueAction.Clear, MergeQueueAction.Disable), MergeQueueRules.actions(repo))
        assertEq(emptyList<Pair<MergeQueueAction, Int>>(), MergeQueueRules.entryActions(repo.entries[0], repo))
        assertEq(listOf(MergeQueueAction.Move to -1, MergeQueueAction.Remove to 0), MergeQueueRules.entryActions(repo.entries[2], repo))
        expect(MergeQueueRules.canEnqueue("open", repo, 9))
        expect(!MergeQueueRules.canEnqueue("open", repo, 5))
        assertEq(listOf(MergeQueueAction.Enable), MergeQueueRules.actions(repo.copy(enabled = false)))
        val calls = ArrayList<Pair<String, Map<String, Any?>>>()
        val remote = MergeQueueRemote { suffix, params, _ ->
            calls.add(suffix to params)
            when (suffix) {
                "prepare" -> mapOf("token" to "t1")
                "run" -> mapOf("message" to "ok", "queue" to mapOf("owner" to "alice", "name" to "tool", "enabled" to false))
                else -> mapOf("repos" to listOf(mapOf("owner" to "alice", "name" to "tool")))
            }
        }
        val result = remote.perform(MergeQueueRequest("alice", "tool", MergeQueueAction.Move, 6, -1))
        assertEq("ok", result.message)
        assertEq(mapOf<String, Any?>("owner" to "alice", "name" to "tool", "action" to "move", "number" to 6L, "delta" to -1L, "token" to "t1"), calls[1].second)
        assertEq(1, remote.list("alice", "tool").size)
        val empty = MergeQueueRemote { _, _, _ -> emptyMap() }
        assertEq("confirmation_missing", assertFails<LocalControlException> { empty.prepare(MergeQueueRequest("a", "b", MergeQueueAction.Pause)) }.code)
        assertEq("The confirmation expired. Try again.", MergeQueueRemote.message(LocalControlException("expired")))
    }
}
