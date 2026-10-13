package com.forkmesh.nativeapp.feature.localrepos

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.size
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.MutableState
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties
import com.forkmesh.nativeapp.ui.kit.FmCard
import com.forkmesh.nativeapp.ui.kit.FmCircularProgressIndicator
import com.forkmesh.nativeapp.ui.kit.FmMessenger
import com.forkmesh.nativeapp.ui.kit.FmTone
import com.forkmesh.nativeapp.ui.kit.FmToneButton
import com.forkmesh.nativeapp.ui.kit.fmTapSounds
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmRadius
import com.forkmesh.nativeapp.ui.theme.FmSpace
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.launch

@Composable
internal fun LocalGitBusyCard(label: String, onCancel: (() -> Unit)?) {
    FmCard(radius = FmRadius.lg) {
        Column(horizontalAlignment = Alignment.CenterHorizontally) {
            FmCircularProgressIndicator(Modifier.size(22.dp), strokeWidth = 2.4.dp)
            Spacer(Modifier.height(FmSpace.x3))
            Text("$label…", style = Fm.text.labelMedium.copy(color = Fm.colors.textPrimary))
            if (onCancel != null) {
                Spacer(Modifier.height(FmSpace.x3))
                FmToneButton(label = "Cancel", onPressed = onCancel, tone = FmTone.Danger)
            }
        }
    }
}

@Composable
internal fun LocalGitBusyOverlay(label: String, onCancel: (() -> Unit)?) {
    Box(
        Modifier
            .fillMaxSize()
            .background(Fm.colors.bgBase.copy(alpha = .55f))
            .clickable(interactionSource = remember { MutableInteractionSource() }, indication = null) {},
        contentAlignment = Alignment.Center,
    ) {
        LocalGitBusyCard(label, onCancel)
    }
}

internal suspend fun <T> FmMessenger.runBusy(
    label: MutableState<String>,
    onCancel: (() -> Unit)?,
    body: suspend () -> T,
): T = coroutineScope {
    val dialog = launch(start = CoroutineStart.UNDISPATCHED) {
        showDialog<Unit> { _ ->
            Dialog(
                onDismissRequest = {},
                properties = DialogProperties(dismissOnBackPress = false, dismissOnClickOutside = false),
            ) {
                Box(Modifier.fmTapSounds()) { LocalGitBusyCard(label.value, onCancel) }
            }
        }
    }
    try {
        body()
    } finally {
        dialog.cancel()
    }
}

internal fun busyLabel(text: String): MutableState<String> = mutableStateOf(text)
