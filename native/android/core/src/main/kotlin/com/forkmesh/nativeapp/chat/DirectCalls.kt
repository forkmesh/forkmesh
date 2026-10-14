package com.forkmesh.nativeapp.chat

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

data class DirectCallInvite(
    val conversation: String,
    val callId: String,
    val from: String,
    val partner: String,
    val media: DirectCallKind,
)

data class DirectCallFace(val id: String, val label: String)

interface DirectCallPeer {
    var onIceCandidate: ((Map<String, Any?>) -> Unit)?
    var onLive: (() -> Unit)?
    var onClosed: (() -> Unit)?

    fun offer(): Map<String, Any?>

    fun answer(offer: Map<String, Any?>): Map<String, Any?>

    fun acceptAnswer(answer: Map<String, Any?>)

    fun add(candidate: Map<String, Any?>)

    fun setMuted(muted: Boolean)

    fun setCameraOff(off: Boolean)

    fun setFace(id: String) {}

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
) {
    companion object {
        val ICE_SERVERS = listOf("stun:stun.cloudflare.com:3478")
        const val RING_TIMEOUT_MS = 45_000L
        const val INVITE_RESEND_MS = 3_000L
        const val INVITE_STALE_MS = 9_000L
        val FRAME_TYPES = setOf(
            "call-invite", "call-accept", "call-decline", "call-offer", "call-answer", "call-ice", "call-bye",
        )
        val FACES = listOf(
            DirectCallFace("maya", "Maya"),
            DirectCallFace("theo", "Theo"),
            DirectCallFace("kofi", "Kofi"),
            DirectCallFace("ingrid", "Ingrid"),
            DirectCallFace("ren", "Ren"),
        )
        const val CUSTOM_BACKGROUND = "custom"
        val BACKGROUNDS = listOf(
            DirectCallFace("", "No background"),
            DirectCallFace("blur", "Blur"),
            DirectCallFace("blur-strong", "Strong blur"),
            DirectCallFace("world", "ForkMesh World"),
            DirectCallFace("office", "ForkMesh office"),
            DirectCallFace("meeting", "Meeting room"),
            DirectCallFace("library", "Home library"),
            DirectCallFace("skyline", "City window"),
            DirectCallFace(CUSTOM_BACKGROUND, "Your photo"),
        )

        fun backgroundLabel(id: String): String = BACKGROUNDS.firstOrNull { it.id == id }?.label ?: "No background"
        const val VOICE_MATCH_FACE = "face"
        const val VOICE_OFF = "off"
        val VOICES: List<DirectCallFace> get() = FACES

        fun voiceLabel(id: String): String = VOICES.firstOrNull { it.id == id }?.label ?: "Your own"
    }

    @Volatile
    var peerFactory: DirectCallPeerFactory? = null

    @Volatile
    var onMissed: ((String, DirectCallKind) -> Unit)? = null

    @Volatile
    var onFaceChanged: ((String) -> Unit)? = null

    @Volatile
    var onVoiceChanged: ((String) -> Unit)? = null

    @Volatile
    var onBackgroundChanged: ((String, String) -> Unit)? = null

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
    var face: String = ""
        private set

    @Volatile
    var background: String = ""
        private set

    @Volatile
    var backgroundImage: String = ""
        private set

    @Volatile
    var voiceChoice: String = VOICE_MATCH_FACE
        private set

    @Volatile
    var voice: String = ""
        private set

    val effectiveVoice: String
        get() = when (val choice = voiceChoice) {
            VOICE_OFF -> ""
            VOICE_MATCH_FACE -> face
            else -> choice
        }

    private var caller = false
    private var peerId = ""
    private var peer: DirectCallPeer? = null
    private val pendingIce = ArrayList<Map<String, Any?>>()
    private var remoteReady = false
    private var cancelRing: (() -> Unit)? = null
    private var cancelResend: (() -> Unit)? = null
    private var cancelStale: (() -> Unit)? = null

    val available: Boolean get() = peerFactory != null

    val active: Boolean get() = phase != null

    val stateLine: String
        get() = when (phase) {
            DirectCallPhase.RINGING -> "Calling @$partner…"
            DirectCallPhase.CONNECTING -> "Connecting…"
            DirectCallPhase.LIVE -> "Connected · encrypted peer-to-peer"
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
        val missedConversation: String
        val missedMedia: DirectCallKind
        synchronized(lock) {
            if (callId != id || phase != DirectCallPhase.RINGING) return
            missedConversation = conversation
            missedMedia = media
        }
        end(notifyPeer = true, reason = "missed")
        onMissed?.invoke(missedConversation, missedMedia)
    }

    fun acceptInvite() {
        val pending: DirectCallInvite
        synchronized(lock) {
            pending = invite ?: return
            if (active || !available) return
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
        }
        sendSignal(pending.conversation, "call-accept", linkedMapOf("call" to pending.callId, "to" to pending.from))
        notifyListeners()
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
        end(notifyPeer = true, reason = "")
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

    fun setFace(id: String): Boolean {
        if (id.isNotEmpty() && FACES.none { it.id == id }) return false
        val target: DirectCallPeer?
        synchronized(lock) {
            face = id
            target = if (media == DirectCallKind.VIDEO) peer else null
        }
        notifyListeners()
        onFaceChanged?.invoke(id)
        target?.let { runCatching { it.setFace(id) } }
        if (voiceChoice == VOICE_MATCH_FACE) pushVoice()
        return true
    }

    fun setBackground(id: String, image: String = ""): Boolean {
        if (BACKGROUNDS.none { it.id == id }) return false
        val custom = id == CUSTOM_BACKGROUND
        val target: DirectCallPeer?
        val picture: String
        synchronized(lock) {
            picture = if (custom && image.isNotEmpty()) image else backgroundImage
            if (custom && picture.isEmpty()) return false
            if (id == background && picture == backgroundImage) return true
            background = id
            backgroundImage = picture
            target = if (media == DirectCallKind.VIDEO) peer else null
        }
        notifyListeners()
        onBackgroundChanged?.invoke(id, picture)
        target?.let { runCatching { it.setBackground(id, if (custom) picture else "") } }
        return true
    }

    fun setVoiceChoice(choice: String): Boolean {
        val valid = choice == VOICE_OFF || choice == VOICE_MATCH_FACE || VOICES.any { it.id == choice }
        if (!valid) return false
        synchronized(lock) {
            if (voiceChoice == choice) return true
            voiceChoice = choice
        }
        onVoiceChanged?.invoke(choice)
        pushVoice()
        return true
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
        val chosen = face
        if (kind == DirectCallKind.VIDEO && chosen.isNotEmpty()) made.setFace(chosen)
        val scene = background
        if (kind == DirectCallKind.VIDEO && scene.isNotEmpty()) {
            made.setBackground(scene, if (scene == CUSTOM_BACKGROUND) backgroundImage else "")
        }
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
                val proceed = synchronized(lock) { active && callId == frameCall && caller && peerId.isEmpty() }
                if (proceed) connectAsCaller(from)
            }
            "call-decline" -> {
                if (synchronized(lock) { active && callId == frameCall && caller }) end(notifyPeer = false, reason = "")
            }
            "call-offer" -> {
                val sdp = plain["sdp"] as? Map<*, *> ?: return
                val proceed = synchronized(lock) { active && callId == frameCall && !caller && from == peerId && peer == null }
                if (proceed) connectAsCallee(sdp.entries.associate { jsonText(it.key) to it.value })
            }
            "call-answer" -> {
                val sdp = plain["sdp"] as? Map<*, *> ?: return
                val target = synchronized(lock) {
                    if (active && callId == frameCall && caller && from == peerId) peer else null
                } ?: return
                try {
                    target.acceptAnswer(sdp.entries.associate { jsonText(it.key) to it.value })
                    synchronized(lock) { remoteReady = true }
                    flushPendingIce()
                } catch (_: Exception) {
                    end(notifyPeer = true, reason = "")
                }
            }
            "call-ice" -> {
                val ice = plain["ice"] as? Map<*, *> ?: return
                val candidate = ice.entries.associate { jsonText(it.key) to it.value }
                val target = synchronized(lock) {
                    if (!active || callId != frameCall || from != peerId) return
                    if (remoteReady) {
                        peer
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
            cancelStale = scheduler.schedule(INVITE_STALE_MS) { staleInvite(frameCall) }
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
            if (invite?.callId == frameCall) {
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
        val scope: String
        synchronized(lock) {
            cancelResend?.invoke()
            cancelResend = null
            cancelRing?.invoke()
            cancelRing = null
            peerId = deviceId
            phase = DirectCallPhase.CONNECTING
            id = callId
            kind = media
            scope = conversation
        }
        notifyListeners()
        if (factory == null) {
            end(notifyPeer = true, reason = "")
            return
        }
        try {
            val made = factory.makePeer(kind, ICE_SERVERS)
            attach(made, id)
            applyFace(made, kind)
            applyVoice(made, id)
            val sdp = made.offer()
            sendSignal(scope, "call-offer", linkedMapOf("call" to id, "to" to deviceId, "sdp" to sdp))
        } catch (_: Exception) {
            end(notifyPeer = true, reason = "")
        }
    }

    private fun connectAsCallee(offer: Map<String, Any?>) {
        val factory = peerFactory ?: return
        val id: String
        val kind: DirectCallKind
        val scope: String
        val target: String
        synchronized(lock) {
            id = callId
            kind = media
            scope = conversation
            target = peerId
        }
        try {
            val made = factory.makePeer(kind, ICE_SERVERS)
            attach(made, id)
            applyFace(made, kind)
            applyVoice(made, id)
            val answer = made.answer(offer)
            synchronized(lock) { remoteReady = true }
            flushPendingIce()
            sendSignal(scope, "call-answer", linkedMapOf("call" to id, "to" to target, "sdp" to answer))
        } catch (_: Exception) {
            end(notifyPeer = true, reason = "")
        }
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
            }
            if (live) {
                sendSignal(scope, "call-ice", linkedMapOf("call" to id, "to" to target, "ice" to candidate))
            }
        }
        made.onLive = {
            var changed = false
            synchronized(lock) {
                if (callId == id) {
                    phase = DirectCallPhase.LIVE
                    changed = true
                }
            }
            if (changed) notifyListeners()
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
        runCatching { closing?.close() }
        if (notifyPeer) {
            val extra = linkedMapOf<String, Any?>("call" to id)
            if (reason.isNotEmpty()) extra["reason"] = reason
            sendSignal(scope, "call-bye", extra)
        }
        notifyListeners()
    }
}
