package com.forkmesh.nativeapp.chat

import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class DirectCallResilienceTest {
    private class Clock : CallScheduler {
        private class Job(val at: Long, val order: Int, val run: () -> Unit) {
            var cancelled = false
        }

        var now = 0L
        private var order = 0
        private val jobs = ArrayList<Job>()

        override fun schedule(delayMs: Long, run: () -> Unit): () -> Unit {
            val job = Job(now + delayMs, order++, run)
            jobs.add(job)
            return { job.cancelled = true }
        }

        fun advance(ms: Long) {
            val until = now + ms
            while (true) {
                val next = jobs.filter { !it.cancelled && it.at <= until }.minWithOrNull(compareBy({ it.at }, { it.order })) ?: break
                jobs.remove(next)
                now = next.at
                next.run()
            }
            now = until
        }
    }

    private class Peer(private val name: String) : DirectCallPeer {
        override var onIceCandidate: ((Map<String, Any?>) -> Unit)? = null
        override var onLive: (() -> Unit)? = null
        override var onClosed: (() -> Unit)? = null
        override var onConnection: ((DirectCallConnection) -> Unit)? = null
        var restarts = 0
        var answers = 0
        val added = ArrayList<Map<String, Any?>>()
        var closed = false

        override fun offer(): Map<String, Any?> = mapOf("type" to "offer", "sdp" to "$name-offer")

        override fun restartOffer(): Map<String, Any?> {
            restarts += 1
            return mapOf("type" to "offer", "sdp" to "$name-restart-$restarts")
        }

        override fun answer(offer: Map<String, Any?>): Map<String, Any?> {
            answers += 1
            return mapOf("type" to "answer", "sdp" to "$name-answer-$answers")
        }

        override fun acceptAnswer(answer: Map<String, Any?>) {}

        override fun add(candidate: Map<String, Any?>) {
            added.add(candidate)
        }

        override fun setMuted(muted: Boolean) {}

        override fun setCameraOff(off: Boolean) {}

        override fun close() {
            closed = true
        }
    }

    private class Rig {
        val clock = Clock()
        val sent = ArrayList<Pair<String, Map<String, Any?>>>()
        var peer: Peer? = null
        val engine = DirectCallEngine(
            selfId = { "me-device" },
            accountName = { "jett" },
            partnerFor = { if (it == "direct:abc") "sam" else "" },
            sendSignal = { _, type, extra ->
                sent.add(type to extra)
                true
            },
            scheduler = clock,
        ).also { engine ->
            engine.peerFactory = DirectCallPeerFactory { _, _ -> Peer("p").also { peer = it } }
        }

        fun count(type: String): Int = sent.count { it.first == type }

        fun frame(type: String, extra: Map<String, Any?> = emptyMap(), call: String = engine.callId) {
            engine.handleFrame(
                mapOf("type" to type, "call" to call, "senderId" to "sam-device", "sender" to "sam", "to" to "me-device") + extra,
                "direct:abc",
            )
        }

        fun invite(call: String = "call-1") {
            engine.handleFrame(
                mapOf("type" to "call-invite", "call" to call, "senderId" to "sam-device", "sender" to "sam", "media" to "audio"),
                "direct:abc",
            )
        }

        fun liveCaller() {
            expect(engine.start("direct:abc", DirectCallKind.AUDIO))
            frame("call-accept")
            frame("call-answer", mapOf("sdp" to mapOf("type" to "answer", "sdp" to "a1")))
            peer?.onLive?.invoke()
        }
    }

    fun testACallThatNeverConnectsEndsAfterThirtySecondsWithAMessage() {
        val rig = Rig()
        expect(rig.engine.start("direct:abc", DirectCallKind.AUDIO))
        rig.frame("call-accept")
        assertEq(DirectCallPhase.CONNECTING, rig.engine.phase)
        rig.clock.advance(DirectCallEngine.CONNECT_TIMEOUT_MS - 1)
        expect(rig.engine.active)
        rig.clock.advance(1)
        expect(!rig.engine.active)
        assertEq(DirectCallEngine.CONNECT_FAILED, rig.engine.ending)
        assertEq(1, rig.count("call-bye"))
        rig.engine.dismissEnding()
        assertEq(null, rig.engine.ending)
    }

    fun testALostOfferIsResentUntilTheAnswerArrives() {
        val rig = Rig()
        expect(rig.engine.start("direct:abc", DirectCallKind.AUDIO))
        rig.frame("call-accept")
        assertEq(1, rig.count("call-offer"))
        rig.clock.advance(DirectCallEngine.OFFER_RESEND_MS)
        assertEq(2, rig.count("call-offer"))
        rig.frame("call-accept")
        assertEq(3, rig.count("call-offer"))
        rig.frame("call-answer", mapOf("sdp" to mapOf("type" to "answer", "sdp" to "a1")))
        rig.clock.advance(DirectCallEngine.OFFER_RESEND_MS * 3)
        assertEq(3, rig.count("call-offer"))
    }

    fun testTheCalleeResendsItsAcceptUntilAnOfferComesAndRepeatsItsAnswer() {
        val rig = Rig()
        rig.invite()
        expect(rig.engine.acceptInvite())
        assertEq(1, rig.count("call-accept"))
        rig.clock.advance(DirectCallEngine.ACCEPT_RESEND_MS * 2)
        assertEq(3, rig.count("call-accept"))
        rig.frame("call-offer", mapOf("sdp" to mapOf("type" to "offer", "sdp" to "o1")))
        assertEq(1, rig.count("call-answer"))
        rig.clock.advance(DirectCallEngine.ACCEPT_RESEND_MS * 4)
        assertEq(3, rig.count("call-accept"))
        rig.frame("call-offer", mapOf("sdp" to mapOf("type" to "offer", "sdp" to "o1")))
        assertEq(2, rig.count("call-answer"))
        assertEq(1, rig.peer?.answers)
        rig.frame("call-offer", mapOf("sdp" to mapOf("type" to "offer", "sdp" to "o2")))
        assertEq(3, rig.count("call-answer"))
        assertEq(2, rig.peer?.answers)
    }

    fun testADroppedCallRestartsAndShowsReconnecting() {
        val rig = Rig()
        rig.liveCaller()
        assertEq(DirectCallPhase.LIVE, rig.engine.phase)
        expect(rig.engine.liveSince != null)
        rig.peer?.onConnection?.invoke(DirectCallConnection.DISCONNECTED)
        expect(rig.engine.reconnecting)
        assertEq("Reconnecting…", rig.engine.stateLine)
        rig.clock.advance(DirectCallEngine.DROP_GRACE_MS - 1)
        assertEq(0, rig.peer?.restarts)
        rig.clock.advance(1)
        assertEq(1, rig.peer?.restarts)
        assertEq("p-restart-1", (rig.sent.last { it.first == "call-offer" }.second["sdp"] as Map<*, *>)["sdp"])
        rig.clock.advance(DirectCallEngine.RESTART_EVERY_MS)
        assertEq(2, rig.peer?.restarts)
        rig.peer?.onConnection?.invoke(DirectCallConnection.CONNECTED)
        expect(!rig.engine.reconnecting)
        rig.clock.advance(DirectCallEngine.RECONNECT_BUDGET_MS)
        assertEq(2, rig.peer?.restarts)
        expect(rig.engine.active)
    }

    fun testSilentAudioRestartsAtOnceAndGivesUpAfterThirtySeconds() {
        val rig = Rig()
        rig.liveCaller()
        rig.peer?.onConnection?.invoke(DirectCallConnection.STALLED)
        rig.clock.advance(0)
        assertEq(1, rig.peer?.restarts)
        rig.clock.advance(DirectCallEngine.RECONNECT_BUDGET_MS)
        expect(!rig.engine.active)
        assertEq(DirectCallEngine.DROPPED, rig.engine.ending)
    }

    fun testIceIsHeldDuringARestartUntilTheNewAnswerIsApplied() {
        val rig = Rig()
        rig.liveCaller()
        rig.peer?.onConnection?.invoke(DirectCallConnection.FAILED)
        rig.clock.advance(0)
        rig.frame("call-ice", mapOf("ice" to mapOf("candidate" to "c1")))
        assertEq(0, rig.peer?.added?.size)
        rig.frame("call-answer", mapOf("sdp" to mapOf("type" to "answer", "sdp" to "a2")))
        assertEq(1, rig.peer?.added?.size)
    }

    fun testTheCalleeOnlyWaitsWhileTheCallerRestarts() {
        val rig = Rig()
        rig.invite()
        expect(rig.engine.acceptInvite())
        rig.frame("call-offer", mapOf("sdp" to mapOf("type" to "offer", "sdp" to "o1")))
        rig.peer?.onLive?.invoke()
        rig.peer?.onConnection?.invoke(DirectCallConnection.FAILED)
        expect(rig.engine.reconnecting)
        rig.clock.advance(DirectCallEngine.DROP_GRACE_MS)
        assertEq(0, rig.peer?.restarts)
        rig.frame("call-offer", mapOf("sdp" to mapOf("type" to "offer", "sdp" to "o2")))
        rig.peer?.onLive?.invoke()
        expect(!rig.engine.reconnecting)
    }

    fun testAHeldInviteDoesNotGoStaleWhileThePermissionPromptIsOpen() {
        val rig = Rig()
        rig.invite()
        rig.engine.holdInvite()
        rig.clock.advance(DirectCallEngine.INVITE_STALE_MS * 2)
        expect(rig.engine.invite != null)
        expect(rig.engine.acceptInvite())
        expect(!rig.engine.acceptInvite())
    }

    fun testAnInviteThatEndedCannotBeAccepted() {
        val rig = Rig()
        rig.invite()
        rig.engine.holdInvite()
        rig.frame("call-bye", call = "call-1")
        assertEq(null, rig.engine.invite)
        expect(!rig.engine.acceptInvite())
    }
}
