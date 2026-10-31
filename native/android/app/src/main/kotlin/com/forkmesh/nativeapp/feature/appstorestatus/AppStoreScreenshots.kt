package com.forkmesh.nativeapp.feature.appstorestatus

import android.graphics.BitmapFactory
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.gestures.detectTransformGestures
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties
import com.forkmesh.nativeapp.feature.market.MarketFeature
import com.forkmesh.nativeapp.market.AppStoreFeedback
import com.forkmesh.nativeapp.market.AppStoreScreenshot
import com.forkmesh.nativeapp.market.AppStoreStatus
import com.forkmesh.nativeapp.market.PackagesException
import com.forkmesh.nativeapp.ui.kit.FmCircularProgressIndicator
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmInlineStatusLine
import com.forkmesh.nativeapp.ui.kit.FmSoundOnShow
import com.forkmesh.nativeapp.ui.kit.fmTapSounds
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmRadius
import com.forkmesh.nativeapp.ui.theme.FmSpace
import java.util.concurrent.ConcurrentHashMap
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

private object ScreenshotCache {
    private const val MAX_ENTRIES = 24
    private val bytes = ConcurrentHashMap<String, ByteArray>()

    fun fetch(shot: AppStoreScreenshot): ByteArray {
        bytes[shot.id]?.let { return it }
        val client = MarketFeature.client() ?: throw PackagesException("offline")
        val data = AppStoreStatus.loadScreenshot(client, shot)
        if (bytes.size >= MAX_ENTRIES) bytes.keys.firstOrNull()?.let { bytes.remove(it) }
        bytes[shot.id] = data
        return data
    }
}

private fun decode(data: ByteArray, maxSide: Int): ImageBitmap? {
    val bounds = BitmapFactory.Options().apply { inJustDecodeBounds = true }
    BitmapFactory.decodeByteArray(data, 0, data.size, bounds)
    var sample = 1
    while (bounds.outWidth / (sample * 2) >= maxSide || bounds.outHeight / (sample * 2) >= maxSide) sample *= 2
    return BitmapFactory.decodeByteArray(data, 0, data.size, BitmapFactory.Options().apply { inSampleSize = sample })?.asImageBitmap()
}

private fun describe(error: Throwable): String = when (error) {
    is AppStoreStatus.ScreenshotMissingException -> AppStoreStatus.SCREENSHOT_MISSING
    else -> PackagesException.describe(error)
}

@Composable
internal fun ScreenshotStrip(feedback: AppStoreFeedback) {
    val shots = feedback.screenshots
    if (shots.isEmpty()) return
    var opened by remember(feedback) { mutableStateOf<Int?>(null) }
    Row(
        Modifier.horizontalScroll(rememberScrollState()).padding(top = FmSpace.x1),
        horizontalArrangement = Arrangement.spacedBy(FmSpace.x2),
    ) {
        shots.forEachIndexed { index, shot ->
            ScreenshotThumbnail(shot, AppStoreStatus.screenshotTitle(index, shots.size)) { opened = index }
        }
    }
    opened?.let { start -> ScreenshotViewer(feedback, start) { opened = null } }
}

@Composable
private fun ScreenshotThumbnail(shot: AppStoreScreenshot, title: String, onOpen: () -> Unit) {
    val c = Fm.colors
    var image by remember(shot.id) { mutableStateOf<ImageBitmap?>(null) }
    var failed by remember(shot.id) { mutableStateOf(false) }
    LaunchedEffect(shot.id) {
        try {
            image = withContext(Dispatchers.IO) { decode(ScreenshotCache.fetch(shot), 320) }
            failed = image == null
        } catch (cancelled: CancellationException) {
            throw cancelled
        } catch (_: Exception) {
            failed = true
        }
    }
    val shape = RoundedCornerShape(FmRadius.md)
    Box(
        Modifier
            .size(width = 64.dp, height = 112.dp)
            .clip(shape)
            .background(c.bgOverlay)
            .border(1.dp, c.border, shape)
            .clickable(onClick = onOpen)
            .semantics { contentDescription = "$title. Opens full screen." },
        contentAlignment = Alignment.Center,
    ) {
        val current = image
        when {
            current != null -> Image(current, null, Modifier.fillMaxSize(), contentScale = ContentScale.Crop)
            failed -> FmIcon("broken_image_outlined", size = 22.dp, tint = c.textTertiary)
            else -> FmCircularProgressIndicator(Modifier.size(18.dp), strokeWidth = 2.dp)
        }
    }
}

