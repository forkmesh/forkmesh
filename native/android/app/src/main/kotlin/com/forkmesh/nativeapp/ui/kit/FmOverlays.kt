package com.forkmesh.nativeapp.ui.kit

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.RowScope
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.BottomSheetDefaults
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.LocalTextStyle
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.ProvideTextStyle
import androidx.compose.material3.SheetState
import androidx.compose.material3.Text
import androidx.compose.material3.rememberModalBottomSheetState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.SideEffect
import androidx.compose.runtime.Stable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.key
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.runtime.staticCompositionLocalOf
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.shadow
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Shape
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.LocalView
import androidx.compose.ui.platform.LocalWindowInfo
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties
import androidx.compose.ui.window.DialogWindowProvider
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmRadius
import com.forkmesh.nativeapp.ui.theme.FmSpace
import com.forkmesh.nativeapp.ui.theme.FmType
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.launch

@Stable
class FmMessenger internal constructor() {
    internal val overlays = mutableStateListOf<Overlay>()
    private var nextOverlayId = 0L

    internal class Overlay(val id: Long, val content: @Composable () -> Unit)

    suspend fun <T> showDialog(content: @Composable (done: (T?) -> Unit) -> Unit): T? {
        val result = CompletableDeferred<T?>()
        val overlay = Overlay(nextOverlayId++) { content { value -> result.complete(value) } }
        overlays += overlay
        try {
            return result.await()
        } finally {
            overlays -= overlay
        }
    }

    suspend fun showConfirmDialog(
        title: String,
        body: String? = null,
        confirmLabel: String = "OK",
        destructive: Boolean = false,
        hideCancel: Boolean = false,
    ): Boolean = showDialog<Boolean> { done ->
        FmConfirmDialog(
            title = title,
            onConfirm = { done(true) },
            onDismissRequest = { done(null) },
            body = body,
            confirmLabel = confirmLabel,
            destructive = destructive,
            hideCancel = hideCancel,
        )
    } == true

    suspend fun showPromptDialog(
        title: String,
        body: String? = null,
        hint: String? = null,
        initialValue: String = "",
        helperText: String? = null,
        confirmLabel: String = "OK",
        destructive: Boolean = false,
        keyboardOptions: KeyboardOptions = KeyboardOptions.Default,
    ): String? = showDialog<String> { done ->
        FmPromptDialog(
            title = title,
            onConfirm = { done(it.trim()) },
            onDismissRequest = { done(null) },
            body = body,
            hint = hint,
            initialValue = initialValue,
            helperText = helperText,
            confirmLabel = confirmLabel,
            destructive = destructive,
            keyboardOptions = keyboardOptions,
        )
    }

    @OptIn(ExperimentalMaterial3Api::class)
    suspend fun <T> showModalBottomSheet(
        showDragHandle: Boolean = false,
        isScrollControlled: Boolean = false,
        containerColor: Color? = null,
        content: @Composable ColumnScope.(close: (T?) -> Unit) -> Unit,
    ): T? = showDialog<T> { done ->
        val state = rememberModalBottomSheetState(skipPartiallyExpanded = true)
        val sheetScope = rememberCoroutineScope()
        val close: (T?) -> Unit = { value ->
            sheetScope.launch { state.hide() }.invokeOnCompletion { done(value) }
        }
        FmModalBottomSheet(
            onDismissRequest = { done(null) },
            sheetState = state,
            showDragHandle = showDragHandle,
            isScrollControlled = isScrollControlled,
            containerColor = containerColor,
        ) {
            content(close)
        }
    }
}

val LocalFmMessenger = staticCompositionLocalOf<FmMessenger> {
    error("no FmMessenger in composition: wrap the UI in ForkMeshTheme")
}

@Composable
fun FmMessengerScope(content: @Composable () -> Unit) {
    val messenger = remember { FmMessenger() }
    CompositionLocalProvider(LocalFmMessenger provides messenger) {
        Box(Modifier.fillMaxSize(), propagateMinConstraints = true) {
            content()
        }
        for (overlay in messenger.overlays) {
            key(overlay.id) { overlay.content() }
        }
    }
}

@Composable
fun FmAlertDialog(
    onDismissRequest: () -> Unit,
    modifier: Modifier = Modifier,
    title: String? = null,
    titleMaxLines: Int = Int.MAX_VALUE,
    containerColor: Color? = null,
    shape: Shape = MaterialTheme.shapes.extraLarge,
    actionsPadding: PaddingValues = PaddingValues(start = FmSpace.x4, end = FmSpace.x4, bottom = FmSpace.x4),
    actions: (@Composable RowScope.() -> Unit)? = null,
    content: (@Composable ColumnScope.() -> Unit)? = null,
) {
    val c = Fm.colors
    FmSoundOnShow()
    Dialog(onDismissRequest = onDismissRequest, properties = DialogProperties(usePlatformDefaultWidth = false)) {
        val window = (LocalView.current.parent as? DialogWindowProvider)?.window
        SideEffect { window?.setDimAmount(0.54f) }
        val shadow = if (c.dark) Color(0x99000000) else Color(0x33000000)
        Column(
            modifier
                .fmTapSounds()
                .padding(FmSpace.x5)
                .widthIn(min = 280.dp)
                .shadow(if (c.dark) 24.dp else 8.dp, shape, ambientColor = shadow, spotColor = shadow)
                .background(containerColor ?: if (c.dark) c.bgOverlay else c.bgRaised, shape),
        ) {
            if (title != null) {
                Text(
                    title,
                    modifier = Modifier.padding(start = 24.dp, top = 24.dp, end = 24.dp, bottom = if (content == null) 20.dp else 0.dp),
                    maxLines = titleMaxLines,
                    overflow = TextOverflow.Ellipsis,
                    style = Fm.text.titleMedium,
                )
            }
            if (content != null) {
                ProvideTextStyle(
                    Fm.text.bodyMedium.copy(color = c.textSecondary, lineHeight = FmType.small * FmType.leadingRelaxed),
                ) {
                    Column(
                        Modifier
                            .weight(1f, fill = false)
                            .padding(start = 24.dp, top = 16.dp, end = 24.dp, bottom = 24.dp),
                        content = content,
                    )
                }
            }
            if (actions != null) {
                Row(
                    Modifier
                        .fillMaxWidth()
                        .padding(actionsPadding),
                    horizontalArrangement = Arrangement.spacedBy(FmSpace.x2, Alignment.End),
                    verticalAlignment = Alignment.CenterVertically,
                    content = actions,
                )
            }
        }
    }
}

