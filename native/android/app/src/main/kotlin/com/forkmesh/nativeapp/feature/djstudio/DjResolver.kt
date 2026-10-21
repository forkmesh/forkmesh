package com.forkmesh.nativeapp.feature.djstudio

import android.annotation.SuppressLint
import android.app.Activity
import android.content.Context
import android.os.Handler
import android.os.Looper
import android.view.ViewGroup
import android.webkit.CookieManager
import android.webkit.JavascriptInterface
import android.webkit.WebResourceRequest
import android.webkit.WebResourceResponse
import android.webkit.WebSettings
import android.webkit.WebView
import android.webkit.WebViewClient
import android.widget.FrameLayout
import com.forkmesh.nativeapp.djstudio.DjStudio
import com.forkmesh.nativeapp.json.Json
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withTimeoutOrNull
import java.io.ByteArrayInputStream
import java.net.HttpURLConnection
import java.net.URL

internal sealed interface DjResolved {
    data class Found(val id: String, val url: String, val title: String, val author: String, val duration: Double) : DjResolved
    data class Failed(val code: Int) : DjResolved
}

@SuppressLint("SetJavaScriptEnabled")
internal class DjResolver(private val context: Context) {
    private val main = Handler(Looper.getMainLooper())
    private val lock = Mutex()
    private var webView: WebView? = null
    private var apiReady = CompletableDeferred<Boolean>()
    private var waiting: CompletableDeferred<DjResolved>? = null
    private var waitingFor = ""
    private var agent = ""

    suspend fun resolve(id: String): DjResolved = lock.withLock {
        if (!DjStudio.isVideoId(id)) return DjResolved.Failed(2)
        val view = view()
        val ready = withTimeoutOrNull(20_000) { apiReady.await() } ?: false
        if (!ready) {
            reload(view)
            return DjResolved.Failed(-1)
        }
        val answer = CompletableDeferred<DjResolved>()
        waiting = answer
        waitingFor = id
        view.evaluateJavascript("fmResolve('$id')", null)
        val result = withTimeoutOrNull(30_000) { answer.await() } ?: DjResolved.Failed(-2)
        waiting = null
        waitingFor = ""
        view.evaluateJavascript("fmIdle()", null)
        result
    }

    fun dock(activity: Activity) {
        val view = view()
        val root = activity.window?.decorView as? ViewGroup ?: return
        if (view.parent === root) return
        (view.parent as? ViewGroup)?.removeView(view)
        root.addView(view, 0, FrameLayout.LayoutParams(2, 2))
    }

    private fun view(): WebView {
        webView?.let { return it }
        val view = WebView(context.applicationContext)
        view.alpha = 0.01f
        view.isFocusable = false
        view.importantForAccessibility = android.view.View.IMPORTANT_FOR_ACCESSIBILITY_NO_HIDE_DESCENDANTS
        view.settings.javaScriptEnabled = true
        view.settings.domStorageEnabled = true
        view.settings.mediaPlaybackRequiresUserGesture = false
        view.settings.cacheMode = WebSettings.LOAD_DEFAULT
        agent = runCatching { WebSettings.getDefaultUserAgent(context) }.getOrDefault("")
        CookieManager.getInstance().setAcceptThirdPartyCookies(view, true)
        view.addJavascriptInterface(Bridge(), "fmAndroid")
        view.webViewClient = object : WebViewClient() {
            override fun shouldInterceptRequest(view: WebView, request: WebResourceRequest): WebResourceResponse? = intercept(request)

            override fun shouldOverrideUrlLoading(view: WebView, request: WebResourceRequest): Boolean = request.isForMainFrame
        }
        webView = view
        reload(view)
        return view
    }

    private fun reload(view: WebView) {
        apiReady = CompletableDeferred()
        view.loadDataWithBaseURL(ORIGIN, HOST_PAGE, "text/html", "utf-8", null)
    }

    private fun intercept(request: WebResourceRequest): WebResourceResponse? {
        val url = request.url ?: return null
        val host = url.host?.lowercase() ?: return null
        if (request.method != "GET" || request.isForMainFrame) return null
        if (!youtubeHost(host) || url.path?.startsWith("/embed/") != true) return null
        return runCatching {
            val connection = URL(url.toString()).openConnection() as HttpURLConnection
            connection.connectTimeout = 10_000
            connection.readTimeout = 15_000
            connection.instanceFollowRedirects = false
            for ((name, value) in request.requestHeaders) {
                if (name.equals("Accept-Encoding", true) || name.equals("Cookie", true)) continue
                connection.setRequestProperty(name, value)
            }
            if (agent.isNotEmpty() && connection.getRequestProperty("User-Agent") == null) connection.setRequestProperty("User-Agent", agent)
            CookieManager.getInstance().getCookie(url.toString())?.let { connection.setRequestProperty("Cookie", it) }
            val status = connection.responseCode
            connection.headerFields["Set-Cookie"]?.forEach { CookieManager.getInstance().setCookie(url.toString(), it) }
            if (status != 200) {
                connection.disconnect()
                return null
            }
            val html = connection.inputStream.use { String(it.readBytes(), Charsets.UTF_8) }
            connection.disconnect()
            val script = "<script>$FRAME_SCRIPT</script>"
            val head = Regex("<head[^>]*>", RegexOption.IGNORE_CASE).find(html)
            val injected = if (head != null) html.substring(0, head.range.last + 1) + script + html.substring(head.range.last + 1) else script + html
            val headers = mapOf("Cache-Control" to "no-store", "Access-Control-Allow-Origin" to "*")
            WebResourceResponse("text/html", "utf-8", 200, "OK", headers, ByteArrayInputStream(injected.toByteArray(Charsets.UTF_8)))
        }.getOrNull()
    }

