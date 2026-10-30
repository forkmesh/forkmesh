package com.forkmesh.nativeapp.ui.navigation

import androidx.compose.runtime.Composable
import androidx.compose.runtime.compositionLocalOf
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.graphics.drawscope.inset
import androidx.compose.ui.semantics.selected
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmRadius
import com.forkmesh.nativeapp.ui.theme.FmSpace

val LocalShellPaneOpen = compositionLocalOf<Screen?> { null }

@Composable
fun Modifier.shellPaneSelected(selected: Boolean): Modifier {
    if (!selected) return this
    val fill = Fm.colors.accentSubtle
    return this
        .drawBehind {
            inset(FmSpace.x2.toPx(), 2.dp.toPx()) {
                drawRoundRect(fill, cornerRadius = CornerRadius(FmRadius.md.toPx()))
            }
        }
        .semantics { this.selected = true }
}
