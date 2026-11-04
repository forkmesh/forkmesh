package com.forkmesh.nativeapp.feature.meeting

import android.Manifest
import android.annotation.SuppressLint
import android.app.Activity
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.graphics.Color
import android.net.Uri
import android.os.Handler
import android.os.Looper
import android.view.WindowManager
import android.webkit.JavascriptInterface
import android.webkit.PermissionRequest
import android.webkit.ValueCallback
import android.webkit.WebChromeClient
import android.webkit.WebResourceRequest
import android.webkit.WebView
import android.webkit.WebViewClient
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.viewinterop.AndroidView
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.account.SettingsService
import com.forkmesh.nativeapp.feature.navigation.NavigationFeature
import com.forkmesh.nativeapp.feature.rssreader.openInBrowser
import com.forkmesh.nativeapp.feature.rssreader.shareText
import com.forkmesh.nativeapp.feature.tools.ToolsAppBar
import com.forkmesh.nativeapp.navigation.MeetingLink
import com.forkmesh.nativeapp.navigation.Route
import com.forkmesh.nativeapp.ui.kit.FmCircularProgressIndicator
import com.forkmesh.nativeapp.ui.kit.FmEmptyState
import com.forkmesh.nativeapp.ui.kit.FmTone
import com.forkmesh.nativeapp.ui.kit.FmToneButton
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmSpace

private val MEDIA_PERMISSIONS = arrayOf(Manifest.permission.CAMERA, Manifest.permission.RECORD_AUDIO)
private const val BRIDGE = "FmMeetingBridge"
private const val MAX_NAME = 120
private const val MAX_TEXT = 2 * 1024 * 1024

private fun mediaPermitted(context: Context): Boolean =
    MEDIA_PERMISSIONS.all { context.checkSelfPermission(it) == PackageManager.PERMISSION_GRANTED }

object MeetingFeature {
    private const val OWNER = "WS13"

    fun install() {
        NavigationFeature.register(Route.Kind.Meeting, OWNER) { route -> MeetingScreen(route as Route.Meeting) }
    }
}

class MeetingScreen(private val meeting: Route.Meeting) : Screen() {
    override val title: String get() = "Meeting"

    private var webView: WebView? = null
    private var pendingFiles: ValueCallback<Array<Uri>>? = null

    @Composable
    override fun Content(navigator: Navigator) {
        val context = LocalContext.current
        val server = Services.get<SettingsService>()?.serverUrl?.value.orEmpty()
        var asked by remember { mutableStateOf(mediaPermitted(context)) }
        val ask = rememberLauncherForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) { asked = true }
        val pick = rememberLauncherForActivityResult(ActivityResultContracts.GetContent()) { uri ->
            pendingFiles?.onReceiveValue(uri?.let { arrayOf(it) })
            pendingFiles = null
        }
        LaunchedEffect(Unit) { if (!asked) ask.launch(MEDIA_PERMISSIONS) }
        DisposableEffect(Unit) {
            val window = (context as? Activity)?.window
            window?.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
            onDispose {
                window?.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
                release()
            }
        }
        Column(Modifier.fillMaxSize().background(Fm.colors.canvas)) {
            ToolsAppBar(title, navigator)
            Text(
                "End-to-end encrypted · the key stays in this link",
                Modifier.fillMaxWidth().padding(horizontal = FmSpace.x4, vertical = FmSpace.x1),
                style = Fm.text.labelSmall,
                color = Fm.colors.textSecondary,
            )
            when {
                server.isEmpty() -> FmEmptyState("cloud_off", "No server", message = "Sign in to a ForkMesh server to join this meeting.")
                !asked -> Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) { FmCircularProgressIndicator() }
                else -> {
                    if (!mediaPermitted(context)) {
                        Column(Modifier.fillMaxWidth().padding(FmSpace.x3), verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                            Text(
                                "Allow the camera and microphone so others can see and hear you.",
                                style = Fm.text.bodySmall,
                                color = Fm.colors.textSecondary,
                            )
                            FmToneButton("Allow", onPressed = { ask.launch(MEDIA_PERMISSIONS) }, tone = FmTone.Accent)
                        }
                    }
                    AndroidView(
                        factory = { host -> page(host, server) { pick.launch("image/*") } },
                        modifier = Modifier.fillMaxSize(),
                    )
                }
            }
        }
    }

    @SuppressLint("SetJavaScriptEnabled")
    private fun page(context: Context, server: String, choose: () -> Unit): WebView {
        webView?.let { return it }
        val main = Handler(Looper.getMainLooper())
        return WebView(context).apply {
            settings.javaScriptEnabled = true
            settings.domStorageEnabled = true
            settings.mediaPlaybackRequiresUserGesture = false
            settings.allowFileAccess = false
            settings.allowContentAccess = false
            setBackgroundColor(Color.BLACK)
            addJavascriptInterface(object {
                @JavascriptInterface
                fun saveText(name: String?, text: String?) {
                    val body = text.orEmpty().take(MAX_TEXT)
                    if (body.isEmpty()) return
                    val subject = name.orEmpty().take(MAX_NAME).ifEmpty { "meeting-transcript.txt" }
                    main.post { if (webView != null && MeetingLink.isMeetingPage(url.orEmpty(), server)) shareText(context, subject, body) }
                }
            }, BRIDGE)
            webViewClient = object : WebViewClient() {
                override fun shouldOverrideUrlLoading(view: WebView, request: WebResourceRequest): Boolean {
                    val target = request.url.toString()
                    if (request.isForMainFrame && MeetingLink.isMeetingPage(target, server)) return false
                    if (target.startsWith("https://")) openInBrowser(context, target)
                    return true
                }
            }
            webChromeClient = object : WebChromeClient() {
                override fun onPermissionRequest(request: PermissionRequest) {
                    val granted = if (MeetingLink.sameOrigin(request.origin.toString(), server)) {
                        request.resources.filter { permitted(context, it) }.toTypedArray()
                    } else {
                        emptyArray()
                    }
                    if (granted.isEmpty()) request.deny() else request.grant(granted)
                }

                override fun onShowFileChooser(view: WebView, callback: ValueCallback<Array<Uri>>, params: FileChooserParams): Boolean {
                    pendingFiles?.onReceiveValue(null)
                    pendingFiles = callback
                    runCatching(choose).onFailure {
                        pendingFiles = null
                        callback.onReceiveValue(null)
                    }
                    return true
                }
            }
            loadUrl(MeetingLink.pageUrl(server, meeting))
        }.also { webView = it }
    }

    private fun permitted(context: Context, resource: String): Boolean = when (resource) {
        PermissionRequest.RESOURCE_AUDIO_CAPTURE -> context.checkSelfPermission(Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED
        PermissionRequest.RESOURCE_VIDEO_CAPTURE -> context.checkSelfPermission(Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED
        else -> false
    }

    private fun release() {
        pendingFiles?.onReceiveValue(null)
        pendingFiles = null
        val view = webView ?: return
        webView = null
        view.removeJavascriptInterface(BRIDGE)
        view.loadUrl("about:blank")
        view.destroy()
    }

    override fun onDisposed() {
        release()
    }
}

internal fun openMeeting(context: Context, url: String) {
    val route = MeetingLink.route(url)
    if (route != null) NavigationFeature.open(route) else runCatching {
        context.startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(url)).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK))
    }
}
