package com.forkmesh.nativeapp.control

import com.forkmesh.nativeapp.agents.AgentLaunchSession
import com.forkmesh.nativeapp.agents.AttachmentOrigin
import com.forkmesh.nativeapp.agents.PromptAttachment
import com.forkmesh.nativeapp.model.AgentSession
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class DesktopSessionAttachmentsTest {
    private val desk = UnifiedAgentRow.fromDesktop(
        LocalAgentRow("alice", "App", AgentSession(id = 7, status = "completed")),
        ControlTransport.Relay,
        "desk",
    )

    private class FakeControl(override val rows: List<UnifiedAgentRow>) : AgentControlling {
        val asked = mutableListOf<UnifiedAgentRow>()
        override val promptNotice: String = ""

        override fun refresh() {}

        override fun stop(row: UnifiedAgentRow) {}

        override fun prompt(row: UnifiedAgentRow, text: String, images: List<PromptAttachment>) {}

        override fun continueRun(row: UnifiedAgentRow) {}

        override fun transcript(row: UnifiedAgentRow, since: Int): AgentTranscriptDelta {
            asked += row
            val json = mapOf<String, Any?>(
                "transcript" to "Attached image: /home/me/shot.png",
                "next" to 34,
                "attachments" to listOf(mapOf("id" to "a".repeat(32), "name" to "shot.png", "mediaType" to "image/png", "size" to 120)),
            )
            return AgentTranscriptDelta.fromDesktop(json, since, row.localId)
        }

        override fun startOnDesktop(owner: String, name: String, task: String, provider: String, images: List<PromptAttachment>): Int = 0
    }

    fun testTheLiveDesktopRowMatchesRepoAndSession() {
        val outside = desk.copy(id = "outside:7", outside = true)
        val node = UnifiedAgentRow.fromCloud(AgentLaunchSession(sessionId = "s7", owner = "alice", repo = "app", localAgentId = 7))
        assertEq(desk, liveDesktopRow(listOf(outside, node, desk), "app", 7))
        expect(liveDesktopRow(listOf(outside, node, desk), "app", 8) == null)
        expect(liveDesktopRow(listOf(outside, node, desk), "site", 7) == null)
    }

    fun testADesktopSessionListsItsPicturesFromTheDesktop() {
        val control = FakeControl(listOf(desk))
        val refs = control.desktopAttachments("app", 7) ?: error("no list")
        assertEq(listOf("shot.png"), refs.map { it.name })
        assertEq(AttachmentOrigin.Desktop(7), refs[0].origin)
        assertEq(listOf(desk), control.asked)
    }

    fun testASessionTheDesktopDoesNotListAsksNothing() {
        val control = FakeControl(emptyList())
        expect(control.desktopAttachments("app", 7) == null)
        expect(control.asked.isEmpty())
    }
}
