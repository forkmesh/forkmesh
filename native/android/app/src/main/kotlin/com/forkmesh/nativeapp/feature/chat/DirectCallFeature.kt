package com.forkmesh.nativeapp.feature.chat

import android.content.Context
import com.forkmesh.nativeapp.chat.DirectCallEngine
import com.forkmesh.nativeapp.chat.DirectMessageService
import com.forkmesh.nativeapp.feature.facechanger.FaceChangerFeature
import com.forkmesh.nativeapp.feature.facechanger.WebCallPeers
import java.util.concurrent.TimeUnit
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update

object DirectCallFeature {
    private const val UNINSTALL_WAIT_MS = 3_000L

    private val worker get() = FaceChangerFeature.worker
    private val revisionState = MutableStateFlow(0)

    val revision: StateFlow<Int> = revisionState.asStateFlow()

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
    ): DirectCallEngine {
        val made = DirectCallEngine(
            selfId = selfId,
            accountName = { accountName().trim().lowercase() },
            partnerFor = { conversation -> direct.record(conversation)?.otherUser.orEmpty() },
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
        made.detach()
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
