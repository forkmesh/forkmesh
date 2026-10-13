package com.forkmesh.nativeapp.ui.kit

import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.Stable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.semantics.LiveRegionMode
import androidx.compose.ui.semantics.liveRegion
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.ui.theme.Fm

@Stable
class FmStatus {
    var message by mutableStateOf<String?>(null)
        private set
    var isError by mutableStateOf(false)
        private set
    var actionLabel by mutableStateOf<String?>(null)
        private set
    private var onAction: (() -> Unit)? = null

    fun show(message: String, actionLabel: String? = null, onAction: (() -> Unit)? = null) =
        set(message, false, actionLabel, onAction)

    fun error(message: String, actionLabel: String? = null, onAction: (() -> Unit)? = null) =
        set(message, true, actionLabel, onAction)

    fun report(message: String, isError: Boolean) = set(message, isError, null, null)

    fun clear() {
        message = null
        isError = false
        actionLabel = null
        onAction = null
    }

    fun runAction() {
        val action = onAction
        clear()
        action?.invoke()
    }

    private fun set(message: String, isError: Boolean, actionLabel: String?, onAction: (() -> Unit)?) {
        this.message = message
        this.isError = isError
        this.actionLabel = if (onAction != null) actionLabel else null
        this.onAction = onAction
    }
}

@Composable
fun rememberFmStatus(): FmStatus = remember { FmStatus() }

@Composable
fun FmInlineStatus(status: FmStatus, modifier: Modifier = Modifier, dismissible: Boolean = true) {
    val message = status.message ?: return
    FmInlineStatusLine(
        message = message,
        isError = status.isError,
        modifier = modifier,
        actionLabel = status.actionLabel,
        onAction = status::runAction,
        onDismiss = if (dismissible) status::clear else null,
    )
}

@Composable
fun FmInlineStatusLine(
    message: String?,
    isError: Boolean,
    modifier: Modifier = Modifier,
    actionLabel: String? = null,
    onAction: (() -> Unit)? = null,
    onDismiss: (() -> Unit)? = null,
) {
    if (message.isNullOrEmpty()) return
    val c = Fm.colors
    Row(
        modifier
            .fillMaxWidth()
            .semantics { liveRegion = if (isError) LiveRegionMode.Assertive else LiveRegionMode.Polite },
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(
            message,
            modifier = Modifier.weight(1f),
            style = Fm.text.bodySmall.copy(color = if (isError) c.danger else c.textSecondary),
        )
        if (actionLabel != null && onAction != null) {
            FmTextButton(onClick = onAction) { Text(actionLabel) }
        }
        if (onDismiss != null) {
            FmIconButton("close", "Dismiss", onDismiss, iconSize = 16.dp)
        }
    }
}
