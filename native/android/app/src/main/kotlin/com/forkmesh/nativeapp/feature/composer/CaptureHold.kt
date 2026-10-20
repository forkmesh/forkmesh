package com.forkmesh.nativeapp.feature.composer

import android.app.Activity
import android.app.Application
import android.content.ClipData
import android.content.ClipboardManager
import android.graphics.Bitmap
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.view.HapticFeedbackConstants
import android.view.MotionEvent
import android.view.SurfaceView
import android.view.TextureView
import android.view.View
import android.view.ViewConfiguration
import android.view.ViewGroup
import android.view.Window
import android.webkit.WebView
import android.widget.TextView
import androidx.activity.compose.BackHandler
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.gestures.detectDragGestures
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawingPadding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Rect
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.BlendMode
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.CompositingStrategy
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.node.RootForTest
import androidx.compose.ui.platform.ComposeView
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.semantics.SemanticsActions
import androidx.compose.ui.semantics.SemanticsNode
import androidx.compose.ui.semantics.SemanticsProperties
import androidx.compose.ui.semantics.getOrNull
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.dp
import androidx.core.content.FileProvider
import com.forkmesh.nativeapp.App
import com.forkmesh.nativeapp.MainActivity
import com.forkmesh.nativeapp.composer.CaptureArea
import com.forkmesh.nativeapp.composer.CaptureAreaRules
import com.forkmesh.nativeapp.composer.CaptureHit
import com.forkmesh.nativeapp.feature.market.PackageGateFeature
import com.forkmesh.nativeapp.market.PhonePackages
import com.forkmesh.nativeapp.platform.AppearanceStore
import com.forkmesh.nativeapp.ui.kit.FmActionMenu
import com.forkmesh.nativeapp.ui.kit.FmActionMenuButton
import com.forkmesh.nativeapp.ui.kit.FmTextButton
import com.forkmesh.nativeapp.ui.kit.LocalFmMessenger
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.ForkMeshTheme
import java.io.File
import kotlin.math.abs
import kotlin.math.roundToInt
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

internal object CaptureHold {
    private const val SHOWN_MS = 1_600L
    private const val COPY_DIR = "capture-copy"

    private var installed = false

    fun install(app: App) {
        if (installed) return
        installed = true
        app.registerActivityLifecycleCallbacks(object : Application.ActivityLifecycleCallbacks {
            override fun onActivityCreated(activity: Activity, savedInstanceState: Bundle?) {}

            override fun onActivityStarted(activity: Activity) {}

            override fun onActivityResumed(activity: Activity) {
                if (activity !is MainActivity) return
                val window = activity.window
                val inner = window.callback ?: return
                if (inner !is HoldCallback) window.callback = HoldCallback(activity, inner)
            }

            override fun onActivityPaused(activity: Activity) {}

            override fun onActivityStopped(activity: Activity) {}

            override fun onActivitySaveInstanceState(activity: Activity, outState: Bundle) {}

            override fun onActivityDestroyed(activity: Activity) {}
        })
    }

    val enabled: Boolean get() = PackageGateFeature.deviceSwitch(PhonePackages.CAPTURE) != false

    private class HoldCallback(private val activity: Activity, private val inner: Window.Callback) : Window.Callback by inner {
        private val main = Handler(Looper.getMainLooper())
        private val slop = ViewConfiguration.get(activity).scaledTouchSlop
        private var downX = 0f
        private var downY = 0f
        private var downAt = 0L
        private var tracking = false
        private var swallowing = false
        private val fire = Runnable { fire() }

        override fun dispatchTouchEvent(event: MotionEvent): Boolean {
            if (swallowing) {
                if (event.actionMasked == MotionEvent.ACTION_UP || event.actionMasked == MotionEvent.ACTION_CANCEL) swallowing = false
                return true
            }
            when (event.actionMasked) {
                MotionEvent.ACTION_DOWN -> {
                    stop()
                    if (enabled && !CaptureOverlay.showing && isBlank(activity, event.x, event.y)) {
                        downX = event.x
                        downY = event.y
                        downAt = event.downTime
                        tracking = true
                        main.postDelayed(fire, CaptureAreaRules.HOLD_MS)
                    }
                }
                MotionEvent.ACTION_MOVE -> if (tracking && (abs(event.x - downX) > slop || abs(event.y - downY) > slop)) stop()
                else -> stop()
            }
            return inner.dispatchTouchEvent(event)
        }

        private fun stop() {
            tracking = false
            main.removeCallbacks(fire)
        }

