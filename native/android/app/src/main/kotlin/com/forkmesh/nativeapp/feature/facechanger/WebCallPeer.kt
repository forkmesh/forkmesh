package com.forkmesh.nativeapp.feature.facechanger

import android.Manifest
import android.annotation.SuppressLint
import android.content.Context
import android.content.MutableContextWrapper
import android.content.pm.PackageManager
import android.graphics.Color
import android.media.AudioDeviceInfo
import android.media.AudioManager
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.util.Base64
import android.view.ViewGroup
import android.webkit.JavascriptInterface
import android.webkit.PermissionRequest
import android.webkit.WebChromeClient
import android.webkit.WebView
import com.forkmesh.nativeapp.chat.DirectCallKind
import com.forkmesh.nativeapp.chat.DirectCallPeer
import com.forkmesh.nativeapp.chat.DirectCallPeerFactory
import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.json.asJsonMap
import com.forkmesh.nativeapp.json.jsonText
import java.io.File
import java.util.concurrent.CompletableFuture
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.CountDownLatch
import java.util.concurrent.Executor
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicInteger
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow

class WebCallPeers(
    private val context: Context,
    private val callbacks: Executor,
    private val voice: () -> String = { "" },
    private val face: () -> String,
) : DirectCallPeerFactory {
    private val viewState = MutableStateFlow<WebView?>(null)
    private val facesState = MutableStateFlow(false)
    private val backgroundsState = MutableStateFlow<Map<String, String>?>(null)
    private val previewState = MutableStateFlow(false)
    private val routeState = MutableStateFlow("")

    @Volatile
    private var previewPeer: WebCallPeer? = null

    @Volatile
    private var livePeer: WebCallPeer? = null

    @Volatile
    private var previewOwner: String = ""

    @Volatile
    var previewMedia: DirectCallKind? = null
        private set

    val view: StateFlow<WebView?> = viewState.asStateFlow()
    val facesSupported: StateFlow<Boolean> = facesState.asStateFlow()
    val backgrounds: StateFlow<Map<String, String>?> = backgroundsState.asStateFlow()
    val previewing: StateFlow<Boolean> = previewState.asStateFlow()
    val route: StateFlow<String> = routeState.asStateFlow()

    val busy: Boolean get() = livePeer != null

    fun previewed(): WebCallPeer? = previewPeer

    private fun open(): WebCallPeer {
        var made: WebCallPeer? = null
        val peer = WebCallPeer(context, callbacks) { gone ->
            viewState.compareAndSet(gone, null)
            if (livePeer === made) {
                livePeer = null
                routeState.value = ""
            }
        }
        made = peer
        return peer
    }

    fun preview(
        face: String,
        background: String,
        image: String,
        faceImage: String = "",
        media: DirectCallKind = DirectCallKind.VIDEO,
        owner: String = "",
    ): WebCallPeer {
        check(livePeer == null) { "A call is using the camera." }
        if (previewPeer != null && previewMedia != media) endPreview()
        val peer = previewPeer ?: open().also { made ->
            try {
                viewState.value = made.load()
            } catch (error: Exception) {
                made.close()
                throw error
            }
            previewPeer = made
        }
        previewMedia = media
        previewOwner = owner
        previewState.value = true
        try {
            peer.preview(media, voice(), face, background, image, faceImage)
            learn(peer, media == DirectCallKind.VIDEO)
        } catch (error: Exception) {
            endPreview()
            throw error
        }
        return peer
    }

    fun endPreview() {
        val peer = previewPeer ?: return
        previewPeer = null
        previewMedia = null
        previewOwner = ""
        previewState.value = false
        peer.close()
    }

    fun endPreview(owner: String) {
        if (previewPeer != null && previewOwner == owner) endPreview()
    }

    private fun watchRoute(peer: WebCallPeer) {
        routeState.value = ""
        peer.onRoute = { next -> if (livePeer === peer) routeState.value = next }
    }

    private fun learn(peer: WebCallPeer, video: Boolean) {
        facesState.value = video && (peer.request("faceList", emptyList()) as? List<*>).orEmpty().isNotEmpty()
        backgroundsState.value = if (video) peer.backgroundThumbnails() else null
    }

    override fun makePeer(media: DirectCallKind, iceServers: List<String>): DirectCallPeer {
        val video = media == DirectCallKind.VIDEO
        val previewed = previewPeer
        if (previewed != null && previewMedia == media) {
            previewPeer = null
            previewMedia = null
            previewOwner = ""
            previewState.value = false
            livePeer = previewed
            watchRoute(previewed)
            try {
                previewed.dropRecording()
                previewed.request("setup", listOf(media.wire, iceServers, "", voice()))
                if (!video) learn(previewed, false)
            } catch (error: Exception) {
                if (livePeer === previewed) livePeer = null
                previewed.close()
                throw error
            }
            return previewed
        }
        endPreview()
        val peer = open()
        livePeer = peer
        watchRoute(peer)
        try {
            viewState.value = peer.load()
            peer.request("setup", listOf(media.wire, iceServers, if (video) face() else "", voice()))
            learn(peer, video)
        } catch (error: Exception) {
            if (livePeer === peer) livePeer = null
            peer.close()
            throw error
        }
        return peer
    }
}

