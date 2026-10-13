@file:OptIn(ExperimentalLayoutApi::class)

package com.forkmesh.nativeapp.feature.code.collab

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.width
import androidx.compose.material3.LocalTextStyle
import androidx.compose.material3.Text
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.collab.DISCUSSION_CATEGORY_LABELS
import com.forkmesh.nativeapp.collab.IssueActions
import com.forkmesh.nativeapp.collab.NewIssueDraft
import com.forkmesh.nativeapp.collab.PENDING_NOTE
import com.forkmesh.nativeapp.collab.PullReview
import com.forkmesh.nativeapp.model.Issue
import com.forkmesh.nativeapp.model.PublishedPull
import com.forkmesh.nativeapp.model.PullRequest
import com.forkmesh.nativeapp.model.Repository
import com.forkmesh.nativeapp.ui.kit.FmDialogActions
import com.forkmesh.nativeapp.ui.kit.FmField
import com.forkmesh.nativeapp.ui.kit.FmInboxPendingNote
import com.forkmesh.nativeapp.ui.kit.FmStatus
import com.forkmesh.nativeapp.ui.kit.FmTextField
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmType

internal suspend fun CollabEnv.openNewIssueComposer(repo: Repository, parentNumber: Int = 0, status: FmStatus = this.status) {
    if (!requireAccount("open an issue")) return
    val draft = NewIssueScreen.open(navigator, repo, parentNumber) ?: return
    submitNewIssue(repo, draft, status)
}

internal suspend fun CollabEnv.submitNewIssue(repo: Repository, draft: NewIssueDraft, status: FmStatus = this.status) {
    if (!requireAccount("open an issue")) return
    run(PENDING_NOTE, status) {
        inbox.submitNewIssue(
            repo.owner,
            repo.name,
            title = draft.title,
            body = draft.body,
            labels = draft.labels,
            milestone = draft.milestone,
            priority = draft.priority,
            assignees = draft.assignees,
        )
    }
}

private enum class IssueSheetAction { Vote, Comment, Status }

internal suspend fun CollabEnv.showIssueActions(repo: Repository, issue: Issue) {
    if (!requireAccount("comment on issues")) return
    val action = messenger.showModalBottomSheet<IssueSheetAction>(
        showDragHandle = true,
        containerColor = palette.bgOverlay,
    ) { close ->
        SheetHeader(
            IssueActions.header(issue),
            subtitle = issue.body.ifEmpty { null },
            subtitleMaxLines = 4,
        )
        Spacer(Modifier.height(8.dp))
        SheetTile("Vote", "how_to_vote_outlined") { close(IssueSheetAction.Vote) }
        SheetTile("Comment", "chat_bubble_outline") { close(IssueSheetAction.Comment) }
        SheetTile(IssueActions.statusLabel(issue), IssueActions.statusIcon(issue)) { close(IssueSheetAction.Status) }
    } ?: return
    when (action) {
        IssueSheetAction.Vote -> run(IssueActions.VOTE_SUBMITTED) {
            inbox.voteOnIssue(repo.owner, repo.name, issue.number)
        }
        IssueSheetAction.Comment -> {
            val body = promptText(IssueActions.commentTitle(issue), multiline = true)
            if (body == null || body.trim().isEmpty()) return
            run(PENDING_NOTE) { inbox.commentOnIssue(repo.owner, repo.name, issue.number, body.trim()) }
        }
        IssueSheetAction.Status -> run(PENDING_NOTE) {
            inbox.setIssueStatus(repo.owner, repo.name, issue.number, IssueActions.nextStatus(issue))
        }
    }
}

private data class PullForm(val title: String, val base: String, val head: String, val patch: String)

