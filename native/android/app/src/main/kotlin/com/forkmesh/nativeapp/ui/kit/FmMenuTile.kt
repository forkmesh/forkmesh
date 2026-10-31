package com.forkmesh.nativeapp.ui.kit

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.ui.theme.Fm

@Composable
fun FmMenuTile(tint: Color = Fm.colors.accent, content: @Composable () -> Unit) {
    Box(
        Modifier.size(28.dp).background(tint.copy(alpha = 0.12f), RoundedCornerShape(8.dp)),
        contentAlignment = Alignment.Center,
    ) {
        content()
    }
}
