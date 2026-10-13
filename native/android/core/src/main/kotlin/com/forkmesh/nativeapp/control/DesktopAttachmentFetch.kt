package com.forkmesh.nativeapp.control

import com.forkmesh.nativeapp.agents.AgentAttachmentError

data class DesktopAttachmentBytes(val data: ByteArray, val preview: Boolean)

fun DesktopControlHub.attachment(sessionId: Int, attachmentId: String): DesktopAttachmentBytes = try {
    val link = link()
    val relayed = link.transport != ControlTransport.Lan
    val params = linkedMapOf<String, Any?>("id" to sessionId, "attachment" to attachmentId)
    if (relayed) params["preview"] = true
    val answer = link.request("agents.attachment", params, if (relayed) 60_000 else 30_000)
    DesktopAttachmentBytes(LocalAgentTranscript.attachmentBytes(answer), answer["preview"] == true)
} catch (error: LocalControlException) {
    throw AgentAttachmentError(LocalAgentTranscript.attachmentErrorText(error.code))
}
