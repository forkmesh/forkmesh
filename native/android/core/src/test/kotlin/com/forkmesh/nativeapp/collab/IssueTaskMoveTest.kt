package com.forkmesh.nativeapp.collab

import com.forkmesh.nativeapp.api.ApiJsonResult
import com.forkmesh.nativeapp.model.Issue
import com.forkmesh.nativeapp.model.OrgApiException
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.assertFails
import com.forkmesh.nativeapp.testing.expect

class IssueTaskMoveTest {
    private val issue = Issue(number = 7, title = "Mirror roster goes stale", body = "Steps to reproduce")

    fun testTaskBodyCarriesTheIssueAndItsSource() {
        val body = IssueTaskMove.taskBody("alice", "demo", issue, "mona")
        assertEq("Mirror roster goes stale", body["title"])
        assertEq("Steps to reproduce\n\nMoved from issue alice/demo#7 by mona.", body["details"])
        assertEq("alice/demo", body["repository"])
        assertEq("unassigned", body["assigneeKind"])
        val long = Issue(number = 1, title = "t".repeat(300), body = "b".repeat(9000))
        val fitted = IssueTaskMove.taskBody("a b", "demo", long, "")
        assertEq(IssueTaskMove.MAX_TITLE, (fitted["title"] as String).length)
        assertEq(IssueTaskMove.MAX_DETAILS, (fitted["details"] as String).length)
        assertEq("", fitted["repository"])
        expect((fitted["details"] as String).endsWith("Moved from issue demo#1 by a repository manager."))
    }

    fun testMoveCreatesTheTaskThenDeletesTheIssue() {
        val rec = RecordingCall()
        val writer = CollabRecordWriter(rec.call, CollabSigner(EchoIdentity(), now = { 5L }), "alice", "demo")
        val calls = ArrayList<String>()
        val task = IssueTaskMove.move(issue, writer, "mona") { method, suffix, _ ->
            calls.add("$method $suffix")
            expect(rec.calls.isEmpty())
            mapOf("ok" to true, "task" to mapOf("id" to "t1"))
        }
        assertEq("t1", task["id"])
        assertEq(listOf("POST "), calls)
        assertEq(listOf("POST /api/repo/alice/demo/issues"), rec.calls.map { it.first })
        @Suppress("UNCHECKED_CAST")
        val event = rec.calls[0].second!!["event"] as Map<String, Any?>
        assertEq("delete", event["type"])
        assertEq("self", event["target"])
    }

    fun testAFailedTaskKeepsTheIssue() {
        val rec = RecordingCall()
        val writer = CollabRecordWriter(rec.call, CollabSigner(EchoIdentity(), now = { 5L }), "alice", "demo")
        val failure = assertFails<IssueTaskMoveException> {
            IssueTaskMove.move(issue, writer, "mona") { _, _, _ -> throw OrgApiException("org_member_required", "org member required") }
        }
        expect(!failure.taskCreated)
        expect(rec.calls.isEmpty())
    }

    fun testAFailedDeleteTrashesTheNewTask() {
        val rec = RecordingCall()
        rec.answer = ApiJsonResult(403, mapOf("error" to "not_authorized"))
        val writer = CollabRecordWriter(rec.call, CollabSigner(EchoIdentity(), now = { 5L }), "alice", "demo")
        val calls = ArrayList<String>()
        val failure = assertFails<IssueTaskMoveException> {
            IssueTaskMove.move(issue, writer, "mona") { method, suffix, _ ->
                calls.add("$method $suffix")
                mapOf("ok" to true, "task" to mapOf("id" to "t1"))
            }
        }
        expect(failure.taskCreated && failure.taskTrashed)
        assertEq(listOf("POST ", "DELETE /t1"), calls)
    }
}