        private fun fire() {
            if (!tracking) return
            tracking = false
            if (activity.isFinishing || !enabled || !isBlank(activity, downX, downY)) return
            val now = SystemClock.uptimeMillis()
            val cancel = MotionEvent.obtain(downAt, now, MotionEvent.ACTION_CANCEL, downX, downY, 0)
            inner.dispatchTouchEvent(cancel)
            cancel.recycle()
            swallowing = true
            activity.window.decorView.performHapticFeedback(HapticFeedbackConstants.LONG_PRESS)
            CaptureOverlay.show(activity, downX, downY)
        }
    }

    fun isBlank(activity: Activity, x: Float, y: Float): Boolean {
        val root = activity.window.decorView
        if (root.width <= 0 || root.height <= 0) return false
        val hits = ArrayList<CaptureHit>()
        var native = false
        val location = IntArray(2)
        fun visit(view: View) {
            if (!view.isShown) return
            view.getLocationInWindow(location)
            val inside = x >= location[0] && y >= location[1] && x < location[0] + view.width && y < location[1] + view.height
            if (!inside) return
            if (view is RootForTest) {
                collect(view.semanticsOwner.unmergedRootSemanticsNode, x, y, hits)
                return
            }
            if (view is WebView || view is TextView || view is SurfaceView || view is TextureView || view.isClickable || view.isLongClickable) {
                if (view !== root) native = true
            }
            if (view is ViewGroup) for (i in 0 until view.childCount) visit(view.getChildAt(i))
        }
        runCatching { visit(root) }.onFailure { return false }
        if (native) return false
        return CaptureAreaRules.isBlank(hits, root.width.toDouble(), root.height.toDouble())
    }

    private fun collect(node: SemanticsNode, x: Float, y: Float, out: MutableList<CaptureHit>) {
        val bounds = node.boundsInWindow
        if (!bounds.contains(Offset(x, y))) return
        val config = node.config
        val disabled = config.getOrNull(SemanticsProperties.Disabled) != null
        val actionable = !disabled && (
            config.getOrNull(SemanticsActions.OnClick) != null ||
                config.getOrNull(SemanticsActions.OnLongClick) != null ||
                config.getOrNull(SemanticsActions.SetText) != null
            )
        val labelled = config.getOrNull(SemanticsProperties.Text) != null || config.getOrNull(SemanticsProperties.ContentDescription) != null
        if (actionable || labelled) out += CaptureHit(bounds.width.toDouble(), bounds.height.toDouble(), actionable)
        for (child in node.children) collect(child, x, y, out)
    }

    fun copy(activity: Activity, bytes: ByteArray) {
        val dir = File(activity.cacheDir, COPY_DIR).apply { mkdirs() }
        dir.listFiles()?.filter { it.isFile }?.forEach { it.delete() }
        val file = File(dir, "capture-${System.currentTimeMillis()}.jpg")
        file.writeBytes(bytes)
        val uri = FileProvider.getUriForFile(activity, "${activity.packageName}.composer.files", file)
        val clip = ClipData.newUri(activity.contentResolver, "Screen area", uri)
        activity.getSystemService(ClipboardManager::class.java)?.setPrimaryClip(clip)
    }

    val shownMs: Long get() = SHOWN_MS
}

internal object CaptureOverlay {
    var showing = false
        private set

    private sealed class Step {
        data class Menu(val x: Float, val y: Float) : Step()
        data object Waiting : Step()
        data class Picking(val screen: Bitmap) : Step()
        data class Marking(val crop: Bitmap) : Step()
        data object Copied : Step()
    }

