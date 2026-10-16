package com.forkmesh.nativeapp.chat

import com.forkmesh.nativeapp.facechanger.FaceChangerLook
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class DirectCallVoicesTest {
    private class FakePeer(val log: MutableList<String>) : DirectCallPeer {
        override var onIceCandidate: ((Map<String, Any?>) -> Unit)? = null
        override var onLive: (() -> Unit)? = null
        override var onClosed: (() -> Unit)? = null
        var observer: ((String) -> Unit)? = null

        override fun offer(): Map<String, Any?> {
            log.add("offer")
            return mapOf("type" to "offer", "sdp" to "o")
        }

        override fun answer(offer: Map<String, Any?>): Map<String, Any?> = mapOf("type" to "answer", "sdp" to "a")

        override fun acceptAnswer(answer: Map<String, Any?>) {}

        override fun add(candidate: Map<String, Any?>) {}

        override fun setMuted(muted: Boolean) {}

        override fun setCameraOff(off: Boolean) {}

        override fun setFace(id: String) {
            log.add("face:$id")
        }

        override fun setVoice(id: String) {
            log.add("voice:$id")
        }

        override fun observeVoice(handler: (String) -> Unit) {
            observer = handler
        }

        override fun close() {}
    }

    private class Rig {
        val log = ArrayList<String>()
        val saved = ArrayList<String>()
        var peer: FakePeer? = null
        val engine = DirectCallEngine(
            selfId = { "me-device" },
            accountName = { "jett" },
            partnerFor = { if (it == "direct:abc") "sam" else "" },
            sendSignal = { _, _, _ -> true },
            scheduler = CallScheduler { _, _ -> {} },
        ).also { engine ->
            engine.peerFactory = DirectCallPeerFactory { _, _ -> FakePeer(log).also { peer = it } }
            engine.look.onVoiceChanged = { saved.add(it) }
        }

        fun peerAccepts() {
            engine.handleFrame(
                mapOf("type" to "call-accept", "call" to engine.callId, "senderId" to "sam-device", "sender" to "sam", "to" to "me-device"),
                "direct:abc",
            )
        }
    }

    fun testTheVoiceMatchesTheFaceByDefaultEvenInAVoiceCall() {
        val rig = Rig()
        assertEq(FaceChangerLook.VOICE_MATCH_FACE, rig.engine.voiceChoice)
        rig.engine.setFace("kofi")
        expect(rig.engine.start("direct:abc", DirectCallKind.AUDIO))
        rig.peerAccepts()
        assertEq(listOf("voice:kofi", "offer"), rig.log)
        assertEq("kofi", rig.engine.voice)
    }

    fun testChoosingAVoiceOverridesTheFaceAndOffIsNatural() {
        val rig = Rig()
        expect(!rig.engine.setVoiceChoice("nobody"))
        expect(rig.engine.start("direct:abc", DirectCallKind.VIDEO))
        rig.peerAccepts()
        assertEq(listOf("offer"), rig.log)
        rig.engine.setFace("maya")
        expect(rig.engine.setVoiceChoice("theo"))
        rig.engine.setFace("ren")
        expect(rig.engine.setVoiceChoice(FaceChangerLook.VOICE_OFF))
        assertEq(listOf("offer", "face:maya", "voice:maya", "voice:theo", "face:ren", "voice:"), rig.log)
        assertEq(listOf("theo", "off"), rig.saved)
        rig.peer?.observer?.invoke("")
        assertEq("", rig.engine.voice)
        rig.engine.hangUp()
        assertEq("", rig.engine.voice)
        assertEq("off", rig.engine.voiceChoice)
        assertEq("Ren", FaceChangerLook.voiceLabel("ren"))
        assertEq("Your own", FaceChangerLook.voiceLabel(""))
    }

    fun testVoicesAreTheFaceVoicesPlusTheExtraVoices() {
        assertEq(
            listOf(
                "maya", "theo", "kofi", "ingrid", "ren",
                "deep", "older-man", "young-man", "warm", "low", "older-woman", "teen", "neutral", "robot",
            ),
            FaceChangerLook.VOICES.map { it.id },
        )
        assertEq("Older woman", FaceChangerLook.voiceLabel("older-woman"))
        val rig = Rig()
        expect(rig.engine.setVoiceChoice("robot"))
        assertEq("robot", rig.engine.effectiveVoice)
    }
}
