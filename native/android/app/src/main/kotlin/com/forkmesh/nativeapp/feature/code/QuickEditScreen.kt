package com.forkmesh.nativeapp.feature.code

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.navigationBars
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.api.InboxService
import com.forkmesh.nativeapp.code.QuickEdit
import com.forkmesh.nativeapp.code.QuickEditBlocker
import com.forkmesh.nativeapp.code.codeLangForPath
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.account.AccountFeature
import com.forkmesh.nativeapp.feature.account.AuthService
import com.forkmesh.nativeapp.feature.code.collab.SessionIdentityBar
import com.forkmesh.nativeapp.model.Repository
import com.forkmesh.nativeapp.ui.kit.FmAlertDialog
import com.forkmesh.nativeapp.ui.kit.FmDialogActions
import com.forkmesh.nativeapp.ui.kit.FmEmptyState
import com.forkmesh.nativeapp.ui.kit.FmField
import com.forkmesh.nativeapp.ui.kit.FmInlineStatus
import com.forkmesh.nativeapp.ui.kit.FmSkeletonList
import com.forkmesh.nativeapp.ui.kit.FmStatus
import com.forkmesh.nativeapp.ui.kit.FmTextField
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

private data class ProposedChange(val title: String, val description: String)

internal class QuickEditScreen(
    val repo: Repository,
    val path: String,
    val ref: String = "",
) : Screen() {
    private val editor = CodeEditorState(codeLangForPath(path))
    private var bytes by mutableStateOf<ByteArray?>(null)
    private var original by mutableStateOf("")
    private var error by mutableStateOf<String?>(null)
    private var blocker by mutableStateOf<QuickEditBlocker?>(null)
    private var loading by mutableStateOf(true)
    private var submitting by mutableStateOf(false)
    private var submitted by mutableStateOf(false)
    private val status = FmStatus()
    private var started = false

    override val title: String get() = path.substringAfterLast('/')

    private val base: String get() = QuickEdit.base(ref, repo.defaultBranch)

    private val dirty: Boolean get() = bytes != null && editor.text != original

    private suspend fun load() {
        try {
            val served = withContext(Dispatchers.IO) {
                repoApi.rawBytes(repo.owner, repo.name, path, ref = ref, maxBytes = QuickEdit.maxBytes)
            }
            val found = QuickEdit.blockerFor(served)
            if (found != null) {
                loading = false
                blocker = found
                return
            }
            val text = QuickEdit.decodeStrict(served) ?: ""
            bytes = served
            original = text
            editor.reset(text)
            loading = false
        } catch (cancelled: CancellationException) {
            throw cancelled
        } catch (failure: Exception) {
            loading = false
            blocker = QuickEdit.loadFailureBlocker(failure)
            error = if (blocker == null) failure.toString() else null
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
        suspend fun compose(): ProposedChange? = messenger.showDialog { done -> ProposeDialog(done) }
        fun propose() {
            val served = bytes ?: return
            if (!dirty || submitting || submitted) return
            scope.launch {
                if (!requireAccount("propose a change")) return@launch
                val result = QuickEdit.patch(path, served, editor.text)
                if (result.isEmpty) {
                    status.show(QuickEdit.nothingChanged(path))
                    return@launch
                }
                val account = Services.get<AuthService>()?.core?.session?.nodeName ?: ""
                val composed = compose() ?: return@launch
                submitting = true
                try {
                    withContext(Dispatchers.IO) {
                        Services.require<InboxService>().submitNewPull(
                            repo.owner,
                            repo.name,
                            title = composed.title,
                            base = base,
                            head = QuickEdit.head(account, path),
                            patch = result.patch,
                            description = composed.description,
                            authorName = account,
                        )
                    }
                    submitting = false
                    submitted = true
                    status.show(QuickEdit.submittedMessage)
                } catch (cancelled: CancellationException) {
                    throw cancelled
                } catch (failure: Exception) {
                    submitting = false
                    status.error(QuickEdit.submitFailure(failure.toString()))
                }
            }
        }
        Column(
            Modifier
                .fillMaxSize()
                .background(c.bgBase),
        ) {
            CodeAppBar(
                title = title,
                navigator = navigator,
                actions = {
                    FmToneButton(
                        label = "Propose",
                        onPressed = if (dirty && !submitting && !submitted) ::propose else null,
                        icon = "call_merge",
                        tone = FmTone.Accent,
                        filled = true,
                        height = 32.dp,
                        modifier = Modifier.padding(end = FmSpace.x3),
                        busy = submitting,
                    )
                },
            )
            val shownBlocker = blocker
            val shownError = error
            Box(Modifier.weight(1f)) {
                when {
                    loading -> FmSkeletonList(rows = 8, hasLeading = false, label = "Loading file")
                    shownBlocker != null -> FmEmptyState(
                        "text_snippet_outlined",
                        QuickEdit.blockerTitle(shownBlocker),
                        message = QuickEdit.blockerMessage(shownBlocker),
                    )
                    shownError != null -> FmEmptyState("error_outline", "Could not read file", message = shownError)
                    else -> FmCodeEditor(editor)
                }
            }
            if (!loading && shownBlocker == null && shownError == null) {
                Column(
                    Modifier
                        .fillMaxWidth()
                        .background(c.bgRaised)
                        .windowInsetsPadding(WindowInsets.navigationBars)
                        .imePadding(),
                ) {
                    EditorStatusLine(editor.caret, editor.focused, dirty, "into $base")
                    FmInlineStatus(status, Modifier.padding(horizontal = FmSpace.x3))
                    if (editor.focused) CodeKeyBar(editor)
                }
            }
        }
    }

    @Composable
    private fun ProposeDialog(done: (ProposedChange?) -> Unit) {
        var subject by remember { mutableStateOf(QuickEdit.suggestTitle(path)) }
        var description by remember { mutableStateOf("") }
        FmAlertDialog(
            onDismissRequest = { done(null) },
            title = "Propose change",
            actions = {
                FmDialogActions(
                    onCancel = { done(null) },
                    onConfirm = {
                        val trimmed = subject.trim()
                        if (trimmed.isNotEmpty()) done(ProposedChange(trimmed, description.trim()))
                    },
                    confirmLabel = "Submit",
                )
            },
        ) {
            SessionIdentityBar(verb = "Proposing")
            Spacer(Modifier.height(FmSpace.x3))
            FmField("Title") { FmTextField(subject, { subject = it }, autofocus = true) }
            Spacer(Modifier.height(FmSpace.x3))
            FmField("Description") { FmTextField(description, { description = it }, minLines = 3, maxLines = 6) }
            Spacer(Modifier.height(FmSpace.x3))
            Text(
                QuickEdit.composeFooter(path, base),
                style = Fm.text.bodySmall.copy(color = Fm.colors.textTertiary),
                modifier = Modifier.align(Alignment.Start),
            )
        }
    }
}