    private inner class Bridge {
        @JavascriptInterface
        fun post(message: String) {
            val body = Json.parseObject(message)
            main.post { receive(body) }
        }
    }

    private fun receive(body: Map<String, Any?>) {
        when (body["type"]) {
            "api" -> apiReady.complete(true)
            "found" -> {
                val id = body["id"] as? String ?: return
                val src = body["src"] as? String ?: return
                if (id != waitingFor || !src.startsWith("https://")) return
                waiting?.complete(
                    DjResolved.Found(id, src, body["title"] as? String ?: "", body["author"] as? String ?: "", (body["duration"] as? Number)?.toDouble() ?: 0.0),
                )
            }
            "error" -> {
                val id = body["id"] as? String ?: return
                if (id != waitingFor) return
                waiting?.complete(DjResolved.Failed((body["code"] as? Number)?.toInt() ?: -1))
            }
        }
    }

    private fun youtubeHost(host: String): Boolean =
        host == "youtube.com" || host.endsWith(".youtube.com") ||
            host == "youtube-nocookie.com" || host.endsWith(".youtube-nocookie.com")

    companion object {
        const val ORIGIN = "https://forkmesh.com/"

        private const val HOST_PAGE = """<!doctype html><html><head><meta name="viewport" content="width=device-width,initial-scale=1">
<style>html,body{margin:0;background:#000;overflow:hidden}</style></head><body><div id="h"><div id="p"></div></div><script>
var P=null,ready=false,want="",seen={};
function post(m){try{fmAndroid.post(JSON.stringify(m))}catch(e){}}
function onYouTubeIframeAPIReady(){post({type:"api"})}
function fmResolve(id){want=id;seen={};if(P&&ready){try{P.mute();P.loadVideoById({videoId:id,startSeconds:0})}catch(e){make(id)}return}make(id)}
function fmIdle(){want="";if(P&&ready)try{P.stopVideo()}catch(e){}}
function make(id){if(P){try{P.destroy()}catch(e){}document.getElementById("h").innerHTML='<div id="p"></div>';ready=false}
P=new YT.Player("p",{host:"https://www.youtube-nocookie.com",width:"320",height:"180",videoId:id,
playerVars:{playsinline:1,controls:0,autoplay:1,mute:1,disablekb:1,fs:0,rel:0,iv_load_policy:3,enablejsapi:1,origin:location.origin},
events:{onReady:function(){ready=true;try{P.mute();if(want)P.playVideo()}catch(e){}check()},
onStateChange:function(){check()},
onError:function(e){if(want)post({type:"error",id:want,code:e.data})}}})}
function check(){if(!P||!ready||!want)return;var v=P.getVideoData?P.getVideoData():null,d=P.getDuration?P.getDuration():0;
if(!v||v.video_id!==want||!(d>0))return;
for(var s in seen){if(Math.abs(seen[s]-d)<1.6){var id=want;want="";post({type:"found",id:id,src:s,title:v.title||"",author:v.author||"",duration:d});try{P.pauseVideo()}catch(e){}return}}}
addEventListener("message",function(e){var m=e.data;if(!m||typeof m!=="object"||m.fmdjFrame!=="source"||typeof m.src!=="string")return;seen[m.src]=+m.dur||0;check()});
setInterval(check,300);
</script><script src="https://www.youtube.com/iframe_api"></script></body></html>"""

        private const val FRAME_SCRIPT = """(function(){
if(window.top===window||!/(^|\.)youtube(-nocookie)?\.com$/.test(location.hostname))return;
["MediaSource","ManagedMediaSource","WebKitMediaSource"].forEach(function(n){try{delete window[n]}catch(e){}try{if(window[n])Object.defineProperty(window,n,{configurable:true,writable:true,value:undefined})}catch(e){}});
try{var D=Document.prototype;Object.defineProperty(D,"hidden",{configurable:true,get:function(){return false}});Object.defineProperty(D,"visibilityState",{configurable:true,get:function(){return "visible"}})}catch(e){}
["visibilitychange","webkitvisibilitychange"].forEach(function(t){addEventListener(t,function(e){e.stopImmediatePropagation()},true)});
function report(v){var s=v.currentSrc||v.src||"";if(s.indexOf("https:")!==0||/m3u8|manifest/i.test(s))return;var d=isFinite(v.duration)?v.duration:0;
if(!(d>0)){var m=/[?&]dur=([0-9.]+)/.exec(s);d=m?+m[1]:0}if(d>0)parent.postMessage({fmdjFrame:"source",src:s,dur:d},"*")}
["loadstart","loadedmetadata","durationchange","playing"].forEach(function(t){document.addEventListener(t,function(e){if(e.target&&e.target.tagName==="VIDEO")report(e.target)},true)});
setInterval(function(){var v=document.querySelector("video");if(v)report(v)},700);
})();"""
    }
}
