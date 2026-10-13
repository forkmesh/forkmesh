package com.forkmesh.nativeapp.feature.code.collab

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.input.KeyboardCapitalization
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.collab.NewIssueComposer
import com.forkmesh.nativeapp.collab.NewIssueDraft
import com.forkmesh.nativeapp.model.Repository
import com.forkmesh.nativeapp.ui.kit.FmBottomActionBar
import com.forkmesh.nativeapp.ui.kit.FmField
import com.forkmesh.nativeapp.ui.kit.FmInboxPendingNote
import com.forkmesh.nativeapp.ui.kit.FmSectionHeader
import com.forkmesh.nativeapp.ui.kit.FmTextField
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmRadius
import com.forkmesh.nativeapp.ui.theme.FmSpace
import com.forkmesh.nativeapp.ui.theme.FmType
import kotlinx.coroutines.CompletableDeferred

class NewIssueScreen internal constructor(
    private val repo: Repository,
    private val parentNumber: Int,
    private val onResult: (NewIssueDraft?) -> Unit,
    private val submitsItself: Boolean = false,
) : Screen() {
    private var env: CollabEnv? = null
    private var titleText by mutableStateOf("")
    private var body by mutableStateOf(NewIssueComposer.initialBody(parentNumber))
    private var labels by mutableStateOf("")
    private var priority by mutableStateOf("")
    private var milestone by mutableStateOf("")
    private var assignees by mutableStateOf("")
    private var titleError by mutableStateOf<String?>(null)
    private var delivered = false

    override val title: String get() = NewIssueComposer.title(parentNumber)

    private fun deliver(draft: NewIssueDraft?) {
        if (delivered) return
        delivered = true
        onResult(draft)
    }

    override fun onDisposed() {
        deliver(null)
    }

    private fun submit(navigator: Navigator) {
        val draft = NewIssueComposer.draft(titleText, body, labels, priority, milestone, assignees)
        if (draft == null) {
            titleError = NewIssueComposer.TITLE_REQUIRED
            return
        }
        deliver(draft)
        if (navigator.top === this) navigator.pop()
        if (submitsItself) env?.launch { submitNewIssue(repo, draft) }
    }

    @Composable
    override fun Content(navigator: Navigator) {
        val colors = Fm.colors
        if (submitsItself) env = rememberCollabEnv(navigator)
        Column(
            Modifier
                .fillMaxSize()
                .background(colors.bgBase)
                .imePadding(),
        ) {
            CollabTopBar(
                navigator,
                leadingIcon = "close",
                leadingLabel = "Cancel",
                titleContent = {
                    Column {
                        Text(title, style = Fm.text.titleMedium.copy(color = colors.textPrimary))
                        Text(
                            repo.fullName,
                            maxLines = 1,
                            overflow = TextOverflow.Ellipsis,
                            style = Fm.text.bodySmall.copy(fontWeight = FmType.emphasis, color = colors.textTertiary),
                        )
                    }
                },
            )
            LazyColumn(
                Modifier
                    .weight(1f)
                    .fillMaxWidth(),
                contentPadding = PaddingValues(start = FmSpace.x4, top = FmSpace.x3, end = FmSpace.x4, bottom = FmSpace.x5),
            ) {
                item { Fields(navigator) }
            }
            FmBottomActionBar(padding = PaddingValues(horizontal = FmSpace.x4, vertical = FmSpace.x3)) {
                Button(
                    onClick = { submit(navigator) },
                    modifier = Modifier
                        .fillMaxWidth()
                        .height(48.dp)
                        .testTag("new-issue-submit"),
                    shape = RoundedCornerShape(FmRadius.lg),
                    colors = ButtonDefaults.buttonColors(containerColor = colors.accent, contentColor = Color.White),
                    contentPadding = ButtonDefaults.ButtonWithIconContentPadding,
                ) {
                    FmIcon("send_rounded", size = 18.dp)
                    Spacer(Modifier.width(ButtonDefaults.IconSpacing))
                    Text("Submit", style = Fm.text.titleSmall.copy(color = Color.White))
                }
            }
        }
    }

    @Composable
    private fun Fields(navigator: Navigator) {
        val sentences = KeyboardOptions(capitalization = KeyboardCapitalization.Sentences)
        Column(Modifier.fillMaxWidth()) {
            SessionIdentityBar("Posting")
            Spacer(Modifier.height(FmSpace.x3))
            FmTextField(
                value = titleText,
                onValueChange = {
                    titleText = it
                    if (titleError != null && it.trim().isNotEmpty()) titleError = null
                },
                hint = "Issue title",
                errorText = titleError,
                autofocus = true,
                keyboardOptions = sentences.copy(imeAction = ImeAction.Next),
            )
            Spacer(Modifier.height(FmSpace.x3))
            FmTextField(
                value = body,
                onValueChange = { body = it },
                hint = "Add a description",
                minLines = 6,
                maxLines = 14,
                keyboardOptions = sentences,
            )
            Spacer(Modifier.height(FmSpace.x5))
            FmSectionHeader(title = "Triage", icon = "tune_rounded")
            Spacer(Modifier.height(FmSpace.x3))
            FmField("Labels") {
                FmTextField(
                    value = labels,
                    onValueChange = { labels = it },
                    helperText = "Comma-separated, e.g. mobile, bug",
                    keyboardOptions = KeyboardOptions(imeAction = ImeAction.Next),
                )
            }
            Spacer(Modifier.height(FmSpace.x3))
            Row(verticalAlignment = Alignment.Top) {
                FmField("Priority", Modifier.weight(1f)) {
                    FmTextField(
                        value = priority,
                        onValueChange = { priority = it },
                        keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number, imeAction = ImeAction.Next),
                    )
                }
                Spacer(Modifier.width(FmSpace.x3))
                FmField("Milestone", Modifier.weight(1f)) {
                    FmTextField(
                        value = milestone,
                        onValueChange = { milestone = it },
                        keyboardOptions = KeyboardOptions(imeAction = ImeAction.Next),
                    )
                }
            }
            Spacer(Modifier.height(FmSpace.x3))
            FmField("Assignees") {
                FmTextField(
                    value = assignees,
                    onValueChange = { assignees = it },
                    helperText = "Comma-separated node/user names",
                    keyboardOptions = KeyboardOptions(imeAction = ImeAction.Done),
                    keyboardActions = KeyboardActions(onDone = { submit(navigator) }),
                )
            }
            Spacer(Modifier.height(FmSpace.x4))
            FmInboxPendingNote()
        }
    }

    companion object {
        fun forRoute(repo: Repository): Screen = NewIssueScreen(repo, 0, {}, submitsItself = true)

        suspend fun open(navigator: Navigator, repo: Repository, parentNumber: Int = 0): NewIssueDraft? {
            val result = CompletableDeferred<NewIssueDraft?>()
            navigator.push(NewIssueScreen(repo, parentNumber, onResult = { result.complete(it) }))
            return result.await()
        }
    }
}
