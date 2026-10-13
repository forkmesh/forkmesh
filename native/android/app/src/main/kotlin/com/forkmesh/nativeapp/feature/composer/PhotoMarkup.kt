package com.forkmesh.nativeapp.feature.composer

import android.app.Activity
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.Path
import android.graphics.RectF
import android.os.Handler
import android.os.Looper
import android.view.PixelCopy
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.awaitEachGesture
import androidx.compose.foundation.gestures.awaitFirstDown
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.systemBarsPadding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.graphics.drawscope.drawIntoCanvas
import androidx.compose.ui.graphics.nativeCanvas
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.layout.onSizeChanged
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.role
import androidx.compose.ui.semantics.selected
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.IntSize
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties
import com.forkmesh.nativeapp.agents.PromptAttachment
import com.forkmesh.nativeapp.agents.PromptAttachmentException
import com.forkmesh.nativeapp.agents.PromptAttachmentLimits
import com.forkmesh.nativeapp.composer.Markup
import com.forkmesh.nativeapp.composer.MarkupFit
import com.forkmesh.nativeapp.composer.MarkupStroke
import com.forkmesh.nativeapp.composer.MarkupTool
import com.forkmesh.nativeapp.composer.PhotoPrep
import com.forkmesh.nativeapp.ui.kit.FmCircularProgressIndicator
import com.forkmesh.nativeapp.ui.kit.FmInlineStatus
import com.forkmesh.nativeapp.ui.kit.FmMessenger
import com.forkmesh.nativeapp.ui.kit.FmSoundOnShow
import com.forkmesh.nativeapp.ui.kit.FmTextButton
import com.forkmesh.nativeapp.ui.kit.LocalFmMessenger
import com.forkmesh.nativeapp.ui.kit.fmTapSounds
import com.forkmesh.nativeapp.ui.kit.rememberFmStatus
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmSpace
import java.io.ByteArrayOutputStream
import kotlin.coroutines.resume
import kotlin.coroutines.resumeWithException
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlinx.coroutines.withContext

object ScreenCapture {
    suspend fun window(activity: Activity): Bitmap = suspendCancellableCoroutine { waiting ->
        val view = activity.window.decorView
        if (view.width <= 0 || view.height <= 0) {
            waiting.resumeWithException(PromptAttachmentException("This screen could not be captured."))
            return@suspendCancellableCoroutine
        }
        val bitmap = Bitmap.createBitmap(view.width, view.height, Bitmap.Config.ARGB_8888)
        PixelCopy.request(activity.window, bitmap, { result ->
            if (result == PixelCopy.SUCCESS) {
                waiting.resume(bitmap)
            } else {
                bitmap.recycle()
                waiting.resumeWithException(PromptAttachmentException("This screen could not be captured."))
            }
        }, Handler(Looper.getMainLooper()))
    }
}

