package com.forkmesh.nativeapp.feature.chat

import android.content.Context
import com.forkmesh.nativeapp.api.PrefStore
import com.forkmesh.nativeapp.chat.DirectCallEngine
import com.forkmesh.nativeapp.chat.DirectMessageService
import java.util.concurrent.ExecutorService
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update

object DirectCallFeature {
    const val FACE_PREF = "chat.callFace"
    const val VOICE_PREF = "chat.callVoice"
    const val BACKGROUND_PREF = "chat.callBackground"
    private const val UNINSTALL_WAIT_MS = 3_000L

    private val worker: ExecutorService = Executors.newSingleThreadExecutor { runnable ->
        Thread(runnable, "direct-call").apply { isDaemon = true }
    }
    private val revisionState = MutableStateFlow(0)
    private val previewState = MutableStateFlow<String?>(null)

    val revision: StateFlow<Int> = revisionState.asStateFlow()
    val previewConversation: StateFlow<String?> = previewState.asStateFlow()

    fun openPreview(conversation: String) {
        previewState.value = conversation
    }

    fun closePreview(keepCamera: Boolean = false) {
        previewState.value = null
        if (!keepCamera) worker.execute { runCatching { peers?.endPreview() } }
    }

    @Volatile
    var engine: DirectCallEngine? = null
        private set

    @Volatile
    var peers: WebCallPeers? = null
        private set

    private fun bump() {
        revisionState.update { it + 1 }
    }

    fun install(
        context: Context,
        direct: DirectMessageService,
        selfId: () -> String,
        accountName: () -> String,
        prefs: PrefStore?,
    ): DirectCallEngine {
        val made = DirectCallEngine(
            selfId = selfId,
            accountName = { accountName().trim().lowercase() },
            partnerFor = { conversation -> direct.record(conversation)?.otherUser.orEmpty() },
            sendSignal = { conversation, type, extra ->
                direct.sendCallFrame(conversation, type, extra) ||
                    (direct.open(conversation) && direct.sendCallFrame(conversation, type, extra))
            },
        )
        val factory = WebCallPeers(context.applicationContext, worker, voice = { made.effectiveVoice }) { made.face }
        made.peerFactory = factory
        prefs?.getString(FACE_PREF)?.let { made.setFace(it, CallFaceStore.load(context)) }
        made.onFaceChanged = { face -> prefs?.putString(FACE_PREF, face) }
        made.onFaceImageChanged = { image -> CallFaceStore.save(context, image) }
        prefs?.getString(BACKGROUND_PREF)?.let { made.setBackground(it, CallBackgroundStore.load(context)) }
        made.onBackgroundChanged = { background, image ->
            prefs?.putString(BACKGROUND_PREF, background)
            CallBackgroundStore.save(context, image)
        }
        prefs?.getString(VOICE_PREF)?.let { made.setVoiceChoice(it) }
        made.onVoiceChanged = { choice -> prefs?.putString(VOICE_PREF, choice) }
        made.onMissed = { conversation, media -> direct.send(conversation, "📞 Missed ${media.wire} call") }
        made.addListener { bump() }
        direct.onCallFrame = { plain, conversation -> worker.execute { runCatching { made.handleFrame(plain, conversation) } } }
        engine = made
        peers = factory
        bump()
        return made
    }

    fun uninstall(made: DirectCallEngine, direct: DirectMessageService?) {
        if (engine !== made) return
        engine = null
        peers = null
        direct?.onCallFrame = null
        bump()
        runCatching {
            worker.submit {
                runCatching { made.declineInvite() }
                runCatching { made.hangUp() }
            }.get(UNINSTALL_WAIT_MS, TimeUnit.MILLISECONDS)
        }
    }

    fun run(block: (DirectCallEngine) -> Unit) {
        val current = engine ?: return
        worker.execute { runCatching { block(current) } }
    }
}
