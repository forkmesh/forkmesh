package com.forkmesh.nativeapp.collab

import com.forkmesh.nativeapp.control.DesktopControlFamilyName
import com.forkmesh.nativeapp.control.LocalControlException
import com.forkmesh.nativeapp.model.Repository
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.assertFails
import com.forkmesh.nativeapp.testing.expect

class PullMergeAllTest {
    fun testMergeAllIsTokenConfirmed() {
        val calls = ArrayList<Pair<String, Map<String, Any?>>>()
        val remote = PullMergeAllRemote { suffix, params, _ ->
            calls.add(suffix to params)
            when (suffix) {
                "prepare" -> mapOf("token" to "t1", "summary" to "Start an AI agent", "expiresInMs" to 60000L)
                else -> mapOf(
                    "message" to "Agent #9 is reviewing and merging 2 pull request(s).",
                    "sessionId" to 9L,
                    "pulls" to 2L,
                    "skipped" to listOf("#5 (its code is not on this node)"),
                )
            }
        }
        val confirmation = remote.prepare("alice", "tool")
        assertEq("Start an AI agent", confirmation.summary)
        val result = remote.run("alice", "tool", confirmation.token)
        assertEq(listOf("prepare", "run"), calls.map { it.first })
        assertEq(mapOf<String, Any?>("owner" to "alice", "name" to "tool", "action" to "merge-all-ai"), calls[0].second)
        assertEq(mapOf<String, Any?>("owner" to "alice", "name" to "tool", "action" to "merge-all-ai", "token" to "t1"), calls[1].second)
        assertEq(9, result.sessionId)
        assertEq(2, result.pulls)
        assertEq(listOf("#5 (its code is not on this node)"), result.skipped)
    }

    fun testRefusalsReadAsSentences() {
        val empty = PullMergeAllRemote { _, _, _ -> emptyMap() }
        assertEq("confirmation_missing", assertFails<LocalControlException> { empty.prepare("a", "b") }.code)
        assertEq("The confirmation expired. Try again.", PullMergeAllRemote.message(LocalControlException("expired")))
        expect(PullMergeAllRemote.message(LocalControlException("unknown_method")).contains("Update ForkMesh"))
        assertEq("An agent is already merging this repository's pull requests.", PullMergeAllRules.errorMessage("busy"))
        assertEq(DesktopControlFamilyName.Pulls, DesktopControlFamilyName.owning("pulls.prepare"))
    }

    fun testMirrorRowAsksForThePublicOwner() {
        assertEq("forkmesh", PullMergeAllRules.owner(Repository("fmnext-n1", "forkmesh", publicOwner = "forkmesh")))
        assertEq("alice", PullMergeAllRules.owner(Repository("alice", "tool")))
    }
}
