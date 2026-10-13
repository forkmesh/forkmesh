package com.forkmesh.nativeapp.ui.kit

import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.hapticfeedback.HapticFeedbackType
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalHapticFeedback
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmSpace
import kotlinx.coroutines.launch

@OptIn(ExperimentalFoundationApi::class)
@Composable
fun FmMessageActions(
    text: String,
    modifier: Modifier = Modifier,
    contentAlignment: Alignment = Alignment.TopStart,
    content: @Composable () -> Unit,
) {
    val messenger = LocalFmMessenger.current
    val context = LocalContext.current
    val haptics = LocalHapticFeedback.current
    val scope = rememberCoroutineScope()
    val status = rememberFmStatus()
    Column(modifier) {
        Box(
            Modifier.combinedClickable(
                enabled = text.isNotEmpty(),
                onClick = {},
                onLongClick = {
                    haptics.performHapticFeedback(HapticFeedbackType.LongPress)
                    scope.launch { messageTextActionsFlow(messenger, context, text, status) }
                },
            ),
            contentAlignment = contentAlignment,
        ) {
            content()
        }
        FmInlineStatus(status, Modifier.padding(top = FmSpace.x1))
    }
}

suspend fun messageTextActionsFlow(messenger: FmMessenger, context: Context, text: String, status: FmStatus? = null) {
    val choice = messenger.showModalBottomSheet<String> { close ->
        Spacer(Modifier.height(FmSpace.x2))
        Column {
            FmListRow(title = "Copy text", leading = { FmIconTile(icon = "copy_outlined") }, onTap = { close("copy") })
            FmRowDivider()
            FmListRow(title = "Select text", leading = { FmIconTile(icon = "select_all_outlined") }, onTap = { close("select") })
        }
        Spacer(Modifier.height(FmSpace.x2))
    } ?: return
    when (choice) {
        "copy" -> copyMessageText(context, text, status)
        "select" -> showSelectableText(messenger, text)
    }
}

fun copyMessageText(context: Context, text: String, status: FmStatus? = null) {
    val clipboard = context.getSystemService(Context.CLIPBOARD_SERVICE) as? ClipboardManager
    clipboard?.setPrimaryClip(ClipData.newPlainText("ForkMesh", text))
    if (clipboard == null) status?.error("Could not reach the clipboard.") else status?.show("Copied to clipboard")
}

suspend fun showSelectableText(messenger: FmMessenger, text: String) {
    messenger.showModalBottomSheet<Unit>(isScrollControlled = true) { close ->
        Column(Modifier.fillMaxWidth().padding(FmSpace.x4)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text("Select text", modifier = Modifier.weight(1f), style = Fm.text.titleMedium)
                FmTextButton(onClick = { close(null) }) { Text("Done") }
            }
            Box(Modifier.heightIn(max = 560.dp).verticalScroll(rememberScrollState()).padding(top = FmSpace.x2)) {
                SelectionContainer {
                    Text(text, style = Fm.text.bodyLarge.copy(color = Fm.colors.textPrimary))
                }
            }
        }
    }
}
