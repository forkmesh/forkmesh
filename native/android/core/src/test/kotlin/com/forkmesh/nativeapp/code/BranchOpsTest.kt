package com.forkmesh.nativeapp.code

import com.forkmesh.nativeapp.api.ApiJsonResult
import com.forkmesh.nativeapp.ci.RepoActionsModel
import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.json.asJsonMap
import com.forkmesh.nativeapp.model.RepoBranch
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class BranchOpsTest {
    private val tip = "a".repeat(40)
    private val mainTip = "b".repeat(40)
    private val feature = RepoBranch("feature/x", tip.uppercase())
    private val main = RepoBranch("main", mainTip, isDefault = true)

    private class Fake(private val answers: MutableList<Pair<Int, String>>) {
        val paths = ArrayList<String>()
        val bodies = ArrayList<Map<String, Any?>>()

        fun post(path: String, body: Map<String, Any?>): ApiJsonResult {
            paths += path
            bodies += body
            val (status, text) = if (answers.size > 1) answers.removeAt(0) else answers.first()
            return ApiJsonResult(status, Json.parse(text).asJsonMap())
        }
    }

    private fun client(fake: Fake, sleeps: MutableList<Long> = ArrayList(), maxPolls: Int = BranchOpsClient.MAX_POLLS) =
        BranchOpsClient(fake::post, sleep = { sleeps += it }, maxPolls = maxPolls)

    fun testRequestBodies() {
        val merge = BranchOpRequest.merge(feature, main, deleteBranch = true)
        assertEq(
            mapOf(
                "op" to "merge", "branch" to "feature/x", "expectedOid" to tip, "defaultBranch" to "main",
                "base" to "main", "baseOid" to mainTip, "deleteBranch" to true,
            ),
            merge.body(),
        )
        assertEq(false, BranchOpRequest.merge(feature, main).body()["deleteBranch"])
        assertEq(
            mapOf("op" to "update", "branch" to "feature/x", "expectedOid" to tip, "defaultBranch" to "main"),
            BranchOpRequest.update(feature, "main").body(),
        )
        assertEq(
            mapOf("op" to "delete", "branch" to "feature/x", "expectedOid" to tip, "defaultBranch" to "main"),
            BranchOpRequest.delete(feature, "main").body(),
        )
        val resumed = BranchOpRequest.update(feature, "main").body(BranchOpResume("req-1", "fmnext-n2", "main", mainTip))
        assertEq("req-1", resumed["requestId"])
        assertEq("fmnext-n2", resumed["node"])
        assertEq("main", resumed["pinBase"])
        assertEq(mainTip, resumed["pinOid"])
        val noPin = BranchOpRequest.delete(feature, "main").body(BranchOpResume("req-2", "fmnext-n1", "main"))
        expect(!noPin.containsKey("pinOid"))
        assertEq("/api/repo/alice/app/branches/ops", BranchOpsClient.path("alice", "app"))
    }

    fun testAnswerParsing() {
        val processing = BranchOpAnswer.parse(
            202,
            Json.parse("""{"ok":true,"executed":true,"status":"processing","requestId":"r1","node":"fmnext-n1","pinBase":"main","message":"Merging feature/x into main…"}""").asJsonMap(),
        )
        assertEq(BranchOpState.Processing, processing.state)
        assertEq(BranchOpResume("r1", "fmnext-n1", "main"), processing.resume)
        val merged = BranchOpAnswer.parse(
            200,
            Json.parse("""{"ok":true,"executed":true,"status":"merged","branch":"feature/x","base":"main","mergeOid":"C0FFEE1234567","branchDeleted":true,"cleanupPending":["fmnext-n3"],"message":""}""").asJsonMap(),
        )
        assertEq(BranchOpState.Done, merged.state)
        expect(merged.landedOnBase && merged.branchDeleted)
        assertEq("c0ffee1234567", merged.mergeOid)
        val queued = BranchOpAnswer.parse(200, Json.parse("""{"ok":true,"queued":true,"queuedCount":1,"ops":[]}""").asJsonMap())
        assertEq(BranchOpState.Queued, queued.state)
        expect(!queued.alreadyWaiting)
        val waiting = BranchOpAnswer.parse(200, Json.parse("""{"ok":true,"queued":false,"queuedCount":0,"ops":[]}""").asJsonMap())
        expect(waiting.state == BranchOpState.Queued && waiting.alreadyWaiting)
        val conflict = BranchOpAnswer.parse(
            409,
            Json.parse("""{"ok":false,"executed":true,"status":"failed","error":"merge_conflict","message":"feature/x does not merge cleanly into main.","conflicts":["a.txt","b.txt"]}""").asJsonMap(),
        )
        expect(conflict.state == BranchOpState.Failed && conflict.isConflict && !conflict.retryable)
        assertEq(listOf("a.txt", "b.txt"), conflict.conflicts)
        val pending = BranchOpAnswer.parse(503, Json.parse("""{"ok":false,"error":"merge_status_pending"}""").asJsonMap())
        expect(pending.state == BranchOpState.Failed && pending.retryable)
    }

    fun testWording() {
        val merge = BranchOpRequest.merge(feature, main)
        val update = BranchOpRequest.update(feature, "main")
        val delete = BranchOpRequest.delete(feature, "main")
        assertEq("Merge into main", BranchOpWording.label(BranchOpKind.Merge, "main"))
        assertEq("Merge & delete", BranchOpWording.label(BranchOpKind.Merge, "main", deleteBranch = true))
        assertEq("Update from main", BranchOpWording.label(BranchOpKind.Update, "main"))
        assertEq("Delete", BranchOpWording.label(BranchOpKind.Delete, "main"))
        assertEq("Delete feature/x?", BranchOpWording.confirmTitle(delete))
        expect(BranchOpWording.confirmBody(delete).startsWith("Deletes feature/x at aaaaaaaa."))
        assertEq(
            "Merging feature/x into main is queued; it runs when the owner's desktop is online.",
            BranchOpWording.outcome(merge, BranchOpAnswer(200, BranchOpState.Queued)),
        )
        assertEq(
            "Deleting feature/x is already waiting; it runs when the owner's desktop is online.",
            BranchOpWording.outcome(delete, BranchOpAnswer(200, BranchOpState.Queued, alreadyWaiting = true)),
        )
        assertEq(
            "Merged feature/x into main and deleted it. fmnext-n3 still to catch up.",
            BranchOpWording.outcome(merge, BranchOpAnswer(200, BranchOpState.Done, status = "merged", branchDeleted = true, cleanupPending = listOf("fmnext-n3"))),
        )
        assertEq("Updated feature/x from main.", BranchOpWording.outcome(update, BranchOpAnswer(200, BranchOpState.Done, status = "updated")))
        assertEq("Server says so.", BranchOpWording.outcome(update, BranchOpAnswer(200, BranchOpState.Done, status = "updated", message = "Server says so.")))
        assertEq(BranchOpWording.OWNER_ONLY, BranchOpWording.outcome(merge, BranchOpAnswer(403, BranchOpState.Failed, error = "forbidden", message = "x")))
        assertEq(
            "feature/x conflicts with main. Nothing was changed.",
            BranchOpWording.outcome(update, BranchOpAnswer(409, BranchOpState.Failed, error = "merge_conflict")),
        )
        assertEq(
            "A branch moved since the list was loaded. Refresh and try again.",
            BranchOpWording.outcome(merge, BranchOpAnswer(409, BranchOpState.Failed, error = "stale_base")),
        )
        assertEq(BranchOpWording.STILL_WORKING, BranchOpWording.outcome(merge, BranchOpAnswer(202, BranchOpState.Processing), stillWorking = true))
    }

    fun testExecutedMergeResumesWithTheSameIds() {
        val fake = Fake(
            mutableListOf(
                202 to """{"ok":true,"executed":true,"status":"processing","requestId":"r1","node":"fmnext-n1","pinBase":"main"}""",
                503 to """{"ok":false,"executed":true,"status":"failed","error":"merge_status_pending","retryable":true}""",
                202 to """{"ok":true,"executed":true,"status":"processing","requestId":"r1","node":"fmnext-n1","pinBase":"main"}""",
                200 to """{"ok":true,"executed":true,"status":"merged","branch":"feature/x","base":"main","mergeOid":"${"c".repeat(40)}","branchDeleted":false,"cleanupPending":[],"message":"Merged feature/x into main."}""",
            ),
        )
        val sleeps = ArrayList<Long>()
        val seen = ArrayList<BranchOpState>()
        val result = client(fake, sleeps).run("alice", "app", BranchOpRequest.merge(feature, main)) { seen += it.state }
        assertEq(4, fake.bodies.size)
        assertEq(listOf(2000L, 2000L, 2000L), sleeps)
        assertEq(listOf(BranchOpState.Processing, BranchOpState.Processing), seen)
        expect(!fake.bodies[0].containsKey("requestId"))
        for (index in 1..3) {
            assertEq("r1", fake.bodies[index]["requestId"])
            assertEq("fmnext-n1", fake.bodies[index]["node"])
            assertEq("main", fake.bodies[index]["pinBase"])
            assertEq("merge", fake.bodies[index]["op"])
            assertEq(mainTip, fake.bodies[index]["baseOid"])
        }
        assertEq("Merged feature/x into main.", result.outcome)
        expect(result.offersActions && !result.failed && !result.stillWorking)
        assertEq("c".repeat(40), result.answer.mergeOid)
    }

    fun testUpdateResumeCarriesThePinnedOid() {
        val fake = Fake(
            mutableListOf(
                202 to """{"ok":true,"executed":true,"status":"processing","requestId":"r9","node":"fmnext-n2","pinBase":"main","pinOid":"$mainTip"}""",
                200 to """{"ok":true,"executed":true,"status":"updated","branch":"feature/x","base":"main","message":"Updated feature/x from main."}""",
            ),
        )
        val result = client(fake).run("alice", "app", BranchOpRequest.update(feature, "main"))
        assertEq(mainTip, fake.bodies[1]["pinOid"])
        assertEq("update", fake.bodies[1]["op"])
        assertEq("Updated feature/x from main.", result.outcome)
        expect(!result.offersActions)
    }

    fun testQueuedConflictAndForbiddenStopAtOnce() {
        val queued = Fake(mutableListOf(200 to """{"ok":true,"queued":true,"queuedCount":1,"ops":[]}"""))
        val queuedResult = client(queued).run("alice", "app", BranchOpRequest.delete(feature, "main"))
        assertEq(1, queued.bodies.size)
        assertEq("Deleting feature/x is queued; it runs when the owner's desktop is online.", queuedResult.outcome)
        val conflict = Fake(mutableListOf(409 to """{"ok":false,"executed":true,"status":"failed","error":"merge_conflict","conflicts":["a.txt"],"message":"feature/x does not merge cleanly into main."}"""))
        val conflictResult = client(conflict).run("alice", "app", BranchOpRequest.merge(feature, main))
        expect(conflictResult.failed && conflictResult.answer.isConflict && !conflictResult.canResume)
        assertEq(listOf("a.txt"), conflictResult.answer.conflicts)
        assertEq("feature/x does not merge cleanly into main.", conflictResult.outcome)
        val forbidden = Fake(mutableListOf(403 to """{"error":"forbidden"}"""))
        assertEq(BranchOpWording.OWNER_ONLY, client(forbidden).run("alice", "app", BranchOpRequest.merge(feature, main)).outcome)
        val firstUnavailable = Fake(mutableListOf(503 to """{"ok":false,"error":"merge_node_unavailable"}"""))
        val unavailable = client(firstUnavailable).run("alice", "app", BranchOpRequest.merge(feature, main))
        assertEq(1, firstUnavailable.bodies.size)
        expect(unavailable.failed && !unavailable.canResume)
    }

    fun testPollingCapsAndOffersCheckAgain() {
        val fake = Fake(mutableListOf(202 to """{"ok":true,"executed":true,"status":"processing","requestId":"r1","node":"fmnext-n1","pinBase":"main"}"""))
        val sleeps = ArrayList<Long>()
        val result = client(fake, sleeps, maxPolls = 3).run("alice", "app", BranchOpRequest.merge(feature, main))
        assertEq(3, fake.bodies.size)
        assertEq(2, sleeps.size)
        expect(result.stillWorking && result.canResume)
        assertEq(BranchOpResume("r1", "fmnext-n1", "main"), result.resume)
        assertEq(BranchOpWording.STILL_WORKING, result.outcome)
        val again = Fake(mutableListOf(200 to """{"ok":true,"executed":true,"status":"merged","branch":"feature/x","base":"main"}"""))
        client(again).run("alice", "app", result.request, result.resume)
        assertEq("r1", again.bodies.single()["requestId"])
    }

    fun testUnreachableIsAFailure() {
        val result = BranchOpsClient({ _, _ -> throw java.io.IOException("offline") }, sleep = {}).run("alice", "app", BranchOpRequest.delete(feature, "main"))
        expect(result.failed)
        assertEq("Could not reach ForkMesh: offline", result.outcome)
    }

    fun testActionsCommitFilter() {
        expect(RepoActionsModel.matchesCommit("abcdef1234", null))
        expect(RepoActionsModel.matchesCommit("abcdef1234", ""))
        expect(RepoActionsModel.matchesCommit("ABCDEF1234", "abcdef1"))
        expect(RepoActionsModel.matchesCommit("abcdef1", "abcdef1234567"))
        expect(!RepoActionsModel.matchesCommit("abcdef1234", "abcdef2"))
        expect(!RepoActionsModel.matchesCommit("", "abcdef1"))
    }
}
