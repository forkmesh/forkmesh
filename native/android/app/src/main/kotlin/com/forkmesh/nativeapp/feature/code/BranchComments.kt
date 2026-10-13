package com.forkmesh.nativeapp.feature.code

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.text.selection.DisableSelection
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.code.diff.DiffLine
import com.forkmesh.nativeapp.collab.BranchReviewSession
import com.forkmesh.nativeapp.collab.ReviewThread
import com.forkmesh.nativeapp.feature.code.review.Note
import com.forkmesh.nativeapp.feature.code.review.ThreadCard
import com.forkmesh.nativeapp.feature.code.review.io
import com.forkmesh.nativeapp.ui.kit.FmInlineStatus
import com.forkmesh.nativeapp.ui.kit.FmStatus
import com.forkmesh.nativeapp.ui.kit.FmTextField
import com.forkmesh.nativeapp.ui.kit.FmTone
import com.forkmesh.nativeapp.ui.kit.FmToneButton
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmSpace
import kotlinx.coroutines.launch

internal class BranchComments(val session: BranchReviewSession) {
    var version by mutableIntStateOf(0)
        private set

    val status = FmStatus()

    val canComment: Boolean get() = session.canWrite

    fun bump() {
        version++
    }

    fun tap(path: String, line: DiffLine) {
        if (!canComment) return
        session.tap(path, line)
        bump()
    }

    fun isSelected(path: String, line: DiffLine?): Boolean = line != null && session.isSelected(path, line)

    suspend fun reload(patch: String, headOid: String) {
        io { session.attach(patch, headOid) }
        bump()
        io { session.load() }
        bump()
    }

    val summary: String
        get() {
            val snap = session.snapshot
            val threads = if (snap.totalThreads == 0) "No comments on this branch yet" else
                "${snap.unresolvedThreads} open · ${snap.resolvedThreads} resolved" +
                    if (snap.outdatedThreads > 0) " · ${snap.outdatedThreads} outdated" else ""
            val hint = if (canComment) "Tap a line number to comment; tap another to cover a range." else "Sign in on this device to comment."
            return "$threads. $hint"
        }
}

@Composable
internal fun BranchThreads(comments: BranchComments, threads: List<ReviewThread>, modifier: Modifier, label: String = "") {
    if (threads.isEmpty()) return
    DisableSelection {
        Column(
            modifier
                .background(Fm.colors.bgBase)
                .padding(horizontal = FmSpace.x3, vertical = FmSpace.x2),
            verticalArrangement = Arrangement.spacedBy(FmSpace.x2),
        ) {
            if (label.isNotEmpty()) Note(label)
            for (thread in threads) ThreadCard(comments.session, thread) { comments.bump() }
        }
    }
}

@Composable
internal fun BranchComposer(comments: BranchComments) {
    if (comments.version < 0) return
    val session = comments.session
    val selection = session.selection
    if (selection == null || !comments.canComment) {
        if (comments.status.message == null) return
        Box(
            Modifier
                .fillMaxWidth()
                .height(1.dp)
                .background(Fm.colors.border),
        )
        FmInlineStatus(
            comments.status,
            Modifier
                .background(Fm.colors.bgRaised)
                .padding(horizontal = FmSpace.x3, vertical = FmSpace.x1),
        )
        return
    }
    val scope = rememberCoroutineScope()
    var body by remember { mutableStateOf("") }
    var sending by remember { mutableStateOf(false) }
    Box(
        Modifier
            .fillMaxWidth()
            .height(1.dp)
            .background(Fm.colors.border),
    )
    Column(
        Modifier
            .fillMaxWidth()
            .background(Fm.colors.bgRaised)
            .padding(FmSpace.x3),
        verticalArrangement = Arrangement.spacedBy(FmSpace.x2),
    ) {
        Note(selection.label)
        FmTextField(body, { body = it }, modifier = Modifier.fillMaxWidth(), hint = "Comment on ${session.branch}", minLines = 2, maxLines = 6)
        Row(horizontalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
            FmToneButton(if (sending) "Sending…" else "Comment", onPressed = {
                if (sending) return@FmToneButton
                sending = true
                scope.launch {
                    val notice = io { session.commentNow(body) }
                    sending = false
                    if (!notice.isError) body = ""
                    comments.status.report(notice.message, notice.isError)
                    comments.bump()
                }
            }, tone = FmTone.Accent, filled = true)
            FmToneButton("Cancel", onPressed = {
                session.clearSelection()
                comments.bump()
            })
        }
        FmInlineStatus(comments.status)
    }
}
