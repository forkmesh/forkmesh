package com.forkmesh.nativeapp.ui.kit

import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import androidx.compose.foundation.clickable
import androidx.compose.ui.Modifier
import androidx.compose.ui.composed
import androidx.compose.ui.platform.LocalContext
import com.forkmesh.nativeapp.navigation.ComposeTarget
import com.forkmesh.nativeapp.navigation.MessageHandoff
import com.forkmesh.nativeapp.ui.theme.FmHaptics

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

fun Modifier.fmMessageTools(text: String?): Modifier = composed {
    val message = text.orEmpty()
    if (!MessageHandoff.canHandOff(message)) return@composed Modifier
    val context = LocalContext.current
    val haptics = FmHaptics.current
    Modifier.clickable(onClickLabel = "Open in composer") {
        haptics.tap()
        FmMessageHandoff.send(context, message)
    }
}
