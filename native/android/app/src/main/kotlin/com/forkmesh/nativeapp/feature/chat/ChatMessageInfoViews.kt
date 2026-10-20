package com.forkmesh.nativeapp.feature.chat

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.awaitEachGesture
import androidx.compose.foundation.gestures.awaitFirstDown
import androidx.compose.foundation.gestures.horizontalDrag
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.composed
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.pointer.PointerInputChange
import androidx.compose.ui.input.pointer.PointerInputScope
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.input.pointer.positionChange
import androidx.compose.ui.input.pointer.util.VelocityTracker
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.role
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.forkmesh.nativeapp.chat.ChatInfoLine
import com.forkmesh.nativeapp.chat.ChatMessageInfo
import com.forkmesh.nativeapp.chat.ChatMessageInfoPage
import com.forkmesh.nativeapp.chat.ChatRow
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import kotlin.math.abs

internal enum class ChatSwipeHeading {
    LEFT,
    RIGHT;

    fun matches(dx: Float): Boolean = if (this == LEFT) dx < 0f else dx > 0f
}

private const val FLING_SECONDS = 0.25f

internal fun Modifier.sideSwipe(
    heading: ChatSwipeHeading,
    enabled: Boolean,
    moved: (Float) -> Unit,
    ended: (Float, Float) -> Unit,
): Modifier = composed {
    val onMoved by rememberUpdatedState(moved)
    val onEnded by rememberUpdatedState(ended)
    if (!enabled) Modifier else Modifier.pointerInput(heading) { swipe(heading, { onMoved(it) }, { dx, fling -> onEnded(dx, fling) }) }
}

private suspend fun PointerInputScope.swipe(heading: ChatSwipeHeading, moved: (Float) -> Unit, ended: (Float, Float) -> Unit) {
    val slop = viewConfiguration.touchSlop
    awaitEachGesture {
        val down = awaitFirstDown(requireUnconsumed = false)
        var dx = 0f
        var dy = 0f
        var dragging = false
        val tracker = VelocityTracker()
        tracker.addPosition(down.uptimeMillis, down.position)
        while (!dragging) {
            val event = awaitPointerEvent()
            val change = event.changes.firstOrNull { it.id == down.id } ?: return@awaitEachGesture
            if (!change.pressed) return@awaitEachGesture
            if (change.isConsumed) return@awaitEachGesture
            val delta = change.positionChange()
            dx += delta.x
            dy += delta.y
            tracker.addPosition(change.uptimeMillis, change.position)
            if (abs(dx) < slop && abs(dy) < slop) continue
            if (abs(dx) > abs(dy) * ChatMessageInfo.SIDEWAYS_RATIO && heading.matches(dx)) {
                dragging = true
                change.consume()
                moved(dx)
            } else {
                return@awaitEachGesture
            }
        }
        val finished = horizontalDrag(down.id) { change: PointerInputChange ->
            dx += change.positionChange().x
            tracker.addPosition(change.uptimeMillis, change.position)
            change.consume()
            moved(dx)
        }
        if (finished) {
            val velocity = tracker.calculateVelocity().x
            ended(dx, dx + velocity * FLING_SECONDS)
        } else {
            ended(0f, 0f)
        }
    }
}

private object ChatInfoColors {
    val blue = Color(0xFF0A84FF)

    fun page(dark: Boolean) = if (dark) Color.Black else Color(0xFFF2F2F7)

    fun card(dark: Boolean) = if (dark) Color(0xFF1C1C1E) else Color.White

    fun rule(dark: Boolean) = if (dark) Color.White.copy(alpha = 0.12f) else Color.Black.copy(alpha = 0.1f)

    fun ink(dark: Boolean) = if (dark) Color.White else Color.Black

    fun meta(dark: Boolean) = if (dark) Color(0xFF8E8E93) else Color(0xFF8A8A8E)

    fun received(dark: Boolean) = if (dark) Color(0xFF26262A) else Color(0xFFE9E9EB)
}