@Composable
fun FmConfirmDialog(
    title: String,
    onConfirm: () -> Unit,
    onDismissRequest: () -> Unit,
    modifier: Modifier = Modifier,
    body: String? = null,
    confirmLabel: String = "OK",
    destructive: Boolean = false,
    hideCancel: Boolean = false,
    busy: Boolean = false,
    fields: (@Composable ColumnScope.() -> Unit)? = null,
) {
    FmAlertDialog(
        onDismissRequest = onDismissRequest,
        modifier = modifier,
        title = title,
        titleMaxLines = 2,
        containerColor = Fm.colors.bgOverlay,
        shape = RoundedCornerShape(14.dp),
        actionsPadding = PaddingValues(start = FmSpace.x4, end = FmSpace.x4, bottom = FmSpace.x3),
        actions = {
            if (hideCancel) {
                FmTextButton(onClick = onConfirm) { Text(confirmLabel) }
            } else {
                FmDialogActions(
                    onCancel = onDismissRequest,
                    onConfirm = onConfirm,
                    confirmLabel = confirmLabel,
                    destructive = destructive,
                    busy = busy,
                )
            }
        },
        content = {
            Column(
                Modifier
                    .width(480.dp)
                    .verticalScroll(rememberScrollState()),
            ) {
                if (body != null) {
                    val style = LocalTextStyle.current
                    SelectionContainer {
                        Text(body, style = style.copy(color = Fm.colors.textSecondary, lineHeight = style.fontSize * 1.4f))
                    }
                    FmMessageTools("$title\n$body")
                }
                if (fields != null) fields()
            }
        },
    )
}

@Composable
fun FmPromptDialog(
    title: String,
    onConfirm: (String) -> Unit,
    onDismissRequest: () -> Unit,
    modifier: Modifier = Modifier,
    body: String? = null,
    hint: String? = null,
    initialValue: String = "",
    helperText: String? = null,
    confirmLabel: String = "OK",
    destructive: Boolean = false,
    keyboardOptions: KeyboardOptions = KeyboardOptions.Default,
) {
    var text by rememberSaveable { mutableStateOf(initialValue) }
    FmConfirmDialog(
        title = title,
        onConfirm = { onConfirm(text) },
        onDismissRequest = onDismissRequest,
        modifier = modifier,
        body = body,
        confirmLabel = confirmLabel,
        destructive = destructive,
    ) {
        FmTextField(
            value = text,
            onValueChange = { text = it },
            hint = hint,
            helperText = helperText,
            autofocus = true,
            keyboardOptions = keyboardOptions,
        )
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun FmModalBottomSheet(
    onDismissRequest: () -> Unit,
    modifier: Modifier = Modifier,
    showDragHandle: Boolean = false,
    isScrollControlled: Boolean = false,
    sheetState: SheetState = rememberModalBottomSheetState(skipPartiallyExpanded = !isScrollControlled),
    containerColor: Color? = null,
    content: @Composable ColumnScope.() -> Unit,
) {
    val c = Fm.colors
    val maxHeight: Dp = with(LocalDensity.current) { (LocalWindowInfo.current.containerSize.height * 9f / 16f).toDp() }
    FmSoundOnShow()
    val dragHandle: (@Composable () -> Unit)? = if (showDragHandle) {
        { BottomSheetDefaults.DragHandle(color = c.textPrimary.copy(alpha = .4f)) }
    } else {
        null
    }
    ModalBottomSheet(
        onDismissRequest = onDismissRequest,
        modifier = modifier,
        sheetState = sheetState,
        shape = RoundedCornerShape(topStart = FmSheet.corner, topEnd = FmSheet.corner),
        containerColor = containerColor ?: if (c.dark) c.bgOverlay else c.bgRaised,
        contentColor = c.textPrimary,
        tonalElevation = 0.dp,
        scrimColor = if (c.dark) Color(0x8C000000) else Color(0x59000000),
        dragHandle = dragHandle,
    ) {
        Column(if (isScrollControlled) Modifier.fmTapSounds() else Modifier.fmTapSounds().heightIn(max = maxHeight), content = content)
    }
}

object FmSheet {
    val corner = 28.dp
}

