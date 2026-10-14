package com.forkmesh.nativeapp.feature.facechanger

import android.Manifest
import android.annotation.SuppressLint
import android.content.Context
import android.content.pm.PackageManager
import android.graphics.Bitmap
import android.net.Uri
import android.view.ViewGroup
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
import androidx.compose.foundation.layout.Row
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
import androidx.compose.ui.unit.dp
import androidx.compose.ui.viewinterop.AndroidView
import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.navigation.NavigationFeature
import com.forkmesh.nativeapp.feature.tools.ToolsAppBar
import com.forkmesh.nativeapp.market.FaceChangerPage
import com.forkmesh.nativeapp.ui.kit.FmCircularProgressIndicator
import com.forkmesh.nativeapp.ui.kit.FmEmptyState
import com.forkmesh.nativeapp.ui.kit.FmInlineStatus
import com.forkmesh.nativeapp.ui.kit.FmStatus
import com.forkmesh.nativeapp.ui.kit.FmTone
import com.forkmesh.nativeapp.ui.kit.FmToneButton
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm

private val MEDIA_PERMISSIONS = arrayOf(Manifest.permission.CAMERA, Manifest.permission.RECORD_AUDIO)

private fun granted(context: Context, permission: String): Boolean =
    context.checkSelfPermission(permission) == PackageManager.PERMISSION_GRANTED

private fun mediaPermitted(context: Context): Boolean = MEDIA_PERMISSIONS.all { granted(context, it) }

class FaceChangerScreen : Screen() {
    override val title: String get() = "Face Changer"

    private val status = FmStatus()

    @Composable
    override fun Content(navigator: Navigator) {
        val context = LocalContext.current
        val origin = remember { Services.get<ApiService>()?.gitOrigin?.takeIf { FaceChangerPage.host(it) != null } }
        var asked by remember { mutableStateOf(mediaPermitted(context)) }
        var permitted by remember { mutableStateOf(mediaPermitted(context)) }
        val ask = rememberLauncherForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) { _ ->
            asked = true
            permitted = mediaPermitted(context)
        }
        LaunchedEffect(Unit) { if (!permitted) ask.launch(MEDIA_PERMISSIONS) }
        Column(Modifier.fillMaxSize().background(Fm.colors.bgBase)) {
            ToolsAppBar("Face Changer", navigator)
            when {
                origin == null -> FmEmptyState("face", "Face Changer is not available", message = "Set a server in Settings to open it.")
                !asked -> Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) { FmCircularProgressIndicator() }
                else -> {
                    if (!permitted) {
                        Row(
                            Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 8.dp),
                            verticalAlignment = Alignment.CenterVertically,
                            horizontalArrangement = Arrangement.spacedBy(8.dp),
                        ) {
                            Text(
                                "Allow the camera and microphone to change your face and voice.",
                                Modifier.weight(1f),
                                style = Fm.text.bodySmall,
                                color = Fm.colors.textSecondary,
                            )
                            FmToneButton("Allow", onPressed = { ask.launch(MEDIA_PERMISSIONS) }, tone = FmTone.Accent)
                        }
                    }
                    Box(Modifier.padding(horizontal = 16.dp)) { FmInlineStatus(status) }
                    FaceChangerWeb(origin, status, Modifier.weight(1f).fillMaxWidth())
                }
            }
        }
    }
}

@SuppressLint("SetJavaScriptEnabled", "JavascriptInterface")
@Composable
private fun FaceChangerWeb(origin: String, status: FmStatus, modifier: Modifier) {
    val context = LocalContext.current
    val chooser = remember { mutableStateOf<ValueCallback<Array<Uri>>?>(null) }
    val pick = rememberLauncherForActivityResult(ActivityResultContracts.GetContent()) { uri: Uri? ->
        chooser.value?.onReceiveValue(uri?.let { arrayOf(it) })
        chooser.value = null
    }
    val share = remember(origin) { FaceChangerShare(context, origin) { status.error(it) } }
    val view = remember(origin) {
        WebView(context).apply {
            settings.javaScriptEnabled = true
            settings.domStorageEnabled = true
            settings.mediaPlaybackRequiresUserGesture = false
            settings.allowFileAccess = false
            settings.allowContentAccess = false
            addJavascriptInterface(share, FaceChangerShare.BRIDGE)
            webViewClient = object : WebViewClient() {
                override fun onPageStarted(view: WebView, url: String?, favicon: Bitmap?) {
                    share.pageUrl = url
                    super.onPageStarted(view, url, favicon)
                }

                override fun doUpdateVisitedHistory(view: WebView, url: String?, isReload: Boolean) {
                    share.pageUrl = url
                    super.doUpdateVisitedHistory(view, url, isReload)
                }

                override fun shouldOverrideUrlLoading(view: WebView, request: WebResourceRequest): Boolean {
                    val url = request.url.toString()
                    if (FaceChangerPage.staysInView(url, origin)) return false
                    if (request.isForMainFrame) NavigationFeature.openInBrowser(view.context, url)
                    return true
                }
            }
            webChromeClient = object : WebChromeClient() {
                override fun onPermissionRequest(request: PermissionRequest) {
                    val allowed = if (FaceChangerPage.isPageHost(request.origin?.toString(), origin)) {
                        request.resources.filter { resource ->
                            when (resource) {
                                PermissionRequest.RESOURCE_VIDEO_CAPTURE -> granted(context, Manifest.permission.CAMERA)
                                PermissionRequest.RESOURCE_AUDIO_CAPTURE -> granted(context, Manifest.permission.RECORD_AUDIO)
                                else -> false
                            }
                        }.toTypedArray()
                    } else {
                        emptyArray()
                    }
                    if (allowed.isEmpty()) request.deny() else request.grant(allowed)
                }

                override fun onShowFileChooser(view: WebView, callback: ValueCallback<Array<Uri>>, params: FileChooserParams): Boolean {
                    chooser.value?.onReceiveValue(null)
                    chooser.value = callback
                    if (runCatching { pick.launch("image/*") }.isFailure) {
                        chooser.value = null
                        callback.onReceiveValue(null)
                    }
                    return true
                }
            }
            loadUrl(FaceChangerPage.url(origin))
        }
    }
    DisposableEffect(view) {
        onDispose {
            chooser.value?.onReceiveValue(null)
            chooser.value = null
            share.close()
            view.stopLoading()
            view.removeJavascriptInterface(FaceChangerShare.BRIDGE)
            view.loadUrl("about:blank")
            (view.parent as? ViewGroup)?.removeView(view)
            view.destroy()
        }
    }
    AndroidView(factory = { view }, modifier = modifier)
}