internal suspend fun CollabEnv.showNewPullDialog(repo: Repository) {
    if (!requireAccount("open a pull request")) return
    val form = messenger.showDialog<PullForm> { done ->
        var title by remember { mutableStateOf("") }
        var base by remember { mutableStateOf(repo.defaultBranch) }
        var head by remember { mutableStateOf("") }
        var patch by remember { mutableStateOf("") }
        CollabDialog(
            title = "New pull request",
            width = 520.dp,
            onDismiss = { done(null) },
            actions = {
                FmDialogActions(
                    onCancel = { done(null) },
                    onConfirm = { done(PullForm(title, base, head, patch)) },
                    confirmLabel = "Submit",
                )
            },
        ) {
            SessionIdentityBar("Posting")
            Spacer(Modifier.height(8.dp))
            FmField("Title") { FmTextField(title, { title = it }) }
            Spacer(Modifier.height(10.dp))
            Row(verticalAlignment = Alignment.Top) {
                FmField("Base branch", Modifier.weight(1f)) { FmTextField(base, { base = it }) }
                Spacer(Modifier.width(10.dp))
                FmField("Head branch", Modifier.weight(1f)) { FmTextField(head, { head = it }) }
            }
            Spacer(Modifier.height(10.dp))
            FmField("Unified diff (patch)") {
                FmTextField(
                    patch,
                    { patch = it },
                    minLines = 5,
                    maxLines = 14,
                    textStyle = Fm.text.bodySmall.copy(fontFamily = FmType.mono, color = Fm.colors.textPrimary),
                )
            }
        }
    } ?: return
    if (form.title.trim().isEmpty() || form.patch.trim().isEmpty()) return
    run(PENDING_NOTE) {
        inbox.submitNewPull(
            repo.owner,
            repo.name,
            title = form.title.trim(),
            base = form.base.trim(),
            head = form.head.trim(),
            patch = form.patch,
        )
    }
}

private data class DiscussionForm(val title: String, val category: String, val body: String)

internal suspend fun CollabEnv.showNewDiscussionDialog(repo: Repository) {
    if (!requireAccount("start a discussion")) return
    val form = messenger.showDialog<DiscussionForm> { done ->
        var title by remember { mutableStateOf("") }
        var category by remember { mutableStateOf("general") }
        var body by remember { mutableStateOf("") }
        CollabDialog(
            title = "New discussion",
            width = 520.dp,
            onDismiss = { done(null) },
            actions = {
                FmDialogActions(
                    onCancel = { done(null) },
                    onConfirm = { done(DiscussionForm(title, category, body)) },
                    confirmLabel = "Submit",
                )
            },
        ) {
            SessionIdentityBar("Posting")
            Spacer(Modifier.height(8.dp))
            FmField("Title") { FmTextField(title, { title = it }) }
            Spacer(Modifier.height(12.dp))
            FmField("Category") {
                FlowRow(
                    horizontalArrangement = Arrangement.spacedBy(8.dp),
                    verticalArrangement = Arrangement.spacedBy(8.dp),
                ) {
                    for ((key, label) in DISCUSSION_CATEGORY_LABELS) {
                        CollabChoiceChip(label, selected = category == key) { category = key }
                    }
                }
            }
            Spacer(Modifier.height(12.dp))
            FmField("Body") { FmTextField(body, { body = it }, minLines = 5, maxLines = 12) }
            Spacer(Modifier.height(12.dp))
            FmInboxPendingNote()
        }
    } ?: return
    if (form.title.trim().isEmpty() || form.body.trim().isEmpty()) return
    run(PENDING_NOTE) {
        inbox.submitNewDiscussion(
            repo.owner,
            repo.name,
            title = form.title.trim(),
            body = form.body.trim(),
            category = form.category,
        )
    }
}

private enum class PullSheetAction { Comment, Approve, RequestChanges, ReviewComment }

internal suspend fun CollabEnv.showPullActions(repo: Repository, pr: PullRequest) {
    if (!requireAccount("review pull requests")) return
    val action = pullSheet(PullReview.header(pr.number, pr.title), PullReview.subtitle(pr), withReviewComment = false)
        ?: return
    when (action) {
        PullSheetAction.Comment -> commentOnPull(repo, pr.number)
        PullSheetAction.Approve -> review(repo, pr.number, "approve")
        PullSheetAction.RequestChanges -> review(repo, pr.number, "request-changes")
        PullSheetAction.ReviewComment -> review(repo, pr.number, "comment")
    }
}