object MarkupRender {
    fun draw(canvas: Canvas, strokes: List<MarkupStroke>, fit: MarkupFit) {
        val width = Markup.strokeWidth(fit.width, fit.height)
        val paint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            style = Paint.Style.STROKE
            strokeCap = Paint.Cap.ROUND
            strokeJoin = Paint.Join.ROUND
            strokeWidth = width
        }
        for (stroke in strokes) {
            if (stroke.points.isEmpty()) continue
            paint.color = stroke.color.toInt()
            val xs = stroke.points.map { fit.left + it.x * fit.width }
            val ys = stroke.points.map { fit.top + it.y * fit.height }
            val x0 = xs.first()
            val y0 = ys.first()
            val x1 = xs.last()
            val y1 = ys.last()
            when (stroke.tool) {
                MarkupTool.Pen -> if (xs.size == 1) {
                    canvas.drawPoint(x0, y0, paint)
                } else {
                    val path = Path()
                    path.moveTo(x0, y0)
                    for (i in 1 until xs.size) path.lineTo(xs[i], ys[i])
                    canvas.drawPath(path, paint)
                }
                MarkupTool.Line -> canvas.drawLine(x0, y0, x1, y1, paint)
                MarkupTool.Arrow -> {
                    canvas.drawLine(x0, y0, x1, y1, paint)
                    for ((hx, hy) in Markup.arrowHead(x0, y0, x1, y1, width)) canvas.drawLine(x1, y1, hx, hy, paint)
                }
                MarkupTool.Rect -> canvas.drawRect(minOf(x0, x1), minOf(y0, y1), maxOf(x0, x1), maxOf(y0, y1), paint)
                MarkupTool.Ellipse -> canvas.drawOval(RectF(minOf(x0, x1), minOf(y0, y1), maxOf(x0, x1), maxOf(y0, y1)), paint)
            }
        }
    }

    fun flatten(source: Bitmap, strokes: List<MarkupStroke>, name: String): PromptAttachment {
        val size = PhotoPrep.scaledSize(source.width, source.height)
        val out = Bitmap.createBitmap(size.width, size.height, Bitmap.Config.ARGB_8888)
        val canvas = Canvas(out)
        canvas.drawColor(android.graphics.Color.WHITE)
        canvas.drawBitmap(
            source,
            null,
            RectF(0f, 0f, size.width.toFloat(), size.height.toFloat()),
            Paint(Paint.FILTER_BITMAP_FLAG or Paint.ANTI_ALIAS_FLAG),
        )
        draw(canvas, strokes, MarkupFit(0f, 0f, size.width.toFloat(), size.height.toFloat()))
        val bytes = ByteArrayOutputStream()
        out.compress(Bitmap.CompressFormat.JPEG, PromptAttachmentLimits.JPEG_QUALITY, bytes)
        out.recycle()
        return PhotoPrep.attachment(bytes.toByteArray(), name, size.width, size.height)
    }

    fun decode(attachment: PromptAttachment): Bitmap? =
        BitmapFactory.decodeByteArray(attachment.bytes, 0, attachment.bytes.size)
}

suspend fun FmMessenger.markUp(source: Bitmap, name: String, confirmLabel: String): PromptAttachment? =
    showDialog { done -> MarkupEditor(source, name, confirmLabel, done) }

private val toolIcons = listOf(
    MarkupTool.Pen to ("draw" to "Pen"),
    MarkupTool.Line to ("horizontal_rule" to "Line"),
    MarkupTool.Arrow to ("north_east" to "Arrow"),
    MarkupTool.Rect to ("crop_square" to "Rectangle"),
    MarkupTool.Ellipse to ("circle_outlined" to "Ellipse"),
)

private val colorNames = listOf("Red", "Orange", "Yellow", "Green", "Blue", "Purple", "Black", "White")

