package com.forkmesh.nativeapp.feature.localrepos

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.navigationBars
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.code.codeLangForPath
import com.forkmesh.nativeapp.code.indentLabel
import com.forkmesh.nativeapp.feature.account.AccountFeature
import com.forkmesh.nativeapp.feature.code.CodeAppBar
import com.forkmesh.nativeapp.feature.code.CodeEditorState
import com.forkmesh.nativeapp.feature.code.CodeKeyBar
import com.forkmesh.nativeapp.feature.code.EditorStatusLine
import com.forkmesh.nativeapp.feature.code.FmCodeEditor
import com.forkmesh.nativeapp.localrepos.GitWorkspace
import com.forkmesh.nativeapp.localrepos.GitWorkspaceException
import com.forkmesh.nativeapp.localrepos.LocalFileContent
import com.forkmesh.nativeapp.localrepos.localRepoHumanSize
import com.forkmesh.nativeapp.ui.kit.FmEmptyState
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmInlineStatus
import com.forkmesh.nativeapp.ui.kit.FmSkeletonList
import com.forkmesh.nativeapp.ui.kit.FmStatus
import com.forkmesh.nativeapp.ui.kit.FmTone
import com.forkmesh.nativeapp.ui.kit.FmToneButton
import com.forkmesh.nativeapp.ui.kit.LocalFmMessenger
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmSpace
import kotlin.coroutines.cancellation.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

internal class LocalFileEditorScreen(
    private val workspace: GitWorkspace,
    private val path: String,
    private val repoName: String,
    private val onClosed: (changed: Boolean) -> Unit,
) : Screen() {
    private val editor = CodeEditorState(codeLangForPath(path))
    private var content by mutableStateOf<LocalFileContent?>(null)
    private var error by mutableStateOf<String?>(null)
    private var saved by mutableStateOf("")
    private var busy by mutableStateOf(false)
    private val status = FmStatus()
    private var changed = false
    private var started = false

    override val title: String get() = path.substringAfterLast('/')

    private val dirty: Boolean get() = editor.text != saved

    override fun onDisposed() {
        onClosed(changed)
    }

    private suspend fun load() {
        try {
            val loaded = withContext(Dispatchers.IO) { workspace.readFile(path) }
            content = loaded
            saved = loaded.text
            editor.reset(loaded.text)
            error = null
        } catch (cancelled: CancellationException) {
            throw cancelled
        } catch (failure: GitWorkspaceException) {
            error = failure.message
        }
    }

    @Composable
    override fun Content(navigator: Navigator) {
        val c = Fm.colors
        val messenger = LocalFmMessenger.current
        val requireAccount = AccountFeature.rememberRequireAccount()
        val scope = rememberCoroutineScope()
        LaunchedEffect(Unit) {
            if (!started) {
                started = true
                load()
            }
        }
        fun save() = scope.launch {
            if (!requireAccount("edit files")) return@launch
            busy = true
            status.clear()
            try {
                val text = editor.text
                withContext(Dispatchers.IO) { workspace.writeFile(path, text) }
                saved = text
                changed = true
                status.show("Saved")
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (failure: GitWorkspaceException) {
                status.error(failure.message)
            } finally {
                busy = false
            }
        }
        fun delete() = scope.launch {
            if (!requireAccount("delete files")) return@launch
            val ok = messenger.showConfirmDialog(
                title = "Delete file?",
                body = "$path is removed from the working tree. Stage the deletion to record it in the next commit.",
                confirmLabel = "Delete",
                destructive = true,
            )
            if (!ok) return@launch
            busy = true
            try {
                withContext(Dispatchers.IO) { workspace.deleteFile(path) }
                changed = true
                navigator.pop()
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (failure: GitWorkspaceException) {
                busy = false
                status.error(failure.message)
            }
        }
        val loaded = content
        Column(
            Modifier
                .fillMaxSize()
                .background(c.bgBase),
        ) {
            CodeAppBar(
                title = title,
                navigator = navigator,
                actions = {
                    FmIconButton("delete_outline", "Delete file", onPressed = if (busy) null else ({ delete() }), color = c.textPrimary, iconSize = 24.dp)
                    FmToneButton(
                        label = "Save",
                        onPressed = if ((loaded?.isEditable ?: false) && dirty && !busy) ({ save() }) else null,
                        icon = "save_outlined",
                        tone = FmTone.Accent,
                        filled = true,
                        height = 32.dp,
                        modifier = Modifier.padding(end = FmSpace.x3),
                    )
                },
            )
            FmInlineStatus(status, Modifier.background(c.bgRaised).padding(horizontal = FmSpace.x3))
            val shownError = error
            Box(Modifier.weight(1f)) {
                when {
                    shownError != null -> FmEmptyState("error_outline", "Could not read file", message = shownError)
                    loaded == null -> FmSkeletonList(rows = 8, hasLeading = false, label = "Loading file")
                    !loaded.isEditable -> FmEmptyState(
                        if (loaded.isBinary) "insert_drive_file_outlined" else "text_snippet_outlined",
                        if (loaded.isBinary) "Binary file" else "File too large",
                        message = if (loaded.isBinary) {
                            "${localRepoHumanSize(loaded.bytes.toLong())} · not editable on this device."
                        } else {
                            "${localRepoHumanSize(loaded.bytes.toLong())} exceeds the in-app editor limit."
                        },
                    )
                    else -> FmCodeEditor(editor)
                }
            }
            if (shownError == null && loaded != null && loaded.isEditable) {
                Column(
                    Modifier
                        .fillMaxWidth()
                        .background(c.bgRaised)
                        .windowInsetsPadding(WindowInsets.navigationBars)
                        .imePadding(),
                ) {
                    EditorStatusLine(editor.caret, editor.focused, dirty, "${indentLabel(editor.indentUnit)} · ${editor.lang.label}")
                    if (editor.focused) CodeKeyBar(editor)
                }
            }
        }
    }
}
