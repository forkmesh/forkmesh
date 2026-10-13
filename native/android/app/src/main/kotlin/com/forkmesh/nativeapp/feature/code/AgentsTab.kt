package com.forkmesh.nativeapp.feature.code

import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.Stable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.agents.AgentAttachmentRef
import com.forkmesh.nativeapp.agents.AgentTranscriptAttachments
import com.forkmesh.nativeapp.code.RepoActivity
import com.forkmesh.nativeapp.control.desktopAttachments
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.agents.AgentAttachmentStrip
import com.forkmesh.nativeapp.feature.agents.AgentsFeature
import com.forkmesh.nativeapp.feature.account.AuthService
import com.forkmesh.nativeapp.model.AgentSession
import com.forkmesh.nativeapp.model.AgentTranscript
import com.forkmesh.nativeapp.model.Repository
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmSectionHeader
import com.forkmesh.nativeapp.ui.kit.FmStatusBadge
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmSpace
import com.forkmesh.nativeapp.ui.theme.FmType
import kotlinx.coroutines.delay

private const val AGENT_POLL_INTERVAL_MS = 10_000L

@Stable
internal class AgentsTabState(val repo: Repository) {
    val sessions = AsyncValue<List<AgentSession>>()
    var polling by mutableStateOf(false)

    fun ownerAccount(): String =
        RepoActivity.agentOwnerAccount(Services.get<AuthService>()?.session?.value?.nodeName, repo.owner)

    fun load() {
        val account = ownerAccount()
        sessions.reload { repoApi.agentSessions(repo.owner, repo.name, ownerAccount = account) }
    }
}

@Composable
internal fun AgentsTab(state: AgentsTabState, navigator: Navigator) {
    val value = state.sessions
    value.Bind()
    LaunchedEffect(value.data) {
        value.data?.let { state.polling = RepoActivity.hasActiveSessions(it) }
    }
    if (state.polling) {
        LaunchedEffect(Unit) {
            while (true) {
                delay(AGENT_POLL_INTERVAL_MS)
                state.load()
            }
        }
    }
    val c = Fm.colors
    val items = value.data.orEmpty()
    val error = value.error
    val polling = state.polling || RepoActivity.hasActiveSessions(items)
    val now = System.currentTimeMillis()
    LazyColumn(
        Modifier.fillMaxSize(),
        contentPadding = PaddingValues(top = 10.dp, bottom = 10.dp + navigationBarBottom()),
    ) {
        item {
            FmSectionHeader(
                title = "Agent sessions",
                modifier = Modifier.padding(horizontal = FmSpace.x4, vertical = FmSpace.x2),
                trailing = {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        if (value.data != null) {
                            Text(
                                "${items.size}",
                                style = Fm.text.bodySmall.copy(fontWeight = FmType.emphasis, color = c.textSecondary),
                            )
                        }
                        if (polling) {
                            Spacer(Modifier.width(FmSpace.x2))
                            Text("Live refresh on")
                        }
                        Spacer(Modifier.width(FmSpace.x1))
                        FmIconButton("refresh", "Refresh agent sessions", onPressed = { state.load() }, iconSize = 18.dp)
                    }
                },
            )
        }
        when {
            (value.waiting || !value.started) && items.isEmpty() -> item {
                LoadingCard("Loading agent sessions…", Modifier.padding(FmSpace.x4))
            }
            error != null -> item { EmptyCard("$error", Modifier.padding(FmSpace.x4)) }
            items.isEmpty() -> item {
                EmptyCard(
                    "No agent sessions have been pushed by the desktop node for this repo yet.",
                    Modifier.padding(FmSpace.x4),
                )
            }
            else -> dividedRows(items) { session ->
                MobileCard(
                    icon = "smart_toy_outlined",
                    iconColor = c.accent,
                    title = session.displayTitle,
                    subtitle = RepoActivity.agentSubtitle(session),
                    trailing = { AgentStatusBadge(session) },
                    onTap = { navigator.push(AgentSessionDetailScreen(state.repo, session, state.ownerAccount())) },
                    chips = RepoActivity.agentChips(session, now),
                )
            }
        }
    }
}

@Composable
private fun AgentStatusBadge(session: AgentSession) {
    FmStatusBadge(label = session.statusLabel, color = toneColor(RepoActivity.agentStatusTone(session.statusLabel)))
}

