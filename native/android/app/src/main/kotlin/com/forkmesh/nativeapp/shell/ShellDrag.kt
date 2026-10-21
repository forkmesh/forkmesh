package com.forkmesh.nativeapp.shell

import androidx.compose.foundation.gestures.awaitEachGesture
import androidx.compose.foundation.gestures.awaitFirstDown
import androidx.compose.ui.input.pointer.PointerEventPass
import kotlinx.coroutines.withTimeoutOrNull
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateMapOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Rect
import androidx.compose.ui.geometry.isSpecified
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.layout.boundsInRoot
import androidx.compose.ui.layout.onGloballyPositioned
import androidx.compose.ui.layout.positionInRoot
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.ui.kit.fmGlass
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import kotlin.math.roundToInt

internal object ShellDrag {
    var id by mutableStateOf<String?>(null)
        private set
    var fromBar by mutableStateOf(false)
        private set
    var icon by mutableStateOf("")
        private set
    var pointer by mutableStateOf(Offset.Unspecified)
        private set
    var barArea by mutableStateOf(Rect.Zero)
    val barSlots = mutableStateMapOf<String, Rect>()

    val overBar: Boolean get() = pointer.isSpecified && barArea.width > 0f && barArea.inflate(24f).contains(pointer)

    fun barSlotAt(at: Offset): String? = barSlots.entries.firstOrNull { it.value.contains(at) }?.key

    fun isDragging(itemId: String, bar: Boolean): Boolean = id == itemId && fromBar == bar

    internal fun start(itemId: String, glyph: String, bar: Boolean, at: Offset) {
        id = itemId
        icon = glyph
        fromBar = bar
        pointer = at
    }

    internal fun moveTo(at: Offset) {
        pointer = at
    }

    internal fun end() {
        id = null
        pointer = Offset.Unspecified
    }
}

@Composable
internal fun Modifier.shellDragSource(
    itemId: String,
    icon: String,
    fromBar: Boolean,
    enabled: Boolean,
    onStart: () -> Unit,
    onMove: (Offset) -> Unit = {},
    onDrop: (Offset) -> Unit,
): Modifier {
    if (!enabled) return this
    val origin = remember { arrayOf(Offset.Zero) }
    val start by rememberUpdatedState(onStart)
    val move by rememberUpdatedState(onMove)
    val drop by rememberUpdatedState(onDrop)
    return this
        .onGloballyPositioned { origin[0] = it.positionInRoot() }
        .pointerInput(itemId, fromBar) {
            awaitEachGesture {
                val down = awaitFirstDown(requireUnconsumed = false, pass = PointerEventPass.Initial)
                val early = withTimeoutOrNull(viewConfiguration.longPressTimeoutMillis) {
                    while (true) {
                        val change = awaitPointerEvent(PointerEventPass.Initial).changes.firstOrNull { it.id == down.id } ?: return@withTimeoutOrNull true
                        if (!change.pressed || (change.position - down.position).getDistance() > viewConfiguration.touchSlop) return@withTimeoutOrNull true
                    }
                    @Suppress("UNREACHABLE_CODE")
                    true
                }
                if (early != null) return@awaitEachGesture
                var dragging = false
                try {
                    while (true) {
                        val change = awaitPointerEvent(PointerEventPass.Initial).changes.firstOrNull { it.id == down.id }
                        if (change == null || !change.pressed) {
                            if (dragging) {
                                change?.consume()
                                drop(ShellDrag.pointer)
                            }
                            break
                        }
                        if (!dragging && (change.position - down.position).getDistance() > viewConfiguration.touchSlop) {
                            dragging = true
                            ShellDrag.start(itemId, icon, fromBar, origin[0] + change.position)
                            start()
                        }
                        if (dragging) {
                            change.consume()
                            ShellDrag.moveTo(origin[0] + change.position)
                            move(ShellDrag.pointer)
                        }
                    }
                } finally {
                    if (dragging) ShellDrag.end()
                }
            }
        }
}

internal fun Modifier.shellBarDropSlot(slotId: String): Modifier =
    onGloballyPositioned { ShellDrag.barSlots[slotId] = it.boundsInRoot() }

@Composable
internal fun ShellDragPreview() {
    val at = ShellDrag.pointer
    if (ShellDrag.id == null || !at.isSpecified) return
    val origin = remember { arrayOf(Offset.Zero) }
    Box(Modifier.fillMaxSize().onGloballyPositioned { origin[0] = it.positionInRoot() }, contentAlignment = Alignment.TopStart) {
        val side = 60.dp
        Box(
            Modifier
                .offset {
                    val half = side.toPx() / 2
                    IntOffset((ShellDrag.pointer.x - origin[0].x - half).roundToInt(), (ShellDrag.pointer.y - origin[0].y - half * 1.6f).roundToInt())
                }
                .size(side)
                .graphicsLayer {
                    scaleX = 1.08f
                    scaleY = 1.08f
                }
                .fmGlass(RoundedCornerShape(18.dp)),
            contentAlignment = Alignment.Center,
        ) {
            FmIcon(ShellDrag.icon, size = 28.dp, tint = if (ShellDrag.overBar) Fm.colors.accent else Fm.colors.textPrimary)
        }
    }
}