internal suspend fun CollabEnv.showPublishedPullActions(repo: Repository, pull: PublishedPull) {
    if (!requireAccount("review pull requests")) return
    val action = pullSheet(PullReview.header(pull.number, pull.title), PullReview.subtitle(pull), withReviewComment = true)
        ?: return
    when (action) {
        PullSheetAction.Comment -> commentOnPull(repo, pull.number)
        PullSheetAction.Approve -> review(repo, pull.number, "approve")
        PullSheetAction.RequestChanges -> review(repo, pull.number, "request-changes")
        PullSheetAction.ReviewComment -> review(repo, pull.number, "comment")
    }
}

private suspend fun CollabEnv.pullSheet(header: String, subtitle: String, withReviewComment: Boolean): PullSheetAction? {
    return messenger.showModalBottomSheet<PullSheetAction>(showDragHandle = true, containerColor = palette.bgOverlay) { close ->
        SheetHeader(header, subtitle = subtitle)
        Spacer(Modifier.height(8.dp))
        SheetTile("Comment", "chat_bubble_outline") { close(PullSheetAction.Comment) }
        SheetTile("Approve", "check_circle_outline", iconTint = Fm.colors.success) { close(PullSheetAction.Approve) }
        SheetTile("Request changes", "cancel_outlined", iconTint = Fm.colors.danger) { close(PullSheetAction.RequestChanges) }
        if (withReviewComment) {
            SheetTile("Leave review comment", "rate_review_outlined") { close(PullSheetAction.ReviewComment) }
        }
    }
}

private suspend fun CollabEnv.commentOnPull(repo: Repository, number: Int) {
    val body = promptText(PullReview.commentTitle(number), multiline = true)
    if (body == null || body.trim().isEmpty()) return
    run(PENDING_NOTE) { inbox.commentOnPull(repo.owner, repo.name, number, body.trim()) }
}

private suspend fun CollabEnv.review(repo: Repository, number: Int, state: String) {
    val body = showPullReviewComposer(number, state) ?: return
    run(PENDING_NOTE) { inbox.reviewPull(repo.owner, repo.name, number, state, body) }
}

internal suspend fun CollabEnv.showPullReviewComposer(number: Int, state: String): String? =
    messenger.showDialog<String> { done ->
        var text by remember { mutableStateOf("") }
        var errorText by remember { mutableStateOf("") }
        CollabDialog(
            title = PullReview.composerTitle(state, number),
            width = 520.dp,
            onDismiss = { done(null) },
            actions = {
                FmDialogActions(
                    onCancel = { done(null) },
                    onConfirm = {
                        val body = text.trim()
                        val error = PullReview.validationError(state, body)
                        if (error != null) errorText = error else done(body)
                    },
                    confirmLabel = "Submit review",
                )
            },
        ) {
            SessionIdentityBar("Reviewing")
            Spacer(Modifier.height(8.dp))
            Text(PullReview.composerHelper(state), style = LocalTextStyle.current.withHeight(1.4f))
            Spacer(Modifier.height(12.dp))
            FmTextField(
                value = text,
                onValueChange = { text = it },
                hint = PullReview.REVIEW_HINT,
                errorText = errorText.ifEmpty { null },
                autofocus = true,
                minLines = 4,
                maxLines = 8,
            )
            Spacer(Modifier.height(12.dp))
            FmInboxPendingNote()
        }
    }

internal suspend fun CollabEnv.promptText(title: String, multiline: Boolean = false): String? =
    messenger.showDialog<String> { done ->
        var text by remember { mutableStateOf("") }
        CollabDialog(
            title = title,
            width = 460.dp,
            onDismiss = { done(null) },
            actions = {
                FmDialogActions(onCancel = { done(null) }, onConfirm = { done(text) }, confirmLabel = "Submit")
            },
        ) {
            SessionIdentityBar("Commenting")
            Spacer(Modifier.height(8.dp))
            FmTextField(
                value = text,
                onValueChange = { text = it },
                autofocus = true,
                minLines = if (multiline) 3 else 1,
                maxLines = if (multiline) 8 else 1,
            )
        }
    }