    fun show(activity: Activity, x: Float, y: Float) {
        val decor = activity.window.decorView as? ViewGroup ?: return
        showing = true
        val view = ComposeView(activity)
        fun close() {
            decor.removeView(view)
            showing = false
        }
        view.setContent {
            val appearance by AppearanceStore.appearance.collectAsState()
            ForkMeshTheme(dark = appearance.isDark(isSystemInDarkTheme()), theme = appearance.theme) {
                Flow(activity, x, y, ::close)
            }
        }
        decor.addView(view, ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))
    }

    @Composable
    private fun Flow(activity: Activity, x: Float, y: Float, close: () -> Unit) {
        val scope = rememberCoroutineScope()
        val messenger = LocalFmMessenger.current
        var step by remember { mutableStateOf<Step>(Step.Menu(x, y)) }

        fun capture(whole: Boolean) {
            step = Step.Waiting
            scope.launch {
                delay(CaptureAreaRules.DRAW_DELAY_MS)
                val screen = runCatching { ScreenCapture.window(activity) }.getOrNull()
                if (screen == null) {
                    close()
                    return@launch
                }
                step = if (whole) Step.Marking(screen) else Step.Picking(screen)
            }
        }

        when (val now = step) {
            is Step.Menu -> {
                BackHandler { close() }
                Box(Modifier.fillMaxSize()) {
                    Box(Modifier.offset { IntOffset(now.x.roundToInt(), now.y.roundToInt()) }.size(1.dp)) {
                        FmActionMenu(expanded = true, onDismissRequest = close) {
                            FmActionMenuButton("Select Area", onClick = { capture(whole = false) })
                            FmActionMenuButton("Whole Screen", onClick = { capture(whole = true) })
                        }
                    }
                }
            }
            Step.Waiting -> Box(Modifier.fillMaxSize())
            is Step.Picking -> AreaPicker(
                now.screen,
                onCancel = close,
                onPicked = { area ->
                    val box = CaptureAreaRules.pixels(area, 1.0, now.screen.width, now.screen.height)
                    val crop = Bitmap.createBitmap(now.screen, box.x.toInt(), box.y.toInt(), box.width.toInt(), box.height.toInt())
                    step = Step.Marking(crop)
                },
            )
            is Step.Marking -> {
                LaunchedEffect(now) {
                    val marked = messenger.markUp(now.crop, "capture.jpg", "Copy")
                    if (marked == null) {
                        close()
                        return@LaunchedEffect
                    }
                    withContext(Dispatchers.IO) { runCatching { CaptureHold.copy(activity, marked.bytes) } }
                    step = Step.Copied
                }
                Box(Modifier.fillMaxSize().background(Color.Black)) {
                    Image(now.crop.asImageBitmap(), null, Modifier.fillMaxSize(), contentScale = ContentScale.Fit)
                }
            }
            Step.Copied -> {
                LaunchedEffect(Unit) {
                    delay(CaptureHold.shownMs)
                    close()
                }
                Box(Modifier.fillMaxSize().safeDrawingPadding().padding(top = 12.dp), contentAlignment = Alignment.TopCenter) {
                    Text(
                        "Copied to clipboard",
                        Modifier
                            .background(Fm.colors.bgElevated, RoundedCornerShape(50))
                            .padding(horizontal = 16.dp, vertical = 10.dp),
                        style = Fm.text.labelLarge.copy(color = Fm.colors.textPrimary),
                    )
                }
            }
        }
    }

    @Composable
    private fun AreaPicker(screen: Bitmap, onCancel: () -> Unit, onPicked: (CaptureArea) -> Unit) {
        BackHandler { onCancel() }
        val density = LocalDensity.current
        val minimum = with(density) { CaptureAreaRules.MINIMUM_SIDE.dp.toPx() }.toDouble()
        val accent = Fm.colors.accent
        var start by remember { mutableStateOf<Offset?>(null) }
        var end by remember { mutableStateOf<Offset?>(null) }
        Box(Modifier.fillMaxSize()) {
            Image(screen.asImageBitmap(), null, Modifier.fillMaxSize(), contentScale = ContentScale.FillBounds)
            Canvas(
                Modifier
                    .fillMaxSize()
                    .graphicsLayer(compositingStrategy = CompositingStrategy.Offscreen)
                    .pointerInput(screen) {
                        detectDragGestures(
                            onDragStart = { at ->
                                start = at
                                end = at
                            },
                            onDrag = { change, _ -> end = change.position },
                            onDragEnd = {
                                val from = start
                                val to = end
                                start = null
                                end = null
                                if (from != null && to != null) {
                                    val area = CaptureAreaRules.area(
                                        from.x.toDouble(), from.y.toDouble(), to.x.toDouble(), to.y.toDouble(),
                                        size.width.toDouble(), size.height.toDouble(),
                                    )
                                    if (area.width >= minimum && area.height >= minimum) {
                                        val sx = screen.width / size.width.toDouble()
                                        val sy = screen.height / size.height.toDouble()
                                        onPicked(CaptureArea(area.x * sx, area.y * sy, area.width * sx, area.height * sy))
                                    }
                                }
                            },
                            onDragCancel = {
                                start = null
                                end = null
                            },
                        )
                    },
            ) {
                drawRect(Color.Black.copy(alpha = 0.45f))
                val from = start
                val to = end
                if (from != null && to != null) {
                    val box = Rect(minOf(from.x, to.x), minOf(from.y, to.y), maxOf(from.x, to.x), maxOf(from.y, to.y))
                    drawRect(Color.Transparent, box.topLeft, Size(box.width, box.height), blendMode = BlendMode.Clear)
                    drawRect(accent, box.topLeft, Size(box.width, box.height), style = Stroke(2.dp.toPx()))
                }
            }
            Box(Modifier.fillMaxSize().safeDrawingPadding().padding(12.dp), contentAlignment = Alignment.TopStart) {
                FmTextButton(onClick = onCancel, contentColor = Color.White) { Text("Cancel") }
            }
            Box(Modifier.fillMaxSize().safeDrawingPadding().padding(top = 18.dp), contentAlignment = Alignment.TopCenter) {
                Text("Drag over the part to copy", style = Fm.text.labelLarge.copy(color = Color.White))
            }
        }
    }
}
