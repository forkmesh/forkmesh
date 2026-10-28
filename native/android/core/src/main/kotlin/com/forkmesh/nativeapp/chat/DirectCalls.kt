package com.forkmesh.nativeapp.chat

import com.forkmesh.nativeapp.facechanger.FaceChangerLook
import com.forkmesh.nativeapp.facechanger.FaceChangerPart
import com.forkmesh.nativeapp.json.jsonText
import java.util.UUID
import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.Executors
import java.util.concurrent.ScheduledExecutorService
import java.util.concurrent.TimeUnit

enum class DirectCallKind(val wire: String) {
    AUDIO("audio"),
    VIDEO("video");

    companion object {
        fun from(wire: String): DirectCallKind = if (wire == "video") VIDEO else AUDIO
    }
}

enum class DirectCallPhase { RINGING, CONNECTING, LIVE }

enum class DirectCallConnection(val wire: String) {
    CONNECTED("connected"),
    DISCONNECTED("disconnected"),
    FAILED("failed"),
    STALLED("stalled");

    companion object {
        fun from(wire: String): DirectCallConnection? = entries.firstOrNull { it.wire == wire }
    }
}

data class DirectCallEnding(val title: String, val message: String)

data class DirectCallInvite(
    val conversation: String,
    val callId: String,
    val from: String,
    val partner: String,
    val media: DirectCallKind,
)

interface DirectCallPeer {
    var onIceCandidate: ((Map<String, Any?>) -> Unit)?
    var onLive: (() -> Unit)?
    var onClosed: (() -> Unit)?

    var onConnection: ((DirectCallConnection) -> Unit)?
        get() = null
        set(value) {}

    fun offer(): Map<String, Any?>

    fun restartOffer(): Map<String, Any?>? = null

    fun resumeMedia() {}

    fun answer(offer: Map<String, Any?>): Map<String, Any?>

    fun acceptAnswer(answer: Map<String, Any?>)

    fun add(candidate: Map<String, Any?>)

    fun setMuted(muted: Boolean)

    fun setCameraOff(off: Boolean)

    fun setFace(id: String) {}

    fun setFace(id: String, image: String) {
        setFace(id)
    }

    fun setBackground(id: String, image: String) {}

    fun setVoice(id: String) {}

    fun observeVoice(handler: (String) -> Unit) {}

    fun close()
}

fun interface DirectCallPeerFactory {
    fun makePeer(media: DirectCallKind, iceServers: List<String>): DirectCallPeer
}

fun interface CallScheduler {
    fun schedule(delayMs: Long, run: () -> Unit): () -> Unit
}

object ExecutorCallScheduler : CallScheduler {
    private val executor: ScheduledExecutorService = Executors.newSingleThreadScheduledExecutor { runnable ->
        Thread(runnable, "direct-call-timer").apply { isDaemon = true }
    }

    override fun schedule(delayMs: Long, run: () -> Unit): () -> Unit {
        val future = executor.schedule({ runCatching(run) }, delayMs, TimeUnit.MILLISECONDS)
        return { future.cancel(false) }
    }
}

