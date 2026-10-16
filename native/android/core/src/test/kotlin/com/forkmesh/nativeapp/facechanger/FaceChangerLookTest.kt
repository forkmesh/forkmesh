package com.forkmesh.nativeapp.facechanger

import com.forkmesh.nativeapp.chat.CallScheduler
import com.forkmesh.nativeapp.chat.DirectCallEngine
import com.forkmesh.nativeapp.chat.DirectCallKind
import com.forkmesh.nativeapp.chat.DirectCallPeer
import com.forkmesh.nativeapp.chat.DirectCallPeerFactory
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class FaceChangerLookTest {
    private class FakePeer(val log: MutableList<String>) : DirectCallPeer {
        override var onIceCandidate: ((Map<String, Any?>) -> Unit)? = null
        override var onLive: (() -> Unit)? = null
        override var onClosed: (() -> Unit)? = null

        override fun offer(): Map<String, Any?> = mapOf("type" to "offer", "sdp" to "o")

        override fun answer(offer: Map<String, Any?>): Map<String, Any?> = mapOf("type" to "answer", "sdp" to "a")

        override fun acceptAnswer(answer: Map<String, Any?>) {}

        override fun add(candidate: Map<String, Any?>) {}

        override fun setMuted(muted: Boolean) {}

        override fun setCameraOff(off: Boolean) {}

        override fun setFace(id: String) {
            log.add("face:$id")
        }

        override fun setBackground(id: String, image: String) {
            log.add("background:$id")
        }

        override fun setVoice(id: String) {
            log.add("voice:$id")
        }

        override fun close() {}
    }

    fun testTheLookTellsEveryListenerWhatChanged() {
        val look = FaceChangerLook()
        val parts = ArrayList<FaceChangerPart>()
        look.addListener { parts.add(it) }
        expect(look.setFace("theo"))
        expect(look.setBackground("blur"))
        expect(look.setVoiceChoice("robot"))
        assertEq(listOf(FaceChangerPart.FACE, FaceChangerPart.BACKGROUND, FaceChangerPart.VOICE), parts)
        expect(look.setBackground("blur"))
        expect(look.setVoiceChoice("robot"))
        assertEq(3, parts.size)
        expect(!look.setFace("nobody"))
        assertEq(3, parts.size)
    }

    fun testTheVoiceFollowsTheFaceUntilOneIsChosen() {
        val look = FaceChangerLook()
        assertEq("", look.effectiveVoice)
        look.setFace("kofi")
        assertEq("kofi", look.effectiveVoice)
        look.setVoiceChoice(FaceChangerLook.VOICE_OFF)
        assertEq("", look.effectiveVoice)
        look.setVoiceChoice("teen")
        assertEq("teen", look.effectiveVoice)
    }

    fun testOnlyTheChosenPhotoIsSentAsTheCustomPicture() {
        val look = FaceChangerLook()
        expect(!look.setFace(FaceChangerLook.CUSTOM_FACE))
        expect(look.setFace(FaceChangerLook.CUSTOM_FACE, "data:image/jpeg;base64,AA=="))
        assertEq("data:image/jpeg;base64,AA==", look.customFaceImage)
        look.setFace("ren")
        assertEq("", look.customFaceImage)
        assertEq("data:image/jpeg;base64,AA==", look.faceImage)
    }

    fun testAChoiceMadeInTheFaceChangerReachesALiveVideoCall() {
        val log = ArrayList<String>()
        val look = FaceChangerLook()
        val engine = DirectCallEngine(
            selfId = { "me-device" },
            accountName = { "me" },
            partnerFor = { if (it == "direct:abc") "sam" else "" },
            sendSignal = { _, _, _ -> true },
            scheduler = CallScheduler { _, _ -> {} },
            look = look,
        )
        engine.peerFactory = DirectCallPeerFactory { _, _ -> FakePeer(log) }
        expect(engine.start("direct:abc", DirectCallKind.VIDEO))
        engine.handleFrame(
            mapOf("type" to "call-accept", "call" to engine.callId, "senderId" to "sam-device", "sender" to "sam", "to" to "me-device"),
            "direct:abc",
        )
        log.clear()
        look.setFace("ingrid")
        look.setBackground("office")
        expect("face:ingrid" in log)
        expect("voice:ingrid" in log)
        expect("background:office" in log)
        assertEq("ingrid", engine.face)
        engine.detach()
        log.clear()
        look.setFace("maya")
        assertEq(emptyList<String>(), log)
    }
}
