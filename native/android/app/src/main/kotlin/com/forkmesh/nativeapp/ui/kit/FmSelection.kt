package com.forkmesh.nativeapp.ui.kit

import androidx.compose.foundation.text.selection.DisableSelection
import androidx.compose.runtime.Composable

@Composable
fun FmHoldArea(held: Boolean = true, content: @Composable () -> Unit) {
    if (held) DisableSelection(content) else content()
}
