package com.forkmesh.nativeapp.chat

import com.forkmesh.nativeapp.crypto.TrustState
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

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

    fun testEveryWaitingDeviceSharesOneBanner() {
        val devices = listOf(
            DirectDevice("k2", "pat", TrustState.PENDING, verified = false, firstSeen = 20),
            DirectDevice("k1", "me", TrustState.PENDING, verified = false, firstSeen = 10, ownAccount = true),
            DirectDevice("k3", "pat", TrustState.APPROVED, verified = false, firstSeen = 5),
        )
        val notices = DirectTrustNotices.pending("direct:x", devices, "pat")
        assertEq(1, notices.size, "every waiting device shares one banner")
        assertEq(listOf("k1", "k2"), notices[0].kids)
        assertEq(10L, notices[0].timestamp)
        assertEq("trust-direct:x-devices", notices[0].id)
        assertEq("2 new devices want to join this chat. Check the safety number, then approve or block.", notices[0].text)
        expect(notices[0].asksApproval)

        val mine = DirectTrustNotices.pending("direct:x", devices.filter { it.kid != "k2" }, "pat")
        assertEq(listOf("k1"), mine.single().kids)
        assertEq(true, mine.single().own)
        assertEq(DirectTrustText.OWN_DEVICE, mine.single().text)
        val theirs = DirectTrustNotices.pending("direct:x", devices.filter { it.kid != "k1" }, "pat")
        assertEq(DirectMessageFrames.pendingDeviceLine("pat"), theirs.single().text)
        val several = DirectTrustNotices.pending(
            "direct:x",
            devices.filter { it.kid != "k1" } + DirectDevice("k4", "pat", TrustState.PENDING, verified = false, firstSeen = 90),
            "pat",
        )
        assertEq(listOf("k2", "k4"), several.single().kids)
        assertEq("2 new devices for pat want to join this chat. Check the safety number, then approve or block.", several.single().text)
        expect(DirectTrustText.ownDevices(3).startsWith("3 new devices "))
        assertEq("Block this device?", DirectTrustText.blockTitle(1))
        assertEq("Block these 2 devices?", DirectTrustText.blockTitle(2))
        assertEq(emptyList<DirectTrustNotice>(), DirectTrustNotices.pending("direct:x", devices.filter { it.state != TrustState.PENDING }, "pat"))
    }
}
