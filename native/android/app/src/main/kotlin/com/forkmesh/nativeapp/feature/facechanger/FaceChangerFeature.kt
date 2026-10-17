package com.forkmesh.nativeapp.feature.facechanger

import android.content.Context
import com.forkmesh.nativeapp.api.PrefStore
import com.forkmesh.nativeapp.chat.DirectCallKind
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.facechanger.FaceChangerLook
import com.forkmesh.nativeapp.facechanger.FaceChangerPart
import com.forkmesh.nativeapp.feature.navigation.NavigationFeature
import com.forkmesh.nativeapp.navigation.Route
import java.util.concurrent.ExecutorService
import java.util.concurrent.Executors
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update

object FaceChangerFeature {
    private const val OWNER = "existing"

    val worker: ExecutorService = Executors.newSingleThreadExecutor { runnable ->
        Thread(runnable, "face-changer").apply { isDaemon = true }
    }

    val look = FaceChangerLook()

    private val revisionState = MutableStateFlow(0)

    val revision: StateFlow<Int> = revisionState.asStateFlow()

    @Volatile
    var peers: WebCallPeers? = null
        private set

    @Volatile
    private var listening = false

    fun install(context: Context) {
        NavigationFeature.register(Route.Kind.FaceChanger, OWNER) { FaceChangerScreen() }
        val app = context.applicationContext
        val prefs = Services.get<PrefStore>()
        look.onFaceChanged = null
        look.onFaceImageChanged = null
        look.onBackgroundChanged = null
        look.onVoiceChanged = null
        prefs?.getString(FaceChangerLook.FACE_PREF)?.let { look.setFace(it, CallFaceStore.load(app)) }
        prefs?.getString(FaceChangerLook.BACKGROUND_PREF)?.let { look.setBackground(it, CallBackgroundStore.load(app)) }
        prefs?.getString(FaceChangerLook.VOICE_PREF)?.let { look.setVoiceChoice(it) }
        look.onFaceChanged = { face -> prefs?.putString(FaceChangerLook.FACE_PREF, face) }
        look.onFaceImageChanged = { image -> CallFaceStore.save(app, image) }
        look.onBackgroundChanged = { background, image ->
            prefs?.putString(FaceChangerLook.BACKGROUND_PREF, background)
            CallBackgroundStore.save(app, image)
        }
        look.onVoiceChanged = { choice -> prefs?.putString(FaceChangerLook.VOICE_PREF, choice) }
        if (peers == null) peers = WebCallPeers(app, worker, voice = { look.effectiveVoice }) { look.face }
        if (!listening) {
            listening = true
            look.addListener { part -> changed(part) }
        }
        bump()
    }

    private fun bump() {
        revisionState.update { it + 1 }
    }

    private fun changed(part: FaceChangerPart) {
        bump()
        val factory = peers ?: return
        val peer = factory.previewed() ?: return
        val video = factory.previewMedia != DirectCallKind.AUDIO
        runCatching {
            when (part) {
                FaceChangerPart.FACE -> {
                    if (video) {
                        if (look.face == FaceChangerLook.CUSTOM_FACE) peer.setFace(look.face, look.faceImage) else peer.setFace(look.face)
                    }
                    peer.setVoice(look.effectiveVoice)
                }
                FaceChangerPart.BACKGROUND -> if (video) peer.setBackground(look.background, look.customBackgroundImage)
                FaceChangerPart.VOICE -> peer.setVoice(look.effectiveVoice)
            }
        }
    }

    fun run(block: (FaceChangerLook) -> Unit) {
        worker.execute { runCatching { block(look) } }
    }
}
