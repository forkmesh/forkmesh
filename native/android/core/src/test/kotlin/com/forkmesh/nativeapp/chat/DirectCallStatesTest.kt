package com.forkmesh.nativeapp.chat

import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class DirectCallStatesTest {
    private class FakePeer : DirectCallPeer {
        override var onIceCandidate: ((Map<String, Any?>) -> Unit)? = null
        override var onLive: (() -> Unit)? = null
        override var onClosed: (() -> Unit)? = null

        override fun offer(): Map<String, Any?> = mapOf("type" to "offer", "sdp" to "o")

        override fun answer(offer: Map<String, Any?>): Map<String, Any?> = mapOf("type" to "answer", "sdp" to "a")

        override fun acceptAnswer(answer: Map<String, Any?>) {}

        override fun add(candidate: Map<String, Any?>) {}

        override fun setMuted(muted: Boolean) {}

        override fun setCameraOff(off: Boolean) {}

        override fun close() {}
    }

    private class Rig {
        val sent = ArrayList<String>()
        var peer: FakePeer? = null
        val engine = DirectCallEngine(
            selfId = { "me-device" },
            accountName = { "jett" },
            partnerFor = { if (it == "direct:abc") "sam" else "" },
            sendSignal = { _, type, _ ->
                sent.add(type)
                true
            },
            scheduler = CallScheduler { _, _ -> {} },
        ).also { engine ->
            engine.peerFactory = DirectCallPeerFactory { _, _ -> FakePeer().also { peer = it } }
        }

        fun frame(type: String, extra: Map<String, Any?> = emptyMap()) {
            engine.handleFrame(
                mapOf("type" to type, "call" to engine.callId, "senderId" to "sam-device", "sender" to "sam", "to" to "me-device") + extra,
                "direct:abc",
            )
        }
    }

    fun testTheStatusLineFollowsTheCallFromRingingToLive() {
        val rig = Rig()
        assertEq("", rig.engine.stateLine)
        expect(rig.engine.start("direct:abc", DirectCallKind.VIDEO))
        assertEq(DirectCallPhase.RINGING, rig.engine.phase)
        assertEq("Calling @sam…", rig.engine.stateLine)
        rig.frame("call-accept")
        assertEq(DirectCallPhase.CONNECTING, rig.engine.phase)
        assertEq("Connecting…", rig.engine.stateLine)
        rig.peer?.onLive?.invoke()
        assertEq(DirectCallPhase.LIVE, rig.engine.phase)
        assertEq("Connected · end-to-end encrypted", rig.engine.stateLine)
        rig.engine.hangUp()
        assertEq(null, rig.engine.phase)
        assertEq("", rig.engine.stateLine)
        assertEq(listOf("call-invite", "call-offer", "call-bye"), rig.sent)
    }

    fun testHangingUpBeforeAnAnswerLeavesAMissedCall() {
        val rig = Rig()
        val missed = ArrayList<Pair<String, DirectCallKind>>()
        rig.engine.onMissed = { conversation, media -> missed.add(conversation to media) }
        expect(rig.engine.start("direct:abc", DirectCallKind.VIDEO))
        rig.engine.hangUp()
        assertEq(null, rig.engine.phase)
        assertEq(listOf("call-invite", "call-bye"), rig.sent)
        assertEq(listOf("direct:abc" to DirectCallKind.VIDEO), missed)
        expect(rig.engine.start("direct:abc", DirectCallKind.AUDIO))
        rig.frame("call-accept")
        rig.engine.hangUp()
        assertEq(null, rig.engine.phase)
        assertEq(1, missed.size)
    }

    fun testADeclineEndsTheRingingAtOnce() {
        val rig = Rig()
        expect(rig.engine.start("direct:abc", DirectCallKind.AUDIO))
        assertEq(DirectCallPhase.RINGING, rig.engine.phase)
        rig.frame("call-decline", mapOf("reason" to "declined"))
        assertEq(null, rig.engine.phase)
        assertEq("", rig.engine.stateLine)
    }
}
