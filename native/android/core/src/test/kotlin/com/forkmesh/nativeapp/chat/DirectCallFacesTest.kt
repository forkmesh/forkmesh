package com.forkmesh.nativeapp.chat

import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class DirectCallFacesTest {
    private class FakePeer(val log: MutableList<String>) : DirectCallPeer {
        override var onIceCandidate: ((Map<String, Any?>) -> Unit)? = null
        override var onLive: (() -> Unit)? = null
        override var onClosed: (() -> Unit)? = null

        override fun offer(): Map<String, Any?> {
            log.add("offer")
            return mapOf("type" to "offer", "sdp" to "o")
        }

        override fun answer(offer: Map<String, Any?>): Map<String, Any?> {
            log.add("answer")
            return mapOf("type" to "answer", "sdp" to "a")
        }

        override fun acceptAnswer(answer: Map<String, Any?>) {
            log.add("accept")
        }

        override fun add(candidate: Map<String, Any?>) {
            log.add("ice")
        }

        override fun setMuted(muted: Boolean) {
            log.add("mute:$muted")
        }

        override fun setCameraOff(off: Boolean) {
            log.add("camera:$off")
        }

        override fun setFace(id: String) {
            log.add("face:$id")
        }

        override fun close() {
            log.add("close")
        }
    }

    private class Rig {
        val log = ArrayList<String>()
        val sent = ArrayList<Pair<String, Map<String, Any?>>>()
        val saved = ArrayList<String>()
        val engine = DirectCallEngine(
            selfId = { "me-device" },
            accountName = { "jett" },
            partnerFor = { if (it == "direct:abc") "sam" else "" },
            sendSignal = { _, type, extra ->
                sent.add(type to extra)
                true
            },
            scheduler = CallScheduler { _, _ -> {} },
        ).also { engine ->
            engine.peerFactory = DirectCallPeerFactory { _, _ -> FakePeer(log) }
            engine.onFaceChanged = { saved.add(it) }
        }

        fun peerAccepts() {
            engine.handleFrame(
                mapOf("type" to "call-accept", "call" to engine.callId, "senderId" to "sam-device", "sender" to "sam", "to" to "me-device"),
                "direct:abc",
            )
        }
    }

    fun testTheFiveFacesAreListedInOrder() {
        assertEq(listOf("maya", "theo", "kofi", "ingrid", "ren"), DirectCallEngine.FACES.map { it.id })
        assertEq(listOf("Maya", "Theo", "Kofi", "Ingrid", "Ren"), DirectCallEngine.FACES.map { it.label })
    }

    fun testAnUnknownFaceIsRejected() {
        val rig = Rig()
        expect(rig.engine.setFace("kofi"))
        expect(!rig.engine.setFace("gorilla"))
        assertEq("kofi", rig.engine.face)
        assertEq(listOf("kofi"), rig.saved)
        expect(rig.engine.setFace(""))
        assertEq("", rig.engine.face)
        assertEq(listOf("kofi", ""), rig.saved)
    }

    fun testAVideoCallerAppliesTheFaceBeforeTheOffer() {
        val rig = Rig()
        rig.engine.setFace("ingrid")
        expect(rig.engine.start("direct:abc", DirectCallKind.VIDEO))
        rig.peerAccepts()
        assertEq(listOf("face:ingrid", "offer"), rig.log)
        assertEq("call-offer", rig.sent.last().first)
    }

    fun testAVideoCalleeAppliesTheFaceBeforeTheAnswer() {
        val rig = Rig()
        rig.engine.setFace("ren")
        rig.engine.handleFrame(
            mapOf("type" to "call-invite", "call" to "c1", "senderId" to "sam-device", "sender" to "sam", "media" to "video"),
            "direct:abc",
        )
        expect(rig.engine.invite != null)
        rig.engine.acceptInvite()
        rig.engine.handleFrame(
            mapOf(
                "type" to "call-offer", "call" to "c1", "senderId" to "sam-device", "sender" to "sam", "to" to "me-device",
                "sdp" to mapOf("type" to "offer", "sdp" to "o"),
            ),
            "direct:abc",
        )
        assertEq(listOf("face:ren", "answer"), rig.log)
        assertEq("call-answer", rig.sent.last().first)
    }

    fun testAVoiceCallNeverAppliesAFace() {
        val rig = Rig()
        rig.engine.setFace("maya")
        expect(rig.engine.start("direct:abc", DirectCallKind.AUDIO))
        rig.peerAccepts()
        assertEq(listOf("offer"), rig.log)
        rig.engine.setFace("theo")
        assertEq(listOf("offer"), rig.log)
        assertEq("theo", rig.engine.face)
    }

    fun testOwnFaceIsNotSentToANewPeer() {
        val rig = Rig()
        expect(rig.engine.start("direct:abc", DirectCallKind.VIDEO))
        rig.peerAccepts()
        assertEq(listOf("offer"), rig.log)
    }

    fun testChangingTheFaceMidCallReachesThePeerAndOutlivesTheCall() {
        val rig = Rig()
        expect(rig.engine.start("direct:abc", DirectCallKind.VIDEO))
        rig.peerAccepts()
        expect(rig.engine.setFace("theo"))
        expect(rig.engine.setFace(""))
        assertEq(listOf("offer", "face:theo", "face:"), rig.log)
        rig.engine.setFace("kofi")
        rig.engine.hangUp()
        assertEq(null, rig.engine.phase)
        assertEq("kofi", rig.engine.face)
    }
}