@Composable
private fun ScreenshotViewer(feedback: AppStoreFeedback, start: Int, onClose: () -> Unit) {
    val shots = feedback.screenshots
    var index by remember { mutableIntStateOf(start.coerceIn(0, shots.size - 1)) }
    val shot = shots[index]
    var image by remember(shot.id) { mutableStateOf<ImageBitmap?>(null) }
    var failure by remember(shot.id) { mutableStateOf<String?>(null) }
    var scale by remember(shot.id) { mutableFloatStateOf(1f) }
    var offset by remember(shot.id) { mutableStateOf(Offset.Zero) }
    LaunchedEffect(shot.id) {
        try {
            image = withContext(Dispatchers.IO) { decode(ScreenshotCache.fetch(shot), 2048) }
            if (image == null) failure = "This screenshot could not be shown."
        } catch (cancelled: CancellationException) {
            throw cancelled
        } catch (error: Exception) {
            failure = describe(error)
        }
    }
    FmSoundOnShow()
    Dialog(onDismissRequest = onClose, properties = DialogProperties(usePlatformDefaultWidth = false)) {
        Box(
            Modifier
                .fmTapSounds()
                .fillMaxSize()
                .background(Color.Black),
            contentAlignment = Alignment.Center,
        ) {
            val current = image
            val failed = failure
            when {
                current != null -> Image(
                    current,
                    AppStoreStatus.screenshotTitle(index, shots.size),
                    Modifier
                        .fillMaxSize()
                        .pointerInput(shot.id) {
                            detectTransformGestures { _, pan, zoom, _ ->
                                scale = (scale * zoom).coerceIn(1f, 6f)
                                offset = if (scale == 1f) Offset.Zero else offset + pan
                            }
                        }
                        .pointerInput(shot.id) {
                            detectTapGestures(onDoubleTap = {
                                scale = if (scale > 1f) 1f else 2.5f
                                offset = Offset.Zero
                            })
                        }
                        .graphicsLayer {
                            scaleX = scale
                            scaleY = scale
                            translationX = offset.x
                            translationY = offset.y
                        },
                    contentScale = ContentScale.Fit,
                )
                failed != null -> FmInlineStatusLine(failed, isError = true, modifier = Modifier.padding(FmSpace.x5))
                else -> FmCircularProgressIndicator(Modifier.size(24.dp))
            }
            Row(
                Modifier.align(Alignment.TopCenter).fillMaxWidth().padding(FmSpace.x3),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                FmIconButton("close", "Close", onPressed = onClose, color = Color.White)
                Text(
                    AppStoreStatus.screenshotTitle(index, shots.size),
                    Modifier.weight(1f),
                    style = Fm.text.labelLarge.copy(color = Color.White),
                    textAlign = TextAlign.Center,
                )
                FmIconButton("chevron_left", "Previous screenshot", onPressed = if (index > 0) ({ index -= 1 }) else null, color = Color.White, busy = false)
                FmIconButton("chevron_right", "Next screenshot", onPressed = if (index < shots.size - 1) ({ index += 1 }) else null, color = Color.White, busy = false)
            }
            Column(Modifier.align(Alignment.BottomCenter).padding(FmSpace.x4), horizontalAlignment = Alignment.CenterHorizontally) {
                if (feedback.comment.isNotEmpty()) {
                    Text(feedback.comment, style = Fm.text.bodySmall.copy(color = Color.White), textAlign = TextAlign.Center, maxLines = 3)
                }
                Text(
                    listOf(feedback.from, feedback.device).filter { it.isNotEmpty() }.joinToString(" · "),
                    style = Fm.text.labelSmall.copy(color = Color.White.copy(alpha = .7f)),
                )
            }
        }
    }
}
