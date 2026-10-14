package com.forkmesh.nativeapp.chat

import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class DirectCallBackgroundsTest {
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

        override fun setBackground(id: String, image: String) {
            log.add(if (image.isEmpty()) "background:$id" else "background:$id:$image")
        }

        override fun close() {}
    }

    private class Rig {
        val log = ArrayList<String>()
        val saved = ArrayList<String>()
        val engine = DirectCallEngine(
            selfId = { "me-device" },
            accountName = { "jett" },
            partnerFor = { if (it == "direct:abc") "sam" else "" },
            sendSignal = { _, _, _ -> true },
            scheduler = CallScheduler { _, _ -> {} },
        ).also { engine ->
            engine.peerFactory = DirectCallPeerFactory { _, _ -> FakePeer(log) }
            engine.onBackgroundChanged = { id, image -> saved.add(if (image.isEmpty()) id else "$id:$image") }
        }

        fun peerAccepts() {
            engine.handleFrame(
                mapOf("type" to "call-accept", "call" to engine.callId, "senderId" to "sam-device", "sender" to "sam", "to" to "me-device"),
                "direct:abc",
            )
        }
    }

    fun testTheBackgroundsAreListedInOrder() {
        assertEq(
            listOf("", "blur", "blur-strong", "world", "office", "meeting", "library", "skyline", "custom"),
            DirectCallEngine.BACKGROUNDS.map { it.id },
        )
        assertEq("ForkMesh World", DirectCallEngine.backgroundLabel("world"))
    }

    fun testUnknownBackgroundsAndAnEmptyCustomPhotoAreRejected() {
        val rig = Rig()
        expect(!rig.engine.setBackground("nowhere"))
        expect(!rig.engine.setBackground(DirectCallEngine.CUSTOM_BACKGROUND))
        assertEq("", rig.engine.background)
        assertEq(emptyList<String>(), rig.saved)
    }

    fun testTheBackgroundReachesAVideoPeerBeforeItOffersAndChangesMidCall() {
        val rig = Rig()
        expect(rig.engine.setBackground("world"))
        expect(rig.engine.start("direct:abc", DirectCallKind.VIDEO))
        rig.peerAccepts()
        assertEq(listOf("background:world", "offer"), rig.log)
        expect(rig.engine.setBackground(DirectCallEngine.CUSTOM_BACKGROUND, "data:image/jpeg;base64,AA"))
        expect(rig.engine.setBackground("blur"))
        expect(rig.engine.setBackground(DirectCallEngine.CUSTOM_BACKGROUND))
        assertEq(
            listOf(
                "background:world", "offer", "background:custom:data:image/jpeg;base64,AA", "background:blur",
                "background:custom:data:image/jpeg;base64,AA",
            ),
            rig.log,
        )
        assertEq(
            listOf("world", "custom:data:image/jpeg;base64,AA", "blur:data:image/jpeg;base64,AA", "custom:data:image/jpeg;base64,AA"),
            rig.saved,
        )
    }

    fun testAudioCallsNeverSendABackground() {
        val rig = Rig()
        expect(rig.engine.setBackground("office"))
        expect(rig.engine.start("direct:abc", DirectCallKind.AUDIO))
        rig.peerAccepts()
        assertEq(listOf("offer"), rig.log)
    }
}