class DirectCallEngine(
    private val selfId: () -> String,
    private val accountName: () -> String,
    private val partnerFor: (String) -> String,
    private val sendSignal: (String, String, Map<String, Any?>) -> Boolean,
    private val scheduler: CallScheduler = ExecutorCallScheduler,
    val look: FaceChangerLook = FaceChangerLook(),
) {
    companion object {
        val ICE_SERVERS = listOf("stun:stun.cloudflare.com:3478")
        const val RING_TIMEOUT_MS = 45_000L
        const val INVITE_RESEND_MS = 3_000L
        const val INVITE_STALE_MS = 9_000L
        const val ACCEPT_RESEND_MS = 2_000L
        const val ACCEPT_RESENDS = 8
        const val OFFER_RESEND_MS = 3_000L
        const val OFFER_RESENDS = 5
        const val CONNECT_TIMEOUT_MS = 30_000L
        const val DROP_GRACE_MS = 2_000L
        const val RESTART_EVERY_MS = 6_000L
        const val RECONNECT_BUDGET_MS = 30_000L
        val CONNECT_FAILED = DirectCallEnding("Could not connect", "The call could not connect. Try again.")
        val DROPPED = DirectCallEnding("Call lost", "The connection dropped and did not come back. Try calling again.")
        const val CALL_INVITE_TYPE = "call-invite"
        const val RING_TOPIC = "calls"
        val FRAME_TYPES = setOf(
            "call-invite", "call-accept", "call-decline", "call-offer", "call-answer", "call-ice", "call-bye",
        )
    }

    @Volatile
    var peerFactory: DirectCallPeerFactory? = null

    @Volatile
    var onMissed: ((String, DirectCallKind) -> Unit)? = null

    private val lock = Any()
    private val listeners = CopyOnWriteArrayList<() -> Unit>()

    @Volatile
    var conversation: String = ""
        private set

    @Volatile
    var callId: String = ""
        private set

    @Volatile
    var partner: String = ""
        private set

    @Volatile
    var media: DirectCallKind = DirectCallKind.AUDIO
        private set

    @Volatile
    var phase: DirectCallPhase? = null
        private set

    @Volatile
    var invite: DirectCallInvite? = null
        private set

    @Volatile
    var muted: Boolean = false
        private set

    @Volatile
    var cameraOff: Boolean = false
        private set

    @Volatile
    var voice: String = ""
        private set

    @Volatile
    var reconnecting: Boolean = false
        private set

    @Volatile
    var liveSince: Long? = null
        private set

    @Volatile
    var ending: DirectCallEnding? = null
        private set

    val face: String get() = look.face

    val faceImage: String get() = look.faceImage

    val background: String get() = look.background

    val backgroundImage: String get() = look.backgroundImage

    val voiceChoice: String get() = look.voiceChoice

    val effectiveVoice: String get() = look.effectiveVoice

    private val lookListener: (FaceChangerPart) -> Unit = { part -> lookChanged(part) }

    init {
        look.addListener(lookListener)
    }

    fun detach() {
        look.removeListener(lookListener)
    }

    private var caller = false
    private var peerId = ""
    private var peer: DirectCallPeer? = null
    private val pendingIce = ArrayList<Map<String, Any?>>()
    private var remoteReady = false
    private var cancelRing: (() -> Unit)? = null
    private var cancelResend: (() -> Unit)? = null
    private var cancelStale: (() -> Unit)? = null
    private var heldInvite = ""
    private var cancelAccept: (() -> Unit)? = null
    private var cancelOffer: (() -> Unit)? = null
    private var cancelConnect: (() -> Unit)? = null
    private var cancelDrop: (() -> Unit)? = null
    private var cancelRestart: (() -> Unit)? = null
    private var restartNow = false
    private var restarting = false
    private var dropRound = 0
    private var offerReceived = false
    private var answering = false
    private var answeredOffer = ""
    private var storedAnswer: Map<String, Any?>? = null
    private var localOffer: Map<String, Any?>? = null
    private var offerOutstanding = false
    private var offerRound = 0
    private var appliedAnswer = ""
    private var answerApplied = false
    private var holdingIce = false
    private val heldIce = ArrayList<Map<String, Any?>>()

    val available: Boolean get() = peerFactory != null

    val active: Boolean get() = phase != null

    val stateLine: String
        get() = if (reconnecting && phase != null) "Reconnecting…" else when (phase) {
            DirectCallPhase.RINGING -> "Calling @$partner…"
            DirectCallPhase.CONNECTING -> "Connecting…"
            DirectCallPhase.LIVE -> "Connected · end-to-end encrypted"
            null -> ""
        }

    fun addListener(listener: () -> Unit) {
        listeners.add(listener)
    }

    fun removeListener(listener: () -> Unit) {
        listeners.remove(listener)
    }

    private fun notifyListeners() {
        for (listener in listeners) runCatching { listener() }
    }

    fun start(conversation: String, media: DirectCallKind): Boolean {
        val partner = partnerFor(conversation)
        synchronized(lock) {
            if (active || !available || conversation.isEmpty() || partner.isEmpty()) return false
            this.conversation = conversation
            callId = UUID.randomUUID().toString().lowercase()
            this.partner = partner
            this.media = media
            phase = DirectCallPhase.RINGING
            caller = true
            peerId = ""
            muted = false
            cameraOff = false
            ending = null
            scheduleRing(callId)
        }
        sendSignal(conversation, "call-invite", linkedMapOf("call" to callId, "media" to media.wire))
        notifyListeners()
        return true
    }

    private fun scheduleRing(id: String) {
        cancelResend = scheduler.schedule(INVITE_RESEND_MS) { resendInvite(id) }
        cancelRing = scheduler.schedule(RING_TIMEOUT_MS) { ringTimedOut(id) }
    }

    private fun resendInvite(id: String) {
        val scope: String
        val wire: String
        synchronized(lock) {
            if (callId != id || phase != DirectCallPhase.RINGING) return
            scope = conversation
            wire = media.wire
            cancelResend = scheduler.schedule(INVITE_RESEND_MS) { resendInvite(id) }
        }
        sendSignal(scope, "call-invite", linkedMapOf("call" to id, "media" to wire))
    }

    private fun ringTimedOut(id: String) {
        synchronized(lock) {
            if (callId != id || phase != DirectCallPhase.RINGING) return
        }
        hangUp()
    }

    fun holdInvite() {
        synchronized(lock) { heldInvite = invite?.callId.orEmpty() }
    }

    fun dismissEnding() {
        ending = null
    }

    fun resumeMedia() {
        val target = synchronized(lock) { peer } ?: return
        runCatching { target.resumeMedia() }
    }

    fun acceptInvite(): Boolean {
        val pending: DirectCallInvite
        val fields: Map<String, Any?>
        synchronized(lock) {
            pending = invite ?: return false
            if (active || !available) return false
            clearInvite()
            conversation = pending.conversation
            callId = pending.callId
            partner = pending.partner
            media = pending.media
            phase = DirectCallPhase.CONNECTING
            caller = false
            peerId = pending.from
            muted = false
            cameraOff = false
            ending = null
            armConnectTimeout()
            fields = linkedMapOf("call" to pending.callId, "to" to pending.from)
            cancelAccept = scheduler.schedule(ACCEPT_RESEND_MS) { resendAccept(pending.callId, fields, ACCEPT_RESENDS) }
        }
        sendSignal(pending.conversation, "call-accept", fields)
        notifyListeners()
        return true
    }

    private fun resendAccept(id: String, fields: Map<String, Any?>, left: Int) {
        val scope: String
        synchronized(lock) {
            if (callId != id || !active || caller || offerReceived) return
            scope = conversation
            cancelAccept = if (left > 1) scheduler.schedule(ACCEPT_RESEND_MS) { resendAccept(id, fields, left - 1) } else null
        }
        sendSignal(scope, "call-accept", fields)
    }

    fun declineInvite() {
        val pending: DirectCallInvite
        synchronized(lock) {
            pending = invite ?: return
            clearInvite()
        }
        sendSignal(
            pending.conversation, "call-decline",
            linkedMapOf("call" to pending.callId, "to" to pending.from, "reason" to "declined"),
        )
        notifyListeners()
    }

    fun hangUp() {
        val missed = synchronized(lock) {
            if (caller && phase == DirectCallPhase.RINGING) conversation to media else null
        }
        if (missed == null) {
            end(notifyPeer = true, reason = "")
            return
        }
        end(notifyPeer = true, reason = "missed")
        onMissed?.invoke(missed.first, missed.second)
    }

    fun toggleMute() {
        val target: DirectCallPeer?
        synchronized(lock) {
            if (!active) return
            muted = !muted
            target = peer
        }
        target?.setMuted(muted)
        notifyListeners()
    }

    fun toggleCamera() {
        val target: DirectCallPeer?
        synchronized(lock) {
            if (!active || media != DirectCallKind.VIDEO) return
            cameraOff = !cameraOff
            target = peer
        }
        target?.setCameraOff(cameraOff)
        notifyListeners()
    }

    fun setFace(id: String, image: String = ""): Boolean = look.setFace(id, image)

    fun setBackground(id: String, image: String = ""): Boolean = look.setBackground(id, image)

    fun setVoiceChoice(choice: String): Boolean = look.setVoiceChoice(choice)

    private fun lookChanged(part: FaceChangerPart) {
        val target: DirectCallPeer?
        synchronized(lock) { target = if (media == DirectCallKind.VIDEO) peer else null }
        when (part) {
            FaceChangerPart.FACE -> {
                notifyListeners()
                target?.let { made -> runCatching { applyFaceTo(made) } }
                if (voiceChoice == FaceChangerLook.VOICE_MATCH_FACE) pushVoice()
            }
            FaceChangerPart.BACKGROUND -> {
                notifyListeners()
                target?.let { made -> runCatching { made.setBackground(background, look.customBackgroundImage) } }
            }
            FaceChangerPart.VOICE -> {
                notifyListeners()
                pushVoice()
            }
        }
    }

    private fun applyFaceTo(made: DirectCallPeer) {
        val chosen = face
        if (chosen == FaceChangerLook.CUSTOM_FACE) made.setFace(chosen, faceImage) else made.setFace(chosen)
    }

    private fun pushVoice() {
        val target: DirectCallPeer
        val next: String
        synchronized(lock) {
            target = peer ?: return
            next = effectiveVoice
            voice = next
        }
        notifyListeners()
        runCatching { target.setVoice(next) }
    }

    private fun applyVoice(made: DirectCallPeer, id: String) {
        val chosen = effectiveVoice
        synchronized(lock) { voice = chosen }
        made.observeVoice { applied ->
            val current: Boolean
            synchronized(lock) {
                current = callId == id
                if (current) voice = applied
            }
            if (current) notifyListeners()
        }
        if (chosen.isNotEmpty()) made.setVoice(chosen)
    }

    private fun applyFace(made: DirectCallPeer, kind: DirectCallKind) {
        if (kind != DirectCallKind.VIDEO) return
        if (face.isNotEmpty()) applyFaceTo(made)
        val scene = background
        if (scene.isNotEmpty()) made.setBackground(scene, look.customBackgroundImage)
    }

    fun handleFrame(plain: Map<String, Any?>, scope: String) {
        val type = jsonText(plain["type"])
        if (type !in FRAME_TYPES) return
        val frameCall = jsonText(plain["call"])
        val from = jsonText(plain["senderId"])
        val me = selfId()
        if (frameCall.isEmpty() || from.isEmpty() || from == me) return
        val own = accountName().trim().lowercase()
        val fromOwnAccount = own.isNotEmpty() && jsonText(plain["sender"]).trim().lowercase() == own
        val to = jsonText(plain["to"])
        if (to.isNotEmpty() && to != me) {
            if (fromOwnAccount && invite?.callId == frameCall && (type == "call-accept" || type == "call-decline")) {
                synchronized(lock) { clearInvite() }
                notifyListeners()
            }
            return
        }
        when (type) {
            "call-invite" -> inviteFrame(plain, scope, frameCall, from, fromOwnAccount)
            "call-bye" -> {
                var changed = false
                synchronized(lock) {
                    if (invite?.callId == frameCall) {
                        clearInvite()
                        changed = true
                    }
                }
                if (changed) notifyListeners()
                if (active && callId == frameCall) end(notifyPeer = false, reason = "")
            }
            "call-accept" -> {
                var resend: Map<String, Any?>? = null
                val proceed = synchronized(lock) {
                    if (!active || callId != frameCall || !caller) return
                    if (peerId.isNotEmpty() && from == peerId && offerOutstanding) resend = localOffer
                    peerId.isEmpty()
                }
                if (proceed) connectAsCaller(from) else resend?.let { sendOffer(it) }
            }
            "call-decline" -> {
                if (synchronized(lock) { active && callId == frameCall && caller }) end(notifyPeer = false, reason = "")
            }
            "call-offer" -> {
                val raw = plain["sdp"] as? Map<*, *> ?: return
                val sdp = raw.entries.associate { jsonText(it.key) to it.value }
                val text = jsonText(sdp["sdp"])
                var stored: Map<String, Any?>? = null
                var scope = ""
                var target = ""
                val fresh = synchronized(lock) {
                    if (!active || callId != frameCall || caller || from != peerId) return
                    offerReceived = true
                    cancelAccept?.invoke()
                    cancelAccept = null
                    if (answering) return
                    if (peer != null && text == answeredOffer) {
                        stored = storedAnswer
                        scope = conversation
                        target = peerId
                    }
                    peer == null
                }
                val again = stored
                when {
                    fresh -> connectAsCallee(sdp, text)
                    again != null -> sendSignal(scope, "call-answer", linkedMapOf("call" to frameCall, "to" to target, "sdp" to again))
                    else -> renegotiate(sdp, text)
                }
            }
            "call-answer" -> {
                val raw = plain["sdp"] as? Map<*, *> ?: return
                val sdp = raw.entries.associate { jsonText(it.key) to it.value }
                val text = jsonText(sdp["sdp"])
                val round: Int
                val target = synchronized(lock) {
                    if (!active || callId != frameCall || !caller || from != peerId) return
                    val made = peer ?: return
                    if (!offerOutstanding || (answerApplied && text == appliedAnswer)) return
                    offerOutstanding = false
                    cancelOffer?.invoke()
                    cancelOffer = null
                    round = offerRound
                    made
                }
                try {
                    target.acceptAnswer(sdp)
                    synchronized(lock) {
                        appliedAnswer = text
                        answerApplied = true
                        remoteReady = true
                    }
                    flushPendingIce()
                } catch (_: Exception) {
                    val fatal = synchronized(lock) {
                        if (!answerApplied) {
                            true
                        } else {
                            if (offerRound == round && peer === target) offerOutstanding = true
                            false
                        }
                    }
                    if (fatal) end(notifyPeer = true, reason = "")
                }
            }
            "call-ice" -> {
                val ice = plain["ice"] as? Map<*, *> ?: return
                val candidate = ice.entries.associate { jsonText(it.key) to it.value }
                val target = synchronized(lock) {
                    if (!active || callId != frameCall || from != peerId) return
                    val made = peer
                    if (remoteReady && !answering && !(caller && answerApplied && offerOutstanding) && made != null) {
                        made
                    } else {
                        pendingIce.add(candidate)
                        null
                    }
                }
                target?.let { runCatching { it.add(candidate) } }
            }
        }
    }

    private fun inviteFrame(plain: Map<String, Any?>, scope: String, frameCall: String, from: String, fromOwnAccount: Boolean) {
        if (fromOwnAccount || !available) return
        val busy = synchronized(lock) {
            if (active && callId == frameCall) return
            if (active) return@synchronized true
            val media = DirectCallKind.from(jsonText(plain["media"]))
            var partner = partnerFor(scope)
            if (partner.isEmpty()) partner = jsonText(plain["sender"]).take(63).ifEmpty { "peer" }
            cancelStale?.invoke()
            invite = DirectCallInvite(scope, frameCall, from, partner, media)
            if (heldInvite != frameCall) cancelStale = scheduler.schedule(INVITE_STALE_MS) { staleInvite(frameCall) }
            false
        }
        if (busy) {
            sendSignal(scope, "call-decline", linkedMapOf("call" to frameCall, "to" to from, "reason" to "busy"))
            return
        }
        notifyListeners()
    }

    private fun staleInvite(frameCall: String) {
        var changed = false
        synchronized(lock) {
            if (invite?.callId == frameCall && heldInvite != frameCall) {
                clearInvite()
                changed = true
            }
        }
        if (changed) notifyListeners()
    }

    private fun connectAsCaller(deviceId: String) {
        val factory = peerFactory
        val id: String
        val kind: DirectCallKind
        synchronized(lock) {
            cancelResend?.invoke()
            cancelResend = null
            cancelRing?.invoke()
            cancelRing = null
            peerId = deviceId
            phase = DirectCallPhase.CONNECTING
            id = callId
            kind = media
            armConnectTimeout()
        }
        notifyListeners()
        if (factory == null) {
            end(notifyPeer = true, reason = "")
            return
        }
        try {
            val made = factory.makePeer(kind, ICE_SERVERS)
            if (synchronized(lock) { callId != id || !active }) {
                runCatching { made.close() }
                return
            }
            attach(made, id)
            applyFace(made, kind)
            applyVoice(made, id)
            val sdp = made.offer()
            if (synchronized(lock) { callId != id || peer !== made }) return
            sendOffer(sdp)
        } catch (_: Exception) {
            if (synchronized(lock) { callId == id }) end(notifyPeer = true, reason = "")
        }
    }

    private fun sendOffer(sdp: Map<String, Any?>) {
        val id: String
        val target: String
        val scope: String
        synchronized(lock) {
            id = callId
            target = peerId
            scope = conversation
            if (localOffer != sdp || !offerOutstanding) {
                localOffer = sdp
                offerOutstanding = true
                offerRound += 1
                val round = offerRound
                cancelOffer?.invoke()
                cancelOffer = scheduler.schedule(OFFER_RESEND_MS) { resendOffer(id, target, scope, round, OFFER_RESENDS) }
            }
        }
        sendSignal(scope, "call-offer", linkedMapOf("call" to id, "to" to target, "sdp" to sdp))
    }

    private fun resendOffer(id: String, target: String, scope: String, round: Int, left: Int) {
        val offer: Map<String, Any?>
        synchronized(lock) {
            if (callId != id || !offerOutstanding || offerRound != round) return
            offer = localOffer ?: return
            cancelOffer = if (left > 1) {
                scheduler.schedule(OFFER_RESEND_MS) { resendOffer(id, target, scope, round, left - 1) }
            } else {
                null
            }
        }
        sendSignal(scope, "call-offer", linkedMapOf("call" to id, "to" to target, "sdp" to offer))
    }

    private fun connectAsCallee(offer: Map<String, Any?>, text: String) {
        val factory = peerFactory ?: return
        val id: String
        val kind: DirectCallKind
        synchronized(lock) {
            if (peer != null || answering) return
            answering = true
            id = callId
            kind = media
        }
        try {
            val made = factory.makePeer(kind, ICE_SERVERS)
            if (synchronized(lock) { callId != id || !active }) {
                runCatching { made.close() }
                return
            }
            attach(made, id)
            applyFace(made, kind)
            applyVoice(made, id)
            val answer = made.answer(offer)
            val scope: String
            val target: String
            synchronized(lock) {
                if (callId != id || peer !== made) return
                answering = false
                answeredOffer = text
                storedAnswer = answer
                remoteReady = true
                scope = conversation
                target = peerId
            }
            flushPendingIce()
            sendSignal(scope, "call-answer", linkedMapOf("call" to id, "to" to target, "sdp" to answer))
        } catch (_: Exception) {
            val current = synchronized(lock) {
                if (callId == id) answering = false
                callId == id
            }
            if (current) end(notifyPeer = true, reason = "")
        }
    }

    private fun renegotiate(offer: Map<String, Any?>, text: String) {
        val current: DirectCallPeer
        val id: String
        synchronized(lock) {
            current = peer ?: return
            if (answering) return
            answering = true
            holdingIce = true
            id = callId
        }
        val answer = runCatching { current.answer(offer) }.getOrNull()
        val scope: String
        val target: String
        synchronized(lock) {
            if (callId != id || peer !== current) return
            answering = false
            if (answer != null) {
                answeredOffer = text
                storedAnswer = answer
            }
            scope = conversation
            target = peerId
        }
        if (answer != null) sendSignal(scope, "call-answer", linkedMapOf("call" to id, "to" to target, "sdp" to answer))
        releaseIce()
        flushPendingIce()
    }

    private fun releaseIce() {
        val queued: List<Map<String, Any?>>
        val id: String
        val target: String
        val scope: String
        synchronized(lock) {
            holdingIce = false
            queued = heldIce.toList()
            heldIce.clear()
            id = callId
            target = peerId
            scope = conversation
        }
        if (queued.isEmpty() || target.isEmpty()) return
        for (candidate in queued) sendSignal(scope, "call-ice", linkedMapOf("call" to id, "to" to target, "ice" to candidate))
    }

    private fun armConnectTimeout() {
        cancelConnect?.invoke()
        val id = callId
        cancelConnect = scheduler.schedule(CONNECT_TIMEOUT_MS) { connectTimedOut(id) }
    }

    private fun connectTimedOut(id: String) {
        synchronized(lock) {
            if (callId != id || !active || phase == DirectCallPhase.LIVE) return
            cancelConnect = null
            ending = CONNECT_FAILED
        }
        end(notifyPeer = true, reason = "")
    }

    private fun connectionChanged(state: DirectCallConnection) {
        var changed = false
        synchronized(lock) {
            if (!active || phase == DirectCallPhase.RINGING) return
            val was = reconnecting
            when (state) {
                DirectCallConnection.CONNECTED -> recovered()
                DirectCallConnection.DISCONNECTED -> lost(immediate = false)
                DirectCallConnection.FAILED, DirectCallConnection.STALLED -> lost(immediate = true)
            }
            changed = was != reconnecting
        }
        if (changed) notifyListeners()
    }

    private fun recovered() {
        dropRound += 1
        reconnecting = false
        cancelDrop?.invoke()
        cancelDrop = null
        cancelRestart?.invoke()
        cancelRestart = null
        restartNow = false
    }

    private fun lost(immediate: Boolean) {
        if (phase == DirectCallPhase.LIVE) {
            reconnecting = true
            armReconnectBudget()
        }
        if (!caller) return
        if (cancelRestart != null && (restartNow || !immediate)) return
        cancelRestart?.invoke()
        restartNow = immediate
        val round = dropRound
        val id = callId
        cancelRestart = scheduler.schedule(if (immediate) 0L else DROP_GRACE_MS) { restartLoop(id, round) }
    }

    private fun restartLoop(id: String, round: Int) {
        if (synchronized(lock) { callId != id || dropRound != round }) return
        restart()
        synchronized(lock) {
            if (callId != id || dropRound != round) return
            cancelRestart = scheduler.schedule(RESTART_EVERY_MS) { restartLoop(id, round) }
        }
    }

    private fun armReconnectBudget() {
        if (cancelDrop != null) return
        val round = dropRound
        val id = callId
        cancelDrop = scheduler.schedule(RECONNECT_BUDGET_MS) { reconnectGaveUp(id, round) }
    }

    private fun reconnectGaveUp(id: String, round: Int) {
        synchronized(lock) {
            if (callId != id || dropRound != round || !reconnecting) return
            cancelDrop = null
            ending = DROPPED
        }
        end(notifyPeer = true, reason = "")
    }

    private fun restart() {
        val current: DirectCallPeer
        val id: String
        synchronized(lock) {
            if (!caller || restarting || peerId.isEmpty()) return
            current = peer ?: return
            restarting = true
            holdingIce = true
            id = callId
        }
        val made = runCatching { current.restartOffer() }.getOrNull()
        synchronized(lock) {
            restarting = false
            if (callId != id || peer !== current) return
        }
        if (made != null) sendOffer(made)
        releaseIce()
    }

    private fun attach(made: DirectCallPeer, id: String) {
        synchronized(lock) { peer = made }
        made.onIceCandidate = { candidate ->
            val scope: String
            val target: String
            var live = false
            synchronized(lock) {
                scope = conversation
                target = peerId
                live = callId == id && target.isNotEmpty()
                if (live && holdingIce) {
                    heldIce.add(candidate)
                    live = false
                }
            }
            if (live) {
                sendSignal(scope, "call-ice", linkedMapOf("call" to id, "to" to target, "ice" to candidate))
            }
        }
        made.onLive = {
            var changed = false
            synchronized(lock) {
                if (callId == id) {
                    changed = phase != DirectCallPhase.LIVE || reconnecting
                    phase = DirectCallPhase.LIVE
                    if (liveSince == null) liveSince = System.currentTimeMillis()
                    cancelConnect?.invoke()
                    cancelConnect = null
                    recovered()
                }
            }
            if (changed) notifyListeners()
        }
        made.onConnection = { state ->
            if (synchronized(lock) { callId == id }) connectionChanged(state)
        }
        made.onClosed = {
            if (synchronized(lock) { callId == id }) end(notifyPeer = false, reason = "")
        }
    }

    private fun flushPendingIce() {
        val target: DirectCallPeer?
        val queued: List<Map<String, Any?>>
        synchronized(lock) {
            target = peer
            queued = pendingIce.toList()
            pendingIce.clear()
        }
        if (target == null) return
        for (candidate in queued) runCatching { target.add(candidate) }
    }

    private fun clearInvite() {
        cancelStale?.invoke()
        cancelStale = null
        heldInvite = ""
        invite = null
    }

    private fun end(notifyPeer: Boolean, reason: String) {
        val scope: String
        val id: String
        val closing: DirectCallPeer?
        synchronized(lock) {
            if (!active) return
            cancelResend?.invoke()
            cancelResend = null
            cancelRing?.invoke()
            cancelRing = null
            for (cancel in listOf(cancelAccept, cancelOffer, cancelConnect, cancelDrop, cancelRestart)) cancel?.invoke()
            cancelAccept = null
            cancelOffer = null
            cancelConnect = null
            cancelDrop = null
            cancelRestart = null
            restartNow = false
            restarting = false
            dropRound += 1
            offerReceived = false
            answering = false
            answeredOffer = ""
            storedAnswer = null
            localOffer = null
            offerOutstanding = false
            appliedAnswer = ""
            answerApplied = false
            holdingIce = false
            heldIce.clear()
            reconnecting = false
            liveSince = null
            scope = conversation
            id = callId
            conversation = ""
            callId = ""
            partner = ""
            phase = null
            peerId = ""
            remoteReady = false
            pendingIce.clear()
            muted = false
            cameraOff = false
            voice = ""
            closing = peer
            peer = null
        }
        closing?.onClosed = null
        closing?.onConnection = null
        runCatching { closing?.close() }
        if (notifyPeer) {
            val extra = linkedMapOf<String, Any?>("call" to id)
            if (reason.isNotEmpty()) extra["reason"] = reason
            sendSignal(scope, "call-bye", extra)
        }
        notifyListeners()
    }
}
