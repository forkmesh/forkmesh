package com.forkmesh.nativeapp.feature.chat

import android.Manifest
import android.annotation.SuppressLint
import android.content.Context
import android.content.MutableContextWrapper
import android.content.pm.PackageManager
import android.graphics.Color
import android.os.Handler
import android.os.Looper
import android.view.ViewGroup
import android.webkit.JavascriptInterface
import android.webkit.PermissionRequest
import android.webkit.WebChromeClient
import android.webkit.WebResourceRequest
import android.webkit.WebResourceResponse
import android.webkit.WebView
import android.webkit.WebViewClient
import com.forkmesh.nativeapp.chat.CallQuality
import com.forkmesh.nativeapp.chat.CallQualityEvent
import com.forkmesh.nativeapp.chat.GroupCallPageEvent
import com.forkmesh.nativeapp.json.Json
import java.io.ByteArrayInputStream
import java.io.IOException
import java.util.Base64
import java.util.concurrent.CompletableFuture
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.CountDownLatch
import java.util.concurrent.Executor
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicInteger

internal class GroupCallPage(
    private val context: Context,
    private val callbacks: Executor,
    private val onEvent: (GroupCallPageEvent) -> Unit,
    private val onQuality: (CallQualityEvent) -> Unit,
    private val signer: (String, ByteArray) -> ByteArray?,
) {
    companion object {
        const val ORIGIN_HOST = "appassets.androidplatform.net"
        const val FOLDER = "group-call/"
        const val URL = "https://$ORIGIN_HOST/${FOLDER}index.html"
        const val BRIDGE = "FmGroupBridge"
        const val TIMEOUT_MS = 20_000L
        const val ATTACH_WAIT_MS = 5_000L
        const val DESTROY_DELAY_MS = 300L
        const val MAX_SIGN_TEXT = 96 * 1024
        private val main = Handler(Looper.getMainLooper())

        fun mime(name: String): String = when (name.substringAfterLast('.', "")) {
            "html" -> "text/html"
            "js" -> "text/javascript"
            "css" -> "text/css"
            "json" -> "application/json"
            else -> "application/octet-stream"
        }
    }

    @Volatile
    private var webView: WebView? = null

    @Volatile
    private var closed = false

    private val ready = CountDownLatch(1)
    private val pending = ConcurrentHashMap<Int, CompletableFuture<Any?>>()
    private val ids = AtomicInteger()

    private inner class Bridge {
        @JavascriptInterface
        fun post(text: String) {
            received(text)
        }

        @JavascriptInterface
        fun sign(call: String?, data: String?): String {
            if (closed || call.isNullOrEmpty() || data.isNullOrEmpty() || data.length > MAX_SIGN_TEXT) return ""
            val bytes = runCatching { Base64.getDecoder().decode(data) }.getOrNull() ?: return ""
            val signature = runCatching { signer(call, bytes) }.getOrNull() ?: return ""
            return Base64.getEncoder().encodeToString(signature)
        }
    }

    private fun refused(): WebResourceResponse =
        WebResourceResponse("text/plain", "utf-8", 404, "Not Found", emptyMap(), ByteArrayInputStream(ByteArray(0)))

    private fun asset(request: WebResourceRequest): WebResourceResponse? {
        val url = request.url
        if (url.host != ORIGIN_HOST) return null
        if (url.scheme != "https") return refused()
        val name = url.path.orEmpty().removePrefix("/")
        if (!name.startsWith(FOLDER) || name.contains("..") || name.contains('\\')) return refused()
        return try {
            WebResourceResponse(mime(name), "utf-8", 200, "OK", mapOf("Cache-Control" to "no-store"), context.assets.open(name))
        } catch (_: IOException) {
            refused()
        }
    }

    @SuppressLint("SetJavaScriptEnabled")
    fun load(): WebView {
        val view = onMain {
            WebView(MutableContextWrapper(context)).apply {
                settings.javaScriptEnabled = true
                settings.mediaPlaybackRequiresUserGesture = false
                settings.domStorageEnabled = false
                settings.allowFileAccess = false
                settings.allowContentAccess = false
                setBackgroundColor(Color.BLACK)
                addJavascriptInterface(Bridge(), BRIDGE)
                webViewClient = object : WebViewClient() {
                    override fun shouldInterceptRequest(view: WebView, request: WebResourceRequest): WebResourceResponse? = asset(request)

                    override fun shouldOverrideUrlLoading(view: WebView, request: WebResourceRequest): Boolean = true
                }
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
                loadUrl(URL)
            }.also { webView = it }
        }
        if (!ready.await(TIMEOUT_MS, TimeUnit.MILLISECONDS)) throw IllegalStateException("The call page did not load.")
        return view
    }

    fun awaitAttached(): Boolean {
        val deadline = System.currentTimeMillis() + ATTACH_WAIT_MS
        while (System.currentTimeMillis() < deadline && !closed) {
            if (onMain { webView?.isAttachedToWindow == true }) return true
            Thread.sleep(50)
        }
        return false
    }

    private fun permitted(resource: String): Boolean = when (resource) {
        PermissionRequest.RESOURCE_AUDIO_CAPTURE -> granted(Manifest.permission.RECORD_AUDIO)
        PermissionRequest.RESOURCE_VIDEO_CAPTURE -> granted(Manifest.permission.CAMERA)
        else -> false
    }

    private fun granted(permission: String): Boolean = context.checkSelfPermission(permission) == PackageManager.PERMISSION_GRANTED

    private fun received(text: String) {
        val message = runCatching { Json.parseObject(text) }.getOrNull() ?: return
        if (message["event"] == "quality") {
            val quality = CallQuality.parse(message) ?: return
            deliver { onQuality(quality) }
            return
        }
        when (val event = GroupCallPageEvent.parse(message) ?: return) {
            GroupCallPageEvent.Ready -> ready.countDown()
            is GroupCallPageEvent.Result -> {
                val future = pending.remove(event.id) ?: return
                if (event.ok) future.complete(event.value) else future.completeExceptionally(IllegalStateException(event.error.ifEmpty { "Call operation failed." }))
            }
            is GroupCallPageEvent.Send -> callbacks.execute { runCatching { onEvent(event) } }
            else -> deliver { onEvent(event) }
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

    private fun evaluate(script: String) {
        main.post {
            val view = webView
            if (view != null && !closed) view.evaluateJavascript(script, null)
        }
    }

    fun request(op: String, args: List<Any?>, timeoutMs: Long = TIMEOUT_MS): Any? {
        check(Looper.myLooper() != Looper.getMainLooper()) { "Call operations must not run on the main thread." }
        if (closed) throw IllegalStateException("The call has ended.")
        val id = ids.incrementAndGet()
        val future = CompletableFuture<Any?>()
        pending[id] = future
        evaluate("window.fmGroup($id, ${Json.stringify(op)}, ${Json.stringify(args)})")
        try {
            return future.get(timeoutMs, TimeUnit.MILLISECONDS)
        } finally {
            pending.remove(id)
        }
    }

    fun post(op: String, args: List<Any?>) {
        if (closed) return
        evaluate("window.fmGroup(0, ${Json.stringify(op)}, ${Json.stringify(args)})")
    }

    fun answerTicket(id: Int, ok: Boolean, value: Any?) {
        evaluate("window.fmGroupTicket($id, $ok, ${Json.stringify(value)})")
    }

    fun close() {
        if (closed) return
        closed = true
        val failure = IllegalStateException("The call has ended.")
        for (future in pending.values) future.completeExceptionally(failure)
        pending.clear()
        ready.countDown()
        main.post {
            val view = webView ?: return@post
            view.evaluateJavascript("window.fmGroup && window.fmGroup(0, \"leave\", [])", null)
            webView = null
            main.postDelayed({
                (view.parent as? ViewGroup)?.removeView(view)
                view.removeJavascriptInterface(BRIDGE)
                view.loadUrl("about:blank")
                view.destroy()
            }, DESTROY_DELAY_MS)
        }
    }
}
