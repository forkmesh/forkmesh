package com.forkmesh.nativeapp.ui.kit

import androidx.compose.animation.core.Animatable
import androidx.compose.animation.core.spring
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.navigationBars
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.role
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmHaptics
import com.forkmesh.nativeapp.ui.theme.FmSpace
import kotlinx.coroutines.launch

data class FmSheetAction(val label: String, val destructive: Boolean = false)

object FmActionSheetMetrics {
    val corner = 14.dp
    val row = 57.dp
    val gap = 8.dp
    val margin = 8.dp
}

suspend fun FmMessenger.showActionSheet(
    title: String? = null,
    message: String? = null,
    actions: List<FmSheetAction>,
    cancelLabel: String = "Cancel",
): Int? = showDialog { done -> FmActionSheet(title, message, actions, cancelLabel, onPick = { done(it) }, onDismiss = { done(null) }) }

suspend fun FmMessenger.confirmAction(title: String, label: String, message: String? = null, destructive: Boolean = true): Boolean =
    showActionSheet(title, message, listOf(FmSheetAction(label, destructive))) == 0

@Composable
fun FmActionSheet(
    title: String?,
    message: String?,
    actions: List<FmSheetAction>,
    cancelLabel: String = "Cancel",
    onPick: (Int) -> Unit,
    onDismiss: () -> Unit,
) {
    val c = Fm.colors
    val haptics = FmHaptics.current
    val rise = remember { Animatable(1f) }
    val scope = rememberCoroutineScope()
    LaunchedEffect(Unit) { rise.animateTo(0f, spring(dampingRatio = 0.86f, stiffness = 500f)) }
    fun leave(then: () -> Unit) {
        scope.launch {
            rise.animateTo(1f, spring(stiffness = 900f))
            then()
        }
    }
    val surface = if (c.dark) Color(0xFF2C2C2E) else Color(0xFFF2F2F7)
    val hairline = c.textPrimary.copy(alpha = 0.12f)
    Dialog(onDismissRequest = { leave(onDismiss) }, properties = DialogProperties(usePlatformDefaultWidth = false, decorFitsSystemWindows = false)) {
        Box(
            Modifier
                .fillMaxSize()
                .clickable(interactionSource = remember { MutableInteractionSource() }, indication = null) { leave(onDismiss) },
            contentAlignment = Alignment.BottomCenter,
        ) {
            Column(
                Modifier
                    .widthIn(max = 480.dp)
                    .fillMaxWidth()
                    .windowInsetsPadding(WindowInsets.navigationBars)
                    .padding(FmActionSheetMetrics.margin)
                    .graphicsLayer { translationY = rise.value * size.height },
                verticalArrangement = Arrangement.spacedBy(FmActionSheetMetrics.gap),
            ) {
                Column(
                    Modifier
                        .fillMaxWidth()
                        .background(surface, RoundedCornerShape(FmActionSheetMetrics.corner))
                        .clickable(interactionSource = remember { MutableInteractionSource() }, indication = null) {},
                ) {
                    if (title != null || message != null) {
                        Column(
                            Modifier.fillMaxWidth().padding(horizontal = FmSpace.x4, vertical = 14.dp),
                            horizontalAlignment = Alignment.CenterHorizontally,
                            verticalArrangement = Arrangement.spacedBy(4.dp),
                        ) {
                            if (title != null) Text(title, fontSize = 13.sp, fontWeight = FontWeight.SemiBold, color = c.textSecondary, textAlign = TextAlign.Center)
                            if (message != null) Text(message, fontSize = 13.sp, color = c.textSecondary, textAlign = TextAlign.Center)
                        }
                    }
                    actions.forEachIndexed { index, action ->
                        if (index > 0 || title != null || message != null) Spacer(Modifier.fillMaxWidth().height(0.5.dp).background(hairline))
                        SheetRow(action.label, if (action.destructive) c.danger else c.accent, FontWeight.Normal) {
                            if (action.destructive) haptics.warning() else haptics.selection()
                            leave { onPick(index) }
                        }
                    }
                }
                Box(Modifier.fillMaxWidth().background(surface, RoundedCornerShape(FmActionSheetMetrics.corner))) {
                    SheetRow(cancelLabel, c.accent, FontWeight.SemiBold) { leave(onDismiss) }
                }
            }
        }
    }
}

@Composable
private fun SheetRow(label: String, color: Color, weight: FontWeight, onTap: () -> Unit) {
    Box(
        Modifier
            .fillMaxWidth()
            .heightIn(min = FmActionSheetMetrics.row)
            .clickable(onClick = onTap)
            .semantics { role = Role.Button }
            .padding(horizontal = FmSpace.x4),
        contentAlignment = Alignment.Center,
    ) {
        Text(label, fontSize = 20.sp, fontWeight = weight, color = color, textAlign = TextAlign.Center)
    }
}