@Composable
internal fun ChatMessageInfoPageView(row: ChatRow, page: ChatMessageInfoPage, onClose: () -> Unit, modifier: Modifier = Modifier) {
    val dark = Fm.dark
    val ink = ChatInfoColors.ink(dark)
    val meta = ChatInfoColors.meta(dark)
    Column(
        modifier
            .fillMaxSize()
            .background(ChatInfoColors.page(dark))
            .semantics { contentDescription = "Message Info" },
    ) {
        Box(Modifier.fillMaxWidth().height(52.dp).padding(horizontal = 6.dp), contentAlignment = Alignment.Center) {
            Text("Message Info", style = Fm.text.titleSmall.copy(fontSize = 17.sp, fontWeight = FontWeight.SemiBold, color = ink))
            Box(
                Modifier
                    .align(Alignment.CenterStart)
                    .size(44.dp)
                    .clickable(onClick = onClose)
                    .semantics {
                        role = Role.Button
                        contentDescription = "Back"
                    },
                contentAlignment = Alignment.Center,
            ) {
                FmIcon("chevron_left", size = 28.dp, tint = ink)
            }
        }
        Column(
            Modifier
                .fillMaxSize()
                .verticalScroll(rememberScrollState())
                .padding(start = 16.dp, end = 16.dp, top = 8.dp, bottom = 32.dp),
            verticalArrangement = Arrangement.spacedBy(22.dp),
        ) {
            InfoPreview(row, dark)
            InfoSection(page.heading, page.timeline, dark)
            if (page.more.isNotEmpty()) InfoSection("More", page.more, dark)
            page.note?.let {
                Text(it, modifier = Modifier.padding(horizontal = 16.dp), style = Fm.text.bodySmall.copy(fontSize = 13.sp, color = meta))
            }
        }
    }
}

@Composable
private fun InfoPreview(row: ChatRow, dark: Boolean) {
    val mine = row.message.self
    Row(Modifier.fillMaxWidth().padding(top = 8.dp), horizontalArrangement = if (mine) Arrangement.End else Arrangement.Start) {
        if (mine) Spacer(Modifier.width(48.dp))
        Text(
            ChatMessageInfo.previewText(row),
            maxLines = 8,
            modifier = Modifier
                .widthIn(max = 320.dp)
                .background(if (mine) ChatInfoColors.blue else ChatInfoColors.received(dark), RoundedCornerShape(18.dp))
                .padding(start = if (mine) 13.dp else 18.dp, end = if (mine) 18.dp else 13.dp, top = 8.dp, bottom = 8.dp),
            style = Fm.text.bodyLarge.copy(fontSize = 17.sp, color = if (mine) Color.White else ChatInfoColors.ink(dark)),
        )
        if (!mine) Spacer(Modifier.width(48.dp))
    }
}

@Composable
private fun InfoSection(title: String, lines: List<ChatInfoLine>, dark: Boolean) {
    Column(verticalArrangement = Arrangement.spacedBy(6.dp)) {
        Text(
            title.uppercase(),
            modifier = Modifier.padding(start = 16.dp),
            style = Fm.text.labelSmall.copy(fontSize = 12.sp, fontWeight = FontWeight.Medium, color = ChatInfoColors.meta(dark)),
        )
        Column(Modifier.fillMaxWidth().background(ChatInfoColors.card(dark), RoundedCornerShape(12.dp))) {
            lines.forEachIndexed { index, line ->
                if (index > 0) Box(Modifier.padding(start = 48.dp).fillMaxWidth().height(0.5.dp).background(ChatInfoColors.rule(dark)))
                InfoLine(line, dark)
            }
        }
    }
}

@Composable
private fun InfoLine(line: ChatInfoLine, dark: Boolean) {
    val ink = ChatInfoColors.ink(dark)
    val meta = ChatInfoColors.meta(dark)
    Row(
        Modifier
            .fillMaxWidth()
            .heightIn(min = 50.dp)
            .padding(horizontal = 14.dp, vertical = 11.dp)
            .semantics(mergeDescendants = true) {},
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        Box(Modifier.width(22.dp), contentAlignment = Alignment.Center) {
            FmIcon(line.icon, size = 18.dp, tint = if (line.accent) ChatInfoColors.blue else meta)
        }
        Text(line.title, style = Fm.text.bodyLarge.copy(fontSize = 16.sp, color = ink))
        Spacer(Modifier.weight(1f).widthIn(min = 12.dp))
        Column(horizontalAlignment = Alignment.End, verticalArrangement = Arrangement.spacedBy(1.dp)) {
            Text(
                line.value,
                textAlign = TextAlign.End,
                style = Fm.text.bodyLarge.copy(fontSize = 16.sp, color = if (line.pending) meta else ink),
            )
            if (line.detail.isNotEmpty()) {
                Text(line.detail, style = Fm.text.bodySmall.copy(fontSize = 12.sp, fontFeatureSettings = "tnum", color = meta))
            }
        }
    }
}

@Composable
internal fun ChatReceiptLine(text: String, onTap: () -> Unit) {
    Box(Modifier.fillMaxWidth().padding(end = 14.dp, top = 3.dp), contentAlignment = Alignment.CenterEnd) {
        Text(
            text,
            maxLines = 1,
            modifier = Modifier
                .clickable(onClick = onTap)
                .semantics { contentDescription = "$text. Shows Message Info" },
            style = Fm.text.labelSmall.copy(
                fontSize = 11.sp,
                fontWeight = FontWeight.SemiBold,
                fontFeatureSettings = "tnum",
                color = ChatInfoColors.meta(Fm.dark),
            ),
        )
    }
}
