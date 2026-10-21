package com.forkmesh.nativeapp.feature.agents

import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.scaleIn
import androidx.compose.animation.scaleOut
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.interaction.collectIsDraggedAsState
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxScope
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.LazyListScope
import androidx.compose.foundation.lazy.LazyListState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.runtime.snapshotFlow
import androidx.compose.runtime.withFrameNanos
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.role
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.ui.kit.fmGlass
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmSpace
import kotlinx.coroutines.launch

fun LazyListScope.transcriptEnd() {
    item(key = "transcript-end") { Spacer(Modifier.height(1.dp)) }
}

@Composable
fun TranscriptFollowBox(
    list: LazyListState,
    tail: Any?,
    modifier: Modifier = Modifier,
    pin: Int = 0,
    tag: String = "transcript",
    content: @Composable BoxScope.() -> Unit,
) {
    var following by remember { mutableStateOf(true) }
    var fresh by remember { mutableStateOf(false) }
    val dragged by list.interactionSource.collectIsDraggedAsState()
    val scope = rememberCoroutineScope()

    LaunchedEffect(list) {
        snapshotFlow { list.canScrollForward to dragged }.collect { (more, touching) ->
            if (!more) {
                following = true
                fresh = false
            } else if (touching) {
                following = false
            }
        }
    }
    LaunchedEffect(list) {
        snapshotFlow { list.layoutInfo.viewportSize.height }.collect {
            if (following) list.settleAtEnd(animated = false)
        }
    }
    LaunchedEffect(tail) {
        if (following) list.settleAtEnd(animated = false) else fresh = true
    }
    LaunchedEffect(pin) {
        if (pin == 0) return@LaunchedEffect
        following = true
        fresh = false
        list.settleAtEnd(animated = false)
    }

    Box(modifier) {
        content()
        AnimatedVisibility(
            visible = !following,
            modifier = Modifier.align(Alignment.BottomCenter).padding(bottom = FmSpace.x3),
            enter = fadeIn() + scaleIn(initialScale = 0.7f),
            exit = fadeOut() + scaleOut(targetScale = 0.7f),
        ) {
            TranscriptJumpButton(fresh, "$tag-jump-latest") {
                following = true
                fresh = false
                scope.launch { list.settleAtEnd(animated = true) }
            }
        }
    }
}

private suspend fun LazyListState.settleAtEnd(animated: Boolean) {
    withFrameNanos { }
    val last = layoutInfo.totalItemsCount - 1
    if (last < 0) return
    if (animated) animateScrollToItem(last) else scrollToItem(last)
}

@Composable
private fun TranscriptJumpButton(fresh: Boolean, tag: String, onClick: () -> Unit) {
    val c = Fm.colors
    val label = if (fresh) "New activity below. Jump to latest" else "Jump to latest"
    Box(
        Modifier
            .size(40.dp)
            .fmGlass(CircleShape, interactive = true)
            .clickable(onClick = onClick)
            .semantics {
                role = Role.Button
                contentDescription = label
            }
            .testTag(tag),
        contentAlignment = Alignment.Center,
    ) {
        FmIcon("arrow_downward", size = 20.dp, tint = c.textPrimary)
        if (fresh) Box(Modifier.align(Alignment.TopEnd).padding(6.dp).size(8.dp).background(c.accent, CircleShape))
    }
}
