package com.forkmesh.nativeapp.ui.kit

import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.size
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.role
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.navigation.ComposeTarget
import com.forkmesh.nativeapp.navigation.MessageHandoff
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmHaptics
import com.forkmesh.nativeapp.ui.theme.FmIcon
import kotlinx.coroutines.delay

object FmMessageHandoff {
    var compose: ((ComposeTarget) -> Unit)? = null

    fun send(context: Context, text: String) {
        if (!MessageHandoff.canHandOff(text)) return
        val target = MessageHandoff.composeTarget(text)
        val open = compose
        if (open != null) open(target) else copy(context, target.text.orEmpty())
    }

    fun compose(context: Context, target: ComposeTarget) {
        val open = compose
        if (open != null) open(target) else copy(context, target.text.orEmpty())
    }

    fun copy(context: Context, text: String): Boolean {
        val clipboard = context.getSystemService(Context.CLIPBOARD_SERVICE) as? ClipboardManager ?: return false
        clipboard.setPrimaryClip(ClipData.newPlainText("ForkMesh", MessageHandoff.cleaned(text)))
        return true
    }
}

@Composable
fun FmMessageTools(text: String?, modifier: Modifier = Modifier, color: Color? = null, handoff: ComposeTarget? = null) {
    val message = text.orEmpty()
    if (!MessageHandoff.canHandOff(message)) return
    val context = LocalContext.current
    val haptics = FmHaptics.current
    var copied by remember { mutableStateOf(false) }
    LaunchedEffect(copied) {
        if (copied) {
            delay(1500)
            copied = false
        }
    }
    val tint = color ?: Fm.colors.textTertiary
    Row(modifier, verticalAlignment = Alignment.CenterVertically) {
        MessageTool(if (copied) "check" else "content_copy", if (copied) "Copied" else "Copy message", tint) {
            haptics.tap()
            copied = FmMessageHandoff.copy(context, message)
        }
        MessageTool("send_outlined", "Send to agent", tint) {
            haptics.tap()
            if (handoff != null) FmMessageHandoff.compose(context, handoff) else FmMessageHandoff.send(context, message)
        }
    }
}

@Composable
private fun MessageTool(icon: String, label: String, tint: Color, onClick: () -> Unit) {
    Box(
        Modifier
            .size(24.dp)
            .clickable(onClick = onClick)
            .semantics {
                role = Role.Button
                contentDescription = label
            },
        contentAlignment = Alignment.Center,
    ) {
        FmIcon(icon, size = 13.dp, tint = tint)
    }
}