@Composable
private fun MarkupEditor(source: Bitmap, name: String, confirmLabel: String, done: (PromptAttachment?) -> Unit) {
    val messenger = LocalFmMessenger.current
    val scope = rememberCoroutineScope()
    val image = remember(source) { source.asImageBitmap() }
    var strokes by remember { mutableStateOf(listOf<MarkupStroke>()) }
    var current by remember { mutableStateOf<MarkupStroke?>(null) }
    var tool by remember { mutableStateOf(MarkupTool.Pen) }
    var color by remember { mutableStateOf(Markup.DEFAULT_COLOR) }
    var box by remember { mutableStateOf(IntSize.Zero) }
    var saving by remember { mutableStateOf(false) }
    val status = rememberFmStatus()
    val fit = Markup.fit(source.width, source.height, box.width.toFloat(), box.height.toFloat())

    fun discard() {
        if (saving) return
        if (strokes.isEmpty()) {
            done(null)
            return
        }
        scope.launch {
            if (messenger.showConfirmDialog("Discard this markup?", confirmLabel = "Discard", destructive = true)) done(null)
        }
    }

    fun accept() {
        if (saving) return
        saving = true
        status.clear()
        scope.launch {
            try {
                done(withContext(Dispatchers.Default) { MarkupRender.flatten(source, strokes, name) })
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (error: Exception) {
                status.error(error.message ?: "The markup could not be saved.")
                saving = false
            }
        }
    }

    FmSoundOnShow()
    Dialog(
        onDismissRequest = ::discard,
        properties = DialogProperties(usePlatformDefaultWidth = false, dismissOnClickOutside = false, decorFitsSystemWindows = false),
    ) {
        Column(Modifier.fmTapSounds().fillMaxSize().background(Color(0xFF111111)).systemBarsPadding()) {
            Row(Modifier.fillMaxWidth().padding(horizontal = FmSpace.x2), verticalAlignment = Alignment.CenterVertically) {
                FmTextButton(onClick = ::discard, contentColor = Color.White) { Text("Discard") }
                Spacer(Modifier.weight(1f))
                Box(
                    Modifier
                        .size(44.dp)
                        .clip(CircleShape)
                        .then(if (strokes.isNotEmpty() && !saving) Modifier.clickable { strokes = strokes.dropLast(1) } else Modifier)
                        .semantics {
                            role = Role.Button
                            contentDescription = "Undo"
                        },
                    contentAlignment = Alignment.Center,
                ) {
                    FmIcon("undo", size = 22.dp, tint = if (strokes.isEmpty()) Color.White.copy(alpha = 0.35f) else Color.White)
                }
                if (saving) {
                    Box(Modifier.size(44.dp), contentAlignment = Alignment.Center) {
                        FmCircularProgressIndicator(Modifier.size(18.dp), strokeWidth = 2.dp)
                    }
                } else {
                    FmTextButton(onClick = ::accept, contentColor = Fm.colors.accent) { Text(confirmLabel) }
                }
            }
            FmInlineStatus(status, Modifier.padding(horizontal = FmSpace.x3))
            Box(
                Modifier
                    .weight(1f)
                    .fillMaxWidth()
                    .padding(FmSpace.x2)
                    .onSizeChanged { box = it },
            ) {
                Canvas(
                    Modifier
                        .fillMaxSize()
                        .semantics { contentDescription = "Drawing area" }
                        .pointerInput(tool, color, fit, saving) {
                            if (saving) return@pointerInput
                            awaitEachGesture {
                                val down = awaitFirstDown()
                                down.consume()
                                val start = Markup.normalize(down.position.x, down.position.y, fit)
                                var stroke = MarkupStroke(tool, color, listOf(start))
                                current = stroke
                                while (true) {
                                    val event = awaitPointerEvent()
                                    val change = event.changes.firstOrNull { it.id == down.id } ?: break
                                    if (!change.pressed) break
                                    change.consume()
                                    stroke = Markup.extend(stroke, Markup.normalize(change.position.x, change.position.y, fit))
                                    current = stroke
                                }
                                current = null
                                if (Markup.isVisible(stroke)) strokes = strokes + stroke
                            }
                        },
                ) {
                    drawImage(
                        image,
                        dstOffset = IntOffset(fit.left.toInt(), fit.top.toInt()),
                        dstSize = IntSize(fit.width.toInt(), fit.height.toInt()),
                    )
                    drawIntoCanvas { canvas -> MarkupRender.draw(canvas.nativeCanvas, strokes + listOfNotNull(current), fit) }
                }
            }
            Row(
                Modifier.fillMaxWidth().padding(horizontal = FmSpace.x3, vertical = FmSpace.x1),
                horizontalArrangement = Arrangement.Center,
            ) {
                for ((value, face) in toolIcons) {
                    val picked = value == tool
                    Box(
                        Modifier
                            .size(48.dp)
                            .clip(CircleShape)
                            .background(if (picked) Color.White.copy(alpha = 0.18f) else Color.Transparent)
                            .clickable { tool = value }
                            .semantics {
                                role = Role.RadioButton
                                selected = picked
                                contentDescription = face.second
                            },
                        contentAlignment = Alignment.Center,
                    ) {
                        FmIcon(face.first, size = 24.dp, tint = Color.White)
                    }
                }
            }
            Row(
                Modifier
                    .fillMaxWidth()
                    .horizontalScroll(rememberScrollState())
                    .padding(start = FmSpace.x3, end = FmSpace.x3, top = FmSpace.x1, bottom = FmSpace.x3),
                horizontalArrangement = Arrangement.Center,
            ) {
                Markup.COLORS.forEachIndexed { index, value ->
                    val picked = value == color
                    Box(
                        Modifier
                            .size(44.dp)
                            .clickable { color = value }
                            .semantics {
                                role = Role.RadioButton
                                selected = picked
                                contentDescription = colorNames.getOrElse(index) { "Color" }
                            },
                        contentAlignment = Alignment.Center,
                    ) {
                        Box(
                            Modifier
                                .size(if (picked) 30.dp else 26.dp)
                                .clip(CircleShape)
                                .background(Color(value))
                                .border(if (picked) 3.dp else 1.dp, Color.White.copy(alpha = if (picked) 1f else 0.4f), CircleShape),
                        )
                    }
                    if (index < Markup.COLORS.lastIndex) Spacer(Modifier.width(FmSpace.x1))
                }
            }
        }
    }
}
