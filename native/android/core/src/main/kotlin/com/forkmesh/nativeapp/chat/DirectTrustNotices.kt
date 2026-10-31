package com.forkmesh.nativeapp.chat

import com.forkmesh.nativeapp.crypto.TrustState

enum class DirectSafetyScan(val message: String) {
    VERIFIED(DirectTrustText.VERIFIED_NOTICE),
    NOT_SAFETY_CODE("That QR code is not a ForkMesh safety number."),
    OTHER_CONVERSATION("That safety number is for a different conversation."),
    MISMATCH("The safety numbers do not match. One of you may have a device that is not approved, or someone is in the middle.");

    companion object {
        const val PREFIX = "forkmesh-safety:3:"

        fun result(scanned: String, expected: String, matched: Boolean): DirectSafetyScan {
            if (matched) return VERIFIED
            val code = scanned.trim()
            if (!code.startsWith(PREFIX)) return NOT_SAFETY_CODE
            if (scope(code) != scope(expected)) return OTHER_CONVERSATION
            return MISMATCH
        }

        fun scope(code: String): String {
            if (!code.startsWith(PREFIX)) return ""
            val rest = code.removePrefix(PREFIX)
            val end = rest.lastIndexOf(':')
            return if (end < 0) "" else rest.substring(0, end)
        }
    }
}

object DirectTrustText {
    const val OWN_DEVICE = "A new device was added to your account. Approve it if it is yours, otherwise block it."
    const val VERIFIED_NOTICE = "Safety number verified."

    fun pendingDevice(name: String): String = DirectMessageFrames.pendingDeviceLine(name)

    fun ownDevices(count: Int): String =
        if (count == 1) OWN_DEVICE else "$count new devices were added to your account. Approve them if they are yours, otherwise block them."

    fun pendingDevices(name: String, count: Int, own: Boolean): String {
        if (count == 1) return pendingDevice(name)
        val who = if (own) "" else " for $name"
        return "$count new devices$who want to join this chat. Check the safety number, then approve or block."
    }

    fun blockTitle(count: Int): String = if (count == 1) "Block this device?" else "Block these $count devices?"

    fun blockNote(count: Int): String =
        if (count == 1) "Nothing is sent to it and its messages are dropped from now on."
        else "Nothing is sent to them and their messages are dropped from now on."

    fun safetyExplainer(name: String): String =
        "Compare these numbers with $name, or scan the code on their phone. If they match, nobody is in the middle of this chat."
}

data class DirectTrustNotice(val id: String, val conversation: String, val kids: List<String>, val own: Boolean, val text: String, val timestamp: Long) {
    val asksApproval: Boolean get() = kids.isNotEmpty()
}

object DirectTrustNotices {
    fun pending(conversation: String, devices: List<DirectDevice>, name: String): List<DirectTrustNotice> {
        val waiting = devices.filter { it.state == TrustState.PENDING }.sortedBy { it.firstSeen }
        val first = waiting.firstOrNull() ?: return emptyList()
        val kids = waiting.map { it.kid }
        val ownOnly = waiting.all { it.ownAccount }
        val mixed = !ownOnly && waiting.any { it.ownAccount }
        return listOf(
            DirectTrustNotice(
                id = "trust-$conversation-devices",
                conversation = conversation,
                kids = kids,
                own = ownOnly,
                text = if (ownOnly) DirectTrustText.ownDevices(kids.size) else DirectTrustText.pendingDevices(name, kids.size, mixed),
                timestamp = first.firstSeen,
            ),
        )
    }
}