data class WebCallRecording(val mime: String, val durationMs: Long)

class WebCallPeer internal constructor(
    private val context: Context,
    private val callbacks: Executor,
    private val onDestroyed: (WebView) -> Unit,
) : DirectCallPeer {
    companion object {
        const val ASSET = "direct-call.html"
        const val ORIGIN_HOST = "appassets.androidplatform.net"
        const val BASE_URL = "https://$ORIGIN_HOST/"
        const val BRIDGE = "FmCallBridge"
        const val TIMEOUT_MS = 20_000L
        const val DESTROY_DELAY_MS = 300L
        private val main = Handler(Looper.getMainLooper())
    }

    override var onIceCandidate: ((Map<String, Any?>) -> Unit)? = null
    override var onLive: (() -> Unit)? = null
    override var onClosed: (() -> Unit)? = null

    @Volatile
    private var webView: WebView? = null

    @Volatile
    private var closed = false

    @Volatile
    private var voiceObserver: ((String) -> Unit)? = null

    @Volatile
    var onRoute: ((String) -> Unit)? = null

    private val ready = CountDownLatch(1)
    private val pending = ConcurrentHashMap<Int, CompletableFuture<Any?>>()
    private val ids = AtomicInteger()

    private inner class Bridge {
        @JavascriptInterface
        fun post(text: String) {
            received(text)
        }
    }

    @SuppressLint("SetJavaScriptEnabled")
    fun load(): WebView {
        val html = context.assets.open(ASSET).bufferedReader().use { it.readText() }
        val view = onMain {
            WebView(MutableContextWrapper(context)).apply {
                settings.javaScriptEnabled = true
                settings.mediaPlaybackRequiresUserGesture = false
                settings.domStorageEnabled = true
                settings.allowFileAccess = false
                settings.allowContentAccess = false
                setBackgroundColor(Color.BLACK)
                addJavascriptInterface(Bridge(), BRIDGE)
                webChromeClient = object : WebChromeClient() {
                    override fun onPermissionRequest(request: PermissionRequest) {
                        val granted = if (request.origin?.host == ORIGIN_HOST) {
                            request.resources.filter { permitted(it) }.toTypedArray()
                        } else {
                            emptyArray()
                        }
                        if (granted.isEmpty()) request.deny() else request.grant(granted)
                    }
                }
                loadDataWithBaseURL(BASE_URL, html, "text/html", "utf-8", null)
            }.also { webView = it }
        }
        if (!ready.await(TIMEOUT_MS, TimeUnit.MILLISECONDS)) throw IllegalStateException("The call page did not load.")
        return view
    }

    private fun permitted(resource: String): Boolean = when (resource) {
        PermissionRequest.RESOURCE_AUDIO_CAPTURE -> granted(Manifest.permission.RECORD_AUDIO)
        PermissionRequest.RESOURCE_VIDEO_CAPTURE -> granted(Manifest.permission.CAMERA)
        else -> false
    }

    private fun granted(permission: String): Boolean =
        context.checkSelfPermission(permission) == PackageManager.PERMISSION_GRANTED

    private fun received(text: String) {
        val message = runCatching { Json.parseObject(text) }.getOrNull() ?: return
        when (jsonText(message["event"])) {
            "ready" -> ready.countDown()
            "result" -> {
                val id = (message["id"] as? Number)?.toInt() ?: return
                val future = pending.remove(id) ?: return
                if (message["ok"] == true) {
                    future.complete(message["value"])
                } else {
                    future.completeExceptionally(IllegalStateException(jsonText(message["error"]).ifEmpty { "Call operation failed." }))
                }
            }
            "ice" -> {
                val candidate = message["candidate"].asJsonMap()
                if (candidate.isNotEmpty()) deliver { onIceCandidate?.invoke(candidate) }
            }
            "voice" -> {
                val applied = jsonText(message["id"])
                deliver { voiceObserver?.invoke(applied) }
            }
            "route" -> {
                val route = jsonText(message["route"])
                if (route == "direct" || route == "relay") deliver { onRoute?.invoke(route) }
            }
            "live" -> deliver { onLive?.invoke() }
            "closed" -> deliver { onClosed?.invoke() }
        }
    }

    private fun deliver(block: () -> Unit) {
        if (closed) return
        callbacks.execute {
            if (!closed) runCatching(block)
        }
    }

    private fun <T> onMain(block: () -> T): T {
        if (Looper.myLooper() == Looper.getMainLooper()) return block()
        val future = CompletableFuture<T>()
        main.post {
            try {
                future.complete(block())
            } catch (error: Throwable) {
                future.completeExceptionally(error)
            }
        }
        return future.get(TIMEOUT_MS, TimeUnit.MILLISECONDS)
    }

    fun request(op: String, args: List<Any?>): Any? {
        check(Looper.myLooper() != Looper.getMainLooper()) { "Call operations must not run on the main thread." }
        if (closed) throw IllegalStateException("The call has ended.")
        val id = ids.incrementAndGet()
        val future = CompletableFuture<Any?>()
        pending[id] = future
        val script = "window.fmCall($id, ${Json.stringify(op)}, ${Json.stringify(args)})"
        main.post {
            val view = webView
            if (view == null || closed) {
                future.completeExceptionally(IllegalStateException("The call has ended."))
            } else {
                view.evaluateJavascript(script, null)
            }
        }
        try {
            return future.get(TIMEOUT_MS, TimeUnit.MILLISECONDS)
        } finally {
            pending.remove(id)
        }
    }

    private fun description(value: Any?): Map<String, Any?> {
        val map = value.asJsonMap()
        if (jsonText(map["sdp"]).isEmpty()) throw IllegalStateException("The call page returned no session description.")
        return map
    }

    override fun offer(): Map<String, Any?> = description(request("offer", emptyList()))

    override fun answer(offer: Map<String, Any?>): Map<String, Any?> = description(request("answer", listOf(offer)))

    override fun acceptAnswer(answer: Map<String, Any?>) {
        request("accept", listOf(answer))
    }

    override fun add(candidate: Map<String, Any?>) {
        request("ice", listOf(candidate))
    }

    override fun setMuted(muted: Boolean) {
        request("mute", listOf(muted))
    }

    override fun setCameraOff(off: Boolean) {
        request("camera", listOf(off))
    }

    override fun setFace(id: String, image: String) {
        request("face", listOf(id, image))
    }

    fun faceState(): String = jsonText(request("faceState", emptyList()))

    override fun setFace(id: String) {
        request("face", listOf(id))
    }

    override fun setVoice(id: String) {
        request("voice", listOf(id))
    }

    override fun observeVoice(handler: (String) -> Unit) {
        voiceObserver = handler
    }

    fun preview(
        media: DirectCallKind,
        voice: String,
        face: String = "",
        background: String = "",
        image: String = "",
        faceImage: String = "",
    ): String = jsonText(request("preview", listOf(media.wire, voice, face, background, image, faceImage)))

    override fun setBackground(id: String, image: String) {
        request("background", listOf(id, image))
    }

    fun backgroundThumbnails(): Map<String, String> {
        val list = request("backgroundList", emptyList()) as? List<*> ?: return emptyMap()
        val out = LinkedHashMap<String, String>()
        for (entry in list) {
            val item = entry.asJsonMap()
            out[jsonText(item["id"])] = jsonText(item["thumb"])
        }
        return out
    }

    fun setMonitor(on: Boolean): Boolean = request("monitor", listOf(on)) == true

    fun startRecording(face: String, background: String, voice: String) {
        request("record", listOf(face, background, voice))
    }

    fun finishRecording(target: File): WebCallRecording {
        try {
            val summary = request("recordStop", emptyList()).asJsonMap()
            val size = (summary["size"] as? Number)?.toLong() ?: 0L
            check(size > 0) { "empty_recording" }
            var written = 0L
            target.outputStream().buffered().use { out ->
                while (written < size) {
                    val chunk = Base64.decode(jsonText(request("recordChunk", listOf(written))), Base64.DEFAULT)
                    check(chunk.isNotEmpty()) { "recording_read" }
                    out.write(chunk)
                    written += chunk.size
                }
            }
            check(written == size) { "recording_read" }
            val mime = jsonText(summary["mime"]).substringBefore(';').trim().lowercase()
            return WebCallRecording(
                mime = if (mime.startsWith("video/") || mime.startsWith("audio/")) mime else "video/mp4",
                durationMs = (summary["durationMs"] as? Number)?.toLong() ?: 0L,
            )
        } finally {
            dropRecording()
        }
    }

    fun dropRecording() {
        if (!closed) runCatching { request("recordDrop", emptyList()) }
    }

    override fun close() {
        if (closed) return
        closed = true
        ready.countDown()
        val failure = IllegalStateException("The call has ended.")
        for (future in pending.values) future.completeExceptionally(failure)
        pending.clear()
        main.post {
            val view = webView ?: return@post
            webView = null
            onDestroyed(view)
            view.evaluateJavascript("window.fmCall && window.fmCall(0, \"close\", [])", null)
            main.postDelayed({
                (view.parent as? ViewGroup)?.removeView(view)
                view.removeJavascriptInterface(BRIDGE)
                view.loadUrl("about:blank")
                view.destroy()
            }, DESTROY_DELAY_MS)
        }
    }
}

fun callHeadphonesConnected(context: Context): Boolean {
    val audio = context.getSystemService(AudioManager::class.java) ?: return false
    val worn = mutableSetOf(
        AudioDeviceInfo.TYPE_WIRED_HEADPHONES,
        AudioDeviceInfo.TYPE_WIRED_HEADSET,
        AudioDeviceInfo.TYPE_BLUETOOTH_A2DP,
        AudioDeviceInfo.TYPE_BLUETOOTH_SCO,
        AudioDeviceInfo.TYPE_USB_HEADSET,
    )
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) worn.add(AudioDeviceInfo.TYPE_BLE_HEADSET)
    return audio.getDevices(AudioManager.GET_DEVICES_OUTPUTS).any { it.type in worn }
}

fun WebView.adoptContext(context: Context): WebView {
    (this.context as? MutableContextWrapper)?.baseContext = context
    (parent as? ViewGroup)?.removeView(this)
    return this
}
