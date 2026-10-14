package com.forkmesh.nativeapp.chat

import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class DirectCallFacePhotoTest {
    private class FakePeer(val log: MutableList<String>) : DirectCallPeer {
        override var onIceCandidate: ((Map<String, Any?>) -> Unit)? = null
        override var onLive: (() -> Unit)? = null
        override var onClosed: (() -> Unit)? = null

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

        override fun setFace(id: String, image: String) {
            log.add("face:$id:$image")
        }

        override fun close() {}
    }

    private class Rig {
        val log = ArrayList<String>()
        val faces = ArrayList<String>()
        val pictures = ArrayList<String>()
        val engine = DirectCallEngine(
            selfId = { "me-device" },
            accountName = { "jett" },
            partnerFor = { if (it == "direct:abc") "sam" else "" },
            sendSignal = { _, _, _ -> true },
            scheduler = CallScheduler { _, _ -> {} },
        ).also { engine ->
            engine.peerFactory = DirectCallPeerFactory { _, _ -> FakePeer(log) }
            engine.onFaceChanged = { faces.add(it) }
            engine.onFaceImageChanged = { pictures.add(it) }
        }

        fun peerAccepts() {
            engine.handleFrame(
                mapOf("type" to "call-accept", "call" to engine.callId, "senderId" to "sam-device", "sender" to "sam", "to" to "me-device"),
                "direct:abc",
            )
        }
    }

    fun testYourPhotoIsTheLastFaceChoice() {
        assertEq(listOf("maya", "theo", "kofi", "ingrid", "ren", "custom"), DirectCallEngine.FACE_CHOICES.map { it.id })
        assertEq("Your photo", DirectCallEngine.faceLabel(DirectCallEngine.CUSTOM_FACE))
        assertEq("Your own face", DirectCallEngine.faceLabel(""))
    }

    fun testYourPhotoNeedsAPictureAndReachesThePeerWithIt() {
        val rig = Rig()
        expect(!rig.engine.setFace(DirectCallEngine.CUSTOM_FACE))
        assertEq("", rig.engine.face)
        expect(rig.engine.setFace(DirectCallEngine.CUSTOM_FACE, "data:image/jpeg;base64,AA"))
        assertEq("", rig.engine.effectiveVoice)
        expect(rig.engine.start("direct:abc", DirectCallKind.VIDEO))
        rig.peerAccepts()
        assertEq(listOf("face:custom:data:image/jpeg;base64,AA", "offer"), rig.log)
        expect(rig.engine.setFace("maya"))
        expect(rig.engine.setFace(DirectCallEngine.CUSTOM_FACE))
        assertEq(
            listOf("face:custom:data:image/jpeg;base64,AA", "offer", "face:maya", "face:custom:data:image/jpeg;base64,AA"),
            rig.log,
        )
        assertEq(listOf("custom", "maya", "custom"), rig.faces)
        assertEq(listOf("data:image/jpeg;base64,AA"), rig.pictures)
    }
}
