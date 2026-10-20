package com.forkmesh.nativeapp.chat

import com.forkmesh.nativeapp.crypto.TrustState
import com.forkmesh.nativeapp.testing.assertEq

class DirectTrustNoticesTest {
    private val expected = "forkmesh-safety:3:direct:${"d".repeat(32)}:abc"

    fun testAScanTellsTheThreeFailuresApart() {
        assertEq(DirectSafetyScan.VERIFIED, DirectSafetyScan.result(expected, expected, matched = true))
        assertEq(DirectSafetyScan.NOT_SAFETY_CODE, DirectSafetyScan.result("https://example.com", expected, matched = false))
        assertEq(DirectSafetyScan.OTHER_CONVERSATION, DirectSafetyScan.result("forkmesh-safety:3:direct:${"e".repeat(32)}:abc", expected, matched = false))
        assertEq(DirectSafetyScan.MISMATCH, DirectSafetyScan.result(" forkmesh-safety:3:direct:${"d".repeat(32)}:xyz ", expected, matched = false))
        assertEq("Safety number verified.", DirectSafetyScan.VERIFIED.message)
        assertEq("That QR code is not a ForkMesh safety number.", DirectSafetyScan.NOT_SAFETY_CODE.message)
        assertEq("That safety number is for a different conversation.", DirectSafetyScan.OTHER_CONVERSATION.message)
    }

    fun testANewDeviceOnYourOwnAccountSaysSo() {
        val devices = listOf(
            DirectDevice("k2", "pat", TrustState.PENDING, verified = false, firstSeen = 20),
            DirectDevice("k1", "me", TrustState.PENDING, verified = false, firstSeen = 10, ownAccount = true),
            DirectDevice("k3", "pat", TrustState.APPROVED, verified = false, firstSeen = 5),
        )
        val notices = DirectTrustNotices.pending("direct:x", devices, "pat")
        assertEq(listOf("k1", "k2"), notices.map { it.kid })
        assertEq(DirectTrustText.OWN_DEVICE, notices[0].text)
        assertEq(DirectMessageFrames.pendingDeviceLine("pat"), notices[1].text)
        assertEq("trust-direct:x-k2", notices[1].id)
    }
}
