package com.forkmesh.nativeapp.feature.chat

import android.Manifest
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import com.forkmesh.nativeapp.chat.DirectCallEngine
import com.forkmesh.nativeapp.chat.DirectCallKind
import com.forkmesh.nativeapp.chat.DirectMessageService
import com.forkmesh.nativeapp.feature.facechanger.FaceChangerFeature
import com.forkmesh.nativeapp.feature.facechanger.WebCallPeers
import com.forkmesh.nativeapp.platform.IntentTokens
import com.forkmesh.nativeapp.platform.ProjectLifecycle
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch

data class PreCall(val conversation: String, val partner: String, val media: DirectCallKind)

object DirectCallFeature {
    private const val UNINSTALL_WAIT_MS = 3_000L
    const val PRECALL_OWNER = "precall"

    private val worker get() = FaceChangerFeature.worker
    private val revisionState = MutableStateFlow(0)
    private val precallState = MutableStateFlow<PreCall?>(null)

    val revision: StateFlow<Int> = revisionState.asStateFlow()
    val precall: StateFlow<PreCall?> = precallState.asStateFlow()

    @Volatile
    var engine: DirectCallEngine? = null
        private set

    @Volatile
    var peers: WebCallPeers? = null
        private set

    @Volatile
    private var ringer: CallRinger? = null

    @Volatile
    private var appContext: Context? = null

    @Volatile
    private var partnerOf: (String) -> String = { "" }

    @Volatile
    private var direct: DirectMessageService? = null

    private fun bump() {
        revisionState.update { it + 1 }
    }

    fun install(
        context: Context,
        direct: DirectMessageService,
        selfId: () -> String,
        accountName: () -> String,
    ): DirectCallEngine {
        val partner = { conversation: String -> direct.record(conversation)?.otherUser.orEmpty() }
        val made = DirectCallEngine(
            selfId = selfId,
            accountName = { accountName().trim().lowercase() },
            partnerFor = partner,
            sendSignal = { conversation, type, extra ->
                direct.sendCallFrame(conversation, type, extra) ||
                    (direct.open(conversation) && direct.sendCallFrame(conversation, type, extra))
            },
            look = FaceChangerFeature.look,
        )
        if (FaceChangerFeature.peers == null) FaceChangerFeature.install(context)
        val factory = FaceChangerFeature.peers
        made.peerFactory = factory
        made.onMissed = { conversation, media -> direct.send(conversation, "📞 Missed ${media.wire} call") }
        val wasActive = AtomicBoolean(false)
        made.addListener {
            val active = made.phase != null
            if (active) precallState.value = null
            if (wasActive.getAndSet(active) && !active) worker.execute { runCatching { factory?.endPreview(PRECALL_OWNER) } }
            bump()
        }
        direct.onCallFrame = { plain, conversation -> worker.execute { runCatching { made.handleFrame(plain, conversation) } } }
        val app = context.applicationContext
        appContext = app
        partnerOf = partner
        this.direct = direct
        engine = made
        peers = factory
        precallState.value = null
        val rings = CallRinger(app, made)
        ringer = rings
        rings.start()
        ProjectLifecycle.onClose { rings.release() }
        bump()
        return made
    }

    fun uninstall(made: DirectCallEngine, direct: DirectMessageService?) {
        if (engine !== made) return
        engine = null
        peers = null
        partnerOf = { "" }
        this.direct = null
        precallState.value = null
        ringer?.release()
        ringer = null
        made.detach()
        direct?.onCallFrame = null
        bump()
        runCatching {
            worker.submit {
                runCatching { made.declineInvite() }
                runCatching { made.hangUp() }
                runCatching { FaceChangerFeature.peers?.endPreview(PRECALL_OWNER) }
            }.get(UNINSTALL_WAIT_MS, TimeUnit.MILLISECONDS)
        }
    }

    fun wake(conversation: String): Boolean {
        val current = engine ?: return false
        val service = direct ?: return false
        if (current.active) return true
        ProjectLifecycle.scope.launch(Dispatchers.IO) { runCatching { service.openRinging(conversation) } }
        return true
    }

    fun ringing(conversation: String): Boolean {
        val current = engine ?: return false
        return current.active || current.invite?.conversation == conversation
    }

    fun run(block: (DirectCallEngine) -> Unit) {
        val current = engine ?: return
        worker.execute { runCatching { block(current) } }
    }

    val busy: Boolean get() = engine?.active == true || peers?.busy == true || precallState.value != null

    fun openPreCall(conversation: String, media: DirectCallKind): String? {
        val current = engine ?: return "Calls are not available right now."
        if (current.active || peers?.busy == true || GroupCallFeature.active) return "A call is already in progress."
        if (precallState.value != null) return null
        val partner = partnerOf(conversation)
        if (partner.isEmpty()) return "The call could not start. Try again in a moment."
        precallState.value = PreCall(conversation, partner, media)
        return null
    }

    fun closePreCall() {
        precallState.value = null
    }

    fun permitted(context: Context, media: DirectCallKind): Boolean {
        val needed = if (media == DirectCallKind.VIDEO) {
            listOf(Manifest.permission.RECORD_AUDIO, Manifest.permission.CAMERA)
        } else {
            listOf(Manifest.permission.RECORD_AUDIO)
        }
        return needed.all { context.checkSelfPermission(it) == PackageManager.PERMISSION_GRANTED }
    }

    fun onIntent(intent: Intent?): Boolean {
        val action = intent?.action ?: return false
        if (action != IncomingCallNotifier.ACTION_SHOW && action != IncomingCallNotifier.ACTION_ACCEPT) return false
        val trusted = IntentTokens.carries(appContext, intent)
        val callId = runCatching { intent.getStringExtra(IncomingCallNotifier.EXTRA_CALL) }.getOrNull().orEmpty()
        intent.action = null
        intent.removeExtra(IncomingCallNotifier.EXTRA_CALL)
        IntentTokens.strip(intent)
        if (trusted && action == IncomingCallNotifier.ACTION_ACCEPT) accept(callId)
        return true
    }

    fun accept(callId: String) {
        val context = appContext ?: return
        run { current ->
            val pending = current.invite ?: return@run
            if (pending.callId != callId || current.phase != null) return@run
            if (permitted(context, pending.media)) current.acceptInvite()
        }
    }

    fun decline(callId: String) {
        run { current ->
            if (current.invite?.callId == callId) current.declineInvite()
        }
    }
}
