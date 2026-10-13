package com.forkmesh.nativeapp.ui.kit

import android.graphics.BitmapFactory
import android.icu.text.BreakIterator
import android.util.Base64
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.semantics.clearAndSetSemantics
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.feature.chat.RelayConnection
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmColors
import com.forkmesh.nativeapp.ui.theme.FmType

@Composable
fun AvatarWithDot(
    state: RelayConnection,
    label: String,
    modifier: Modifier = Modifier,
    size: Dp = 34.dp,
) {
    val dot = when (state) {
        RelayConnection.Connected -> FmColors.success
        RelayConnection.Connecting -> FmColors.warning
        RelayConnection.Offline -> FmColors.offline
    }
    val status = when (state) {
        RelayConnection.Connected -> "online"
        RelayConnection.Connecting -> "connecting"
        RelayConnection.Offline -> "offline"
    }
    Box(
        modifier
            .size(size)
            .clearAndSetSemantics { contentDescription = "Connection status: $status. Account $label" },
    ) {
        FmAvatar(name = label, size = size)
        Box(
            Modifier
                .align(Alignment.BottomEnd)
                .offset(x = 1.dp, y = 1.dp)
                .size(12.dp)
                .background(dot, CircleShape)
                .border(2.dp, FmColors.bgRaised, CircleShape),
        )
    }
}

@Composable
fun FmAvatar(
    name: String,
    size: Dp,
    modifier: Modifier = Modifier,
    avatarPng: String = "",
    textStyle: TextStyle = Fm.text.titleMedium,
) {
    val image = remember(avatarPng) { decodeAvatarPng(avatarPng) }
    if (image != null) {
        Image(
            bitmap = image,
            contentDescription = null,
            modifier = modifier
                .size(size)
                .clip(CircleShape),
            contentScale = ContentScale.Crop,
        )
        return
    }
    Box(
        modifier
            .size(size)
            .background(FmColors.senderColor(name), CircleShape),
        contentAlignment = Alignment.Center,
    ) {
        Text(initialOf(name), style = textStyle.copy(color = Color.White, fontWeight = FmType.emphasis))
    }
}

internal fun initialOf(name: String): String {
    if (name.isEmpty()) return "?"
    val graphemes = BreakIterator.getCharacterInstance()
    graphemes.setText(name)
    val end = graphemes.next()
    return name.substring(0, if (end == BreakIterator.DONE) name.length else end).uppercase()
}

private fun decodeAvatarPng(base64: String): ImageBitmap? {
    if (base64.isEmpty()) return null
    return try {
        val bytes = Base64.decode(base64, Base64.DEFAULT)
        BitmapFactory.decodeByteArray(bytes, 0, bytes.size)?.asImageBitmap()
    } catch (_: IllegalArgumentException) {
        null
    }
}