internal class AgentSessionDetailScreen(
    val repo: Repository,
    val session: AgentSession,
    private val ownerAccount: String,
) : Screen() {
    private val transcript = AsyncValue<AgentTranscript>().apply {
        reload { repoApi.agentTranscript(repo.owner, repo.name, session.id, ownerAccount = ownerAccount) }
    }
    private val desktopRefs = AsyncValue<List<AgentAttachmentRef>>().apply {
        reload { AgentsFeature.control.desktopAttachments(repo.name, session.id) ?: emptyList() }
    }

    override val title: String get() = "Agent session #${session.id}"

    @Composable
    override fun Content(navigator: Navigator) {
        transcript.Bind()
        desktopRefs.Bind()
        val c = Fm.colors
        val now = System.currentTimeMillis()
        val statusSummary = session.latestStatusSummary(120)
        val resetCountdown = session.rateLimitCountdownLabel(now)
        val failureReason = session.statusFailureReason(400)
        DetailScaffold(title = title, navigator = navigator) {
            LazyColumn(
                Modifier.fillMaxSize(),
                contentPadding = PaddingValues(start = 20.dp, top = 20.dp, end = 20.dp, bottom = 20.dp + navigationBarBottom()),
            ) {
                item {
                    InfoCard(session.displayTitle) {
                        ChipWrap {
                            RepoChip("smart_toy_outlined", session.providerLabel)
                            RepoChip("info_outline", statusSummary)
                            if (resetCountdown.isNotEmpty()) RepoChip("timer_outlined", resetCountdown)
                            if (session.model.isNotEmpty()) RepoChip("memory_outlined", session.model)
                            if (session.issueNumber > 0) RepoChip("error_outline", "#${session.issueNumber}")
                            if (session.branchName.isNotEmpty()) RepoChip("account_tree_outlined", session.branchName)
                            if (session.numTurns > 0) RepoChip("chat_bubble_outline", "${session.numTurns} turns")
                            if (session.durationLabel.isNotEmpty()) RepoChip("timer_outlined", session.durationLabel)
                            if (session.costLabel.isNotEmpty()) RepoChip("paid_outlined", session.costLabel)
                            if (session.prLabel.isNotEmpty()) RepoChip("call_merge_outlined", session.prLabel)
                            if (session.diffLabel.isNotEmpty()) RepoChip("difference_outlined", session.diffLabel)
                            if (session.conflicted) RepoChip("warning_amber_outlined", "Conflict")
                            if (session.tokenLabel.isNotEmpty()) RepoChip("data_usage_outlined", session.tokenLabel)
                        }
                        Spacer(Modifier.height(12.dp))
                        Text(
                            "Desktop-controlled session. Mobile can watch progress and open results; start, stop, retry, " +
                                "resume, merge, and apply stay on the desktop node until signed pairing is designed.",
                            color = c.textSecondary,
                        )
                        if (failureReason.isNotEmpty()) {
                            Spacer(Modifier.height(12.dp))
                            Text(failureReason, color = c.danger)
                        }
                    }
                }
                item { Spacer(Modifier.height(12.dp)) }
                item {
                    val error = transcript.error
                    when {
                        transcript.waiting -> LoadingCard("Loading transcript…")
                        error != null -> EmptyCard("$error")
                        else -> {
                            val detail = transcript.data ?: AgentTranscript(status = "", transcript = "")
                            InfoCard("Transcript") {
                                ChipWrap {
                                    RepoChip("info_outline", RepoActivity.transcriptStatusLabel(session, detail.status))
                                    RepoChip("folder_outlined", repo.fullName)
                                }
                                Spacer(Modifier.height(12.dp))
                                val refs = desktopRefs.data?.takeIf { it.isNotEmpty() }
                                    ?: AgentTranscriptAttachments.references(detail.transcript)
                                if (refs.isNotEmpty()) {
                                    AgentAttachmentStrip(refs)
                                    Spacer(Modifier.height(12.dp))
                                }
                                SelectionContainer {
                                    Text(
                                        RepoActivity.transcriptText(detail.transcript),
                                        style = Fm.text.bodySmall.copy(
                                            fontFamily = FmType.mono,
                                            lineHeight = FmType.meta * 1.45f,
                                            color = c.textPrimary,
                                        ),
                                    )
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
