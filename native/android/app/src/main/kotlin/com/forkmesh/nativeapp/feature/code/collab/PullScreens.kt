@file:OptIn(ExperimentalLayoutApi::class)

package com.forkmesh.nativeapp.feature.code.collab

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.material3.LocalTextStyle
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.ci.RepoRef
import com.forkmesh.nativeapp.collab.CollabChip
import com.forkmesh.nativeapp.collab.DesktopCommand
import com.forkmesh.nativeapp.collab.PullCard
import com.forkmesh.nativeapp.collab.PullDetail
import com.forkmesh.nativeapp.collab.PullShipCommands
import com.forkmesh.nativeapp.collab.PullTimeline
import com.forkmesh.nativeapp.collab.THREAD_SUBSCRIPTION_NOTE
import com.forkmesh.nativeapp.collab.dartErrorText
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.model.PublishedPull
import com.forkmesh.nativeapp.model.Repository
import com.forkmesh.nativeapp.shell.ShellPullsWatch
import com.forkmesh.nativeapp.ui.kit.FmInboxPendingNote
import com.forkmesh.nativeapp.ui.kit.FmPullToRefreshBox
import com.forkmesh.nativeapp.ui.kit.FmInlineStatus
import com.forkmesh.nativeapp.ui.kit.FmStatus
import com.forkmesh.nativeapp.ui.kit.FmTone
import com.forkmesh.nativeapp.ui.kit.FmToneButton
import com.forkmesh.nativeapp.ui.kit.rememberFmStatus
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

private val PUBLISH_RETRY_DELAYS_MS = listOf(1_500L, 3_000L, 6_000L, 12_000L, 24_000L)

@Composable
internal fun PullsTabContent(repo: Repository, navigator: Navigator) {
    val ref = com.forkmesh.nativeapp.navigation.RepoRef(repo.owner, repo.name)
    val watched by ShellPullsWatch.state.collectAsState()
    val rings by ShellPullsWatch.rings.collectAsState()
    val baseline = remember { rings }
    var state by remember(repo.owner, repo.name) { mutableStateOf<Loadable<List<PublishedPull>>>(Loadable.Loading) }
    var refreshing by remember { mutableStateOf(false) }
    val scope = rememberCoroutineScope()

    suspend fun read() {
        try {
            val pulls = withContext(Dispatchers.IO) { Services.require<ApiService>().publishedPulls(repo.owner, repo.name) }
            state = Loadable.Ready(pulls)
            ShellPullsWatch.viewed(ref, pulls)
        } catch (cancelled: CancellationException) {
            throw cancelled
        } catch (error: Exception) {
            if (state !is Loadable.Ready) state = Loadable.Failed(dartErrorText(error))
        }
    }

    suspend fun reload() {
        if (ShellPullsWatch.isCurrent(ref)) ShellPullsWatch.refresh() else read()
    }

    DisposableEffect(ref) {
        ShellPullsWatch.opened(ref)
        onDispose { ShellPullsWatch.closed(ref) }
    }
    LaunchedEffect(ref) { read() }
    LaunchedEffect(watched.revision) {
        if (watched.holds(ref) && state is Loadable.Ready) state = Loadable.Ready(watched.pulls)
    }
    LaunchedEffect(rings) {
        if (rings != baseline && !ShellPullsWatch.isCurrent(ref)) read()
    }
    FmPullToRefreshBox(
        isRefreshing = refreshing,
        onRefresh = {
            scope.launch {
                refreshing = true
                try {
                    reload()
                } finally {
                    refreshing = false
                }
            }
        },
    ) {
        AsyncListBody(state, empty = "No published pull requests yet.") { pull ->
            MobileCard(
                icon = "call_merge",
                iconColor = toneColor(PullCard.tone(pull)),
                title = pull.title,
                subtitle = PullCard.subtitle(pull),
                badge = PullCard.badge(pull),
                onTap = { navigator.push(PullDetailScreen(repo, pull.number, pull)) },
            )
        }
    }
}

internal class PullDetailScreen(
    private val repo: Repository,
    private val number: Int,
    initial: PublishedPull?,
) : Screen() {
    private var pull by mutableStateOf(initial)
    private var loadError by mutableStateOf<String?>(null)
    private var loaded by mutableStateOf(initial != null)
    private var loadStarted = initial != null
    private var loadJob: Job? = null
    private val subscribed = mutableStateOf(false)

    override val title: String get() = "PR #${pull?.number ?: number}"

    @Composable
    override fun Content(navigator: Navigator) {
        val env = rememberCollabEnv(navigator)
        LaunchedEffect(Unit) {
            if (!loadStarted) {
                loadStarted = true
                loadJob = env.launch { load() }
            }
        }
        CollabScaffold(topBar = { CollabTopBar(navigator, title = title) }) {
            val current = pull
            val error = loadError
            when {
                current != null -> Detail(env, current)
                error != null -> MessageList(padding = 20.dp) { EmptyCard(error) }
                !loaded -> MessageList(padding = 20.dp) { LoadingCard("Loading…") }
                else -> MessageList(padding = 20.dp) { EmptyCard("Pull request #$number is not published.") }
            }
        }
    }

    override fun onDisposed() {
        loadJob?.cancel()
    }

    private suspend fun CollabEnv.load() {
        try {
            var found = withContext(Dispatchers.IO) { api.publishedPull(repo.owner, repo.name, number) }
            for (wait in PUBLISH_RETRY_DELAYS_MS) {
                if (found != null) break
                delay(wait)
                found = withContext(Dispatchers.IO) { api.publishedPull(repo.owner, repo.name, number) }
            }
            pull = found
        } catch (error: CancellationException) {
            throw error
        } catch (error: Exception) {
            loadError = dartErrorText(error)
        } finally {
            loaded = true
        }
    }

    @Composable
    private fun Detail(env: CollabEnv, pull: PublishedPull) {
        val colors = Fm.colors
        val canRequestMerge = PullDetail.canRequestMerge(pull)
        val desktopStatus = rememberFmStatus()
        var desktopRoute by remember(repo.fullName) {
            mutableStateOf(com.forkmesh.nativeapp.control.DesktopCommandAvailability.shared.available)
        }
        LaunchedEffect(repo.fullName) {
            desktopRoute = withContext(Dispatchers.IO) {
                com.forkmesh.nativeapp.control.DesktopCommandAvailability.shared.refresh {
                    env.api.desktopCommandRouteAnswered(repo.owner, repo.name)
                }
            }
        }
        LazyColumn(Modifier.fillMaxSize(), contentPadding = PaddingValues(20.dp)) {
            item {
                InfoCard(pull.title) {
                    FlowRow(
                        horizontalArrangement = Arrangement.spacedBy(8.dp),
                        verticalArrangement = Arrangement.spacedBy(8.dp),
                    ) {
                        ChipView(CollabChip("label_outline", pull.status))
                        if (pull.signed) ChipView(CollabChip("verified_outlined", "signed"))
                        PullDetail.branchesChip(pull)?.let { ChipView(CollabChip("account_tree_outlined", it)) }
                    }
                    Spacer(Modifier.height(10.dp))
                    FmInboxPendingNote()
                    Spacer(Modifier.height(16.dp))
                    SelectableBody(PullDetail.body(pull), style = LocalTextStyle.current.withHeight(1.45f), repo = RepoRef(repo.owner, repo.name))
                }
            }
            item {
                Spacer(Modifier.height(12.dp))
                com.forkmesh.nativeapp.feature.code.review.PullWorkflowCards(repo, pull, env.navigator)
            }
            if (pull.patch.isNotEmpty()) {
                item {
                    Spacer(Modifier.height(12.dp))
                    PatchFilesCard(pull.patch)
                }
            }
            item {
                Spacer(Modifier.height(12.dp))
                InfoCard("Patch") {
                    if (pull.patch.isEmpty()) {
                        Text(PullDetail.NO_PATCH)
                    } else {
                        Column {
                            com.forkmesh.nativeapp.feature.code.OpenDiffViewerLink {
                                env.navigator.push(com.forkmesh.nativeapp.feature.code.PatchFilesScreen("Pull #${pull.number}", pull.patch))
                            }
                            DiffViewer(pull.patch)
                        }
                    }
                }
            }
            item {
                Spacer(Modifier.height(12.dp))
                InfoCard("Timeline") { PullTimelineView(pull, RepoRef(repo.owner, repo.name)) }
            }
            if (!repo.isPrivate) {
                item {
                    Spacer(Modifier.height(12.dp))
                    FederatedRepliesCard(repo, kind = "pull", number = pull.number)
                }
            }
            if (desktopRoute) item {
                Spacer(Modifier.height(12.dp))
                InfoCard("On the desktop") {
                    Text(PullDetail.shipControlsCopy(pull), color = colors.textSecondary)
                    Spacer(Modifier.height(10.dp))
                    FlowRow(
                        horizontalArrangement = Arrangement.spacedBy(8.dp),
                        verticalArrangement = Arrangement.spacedBy(8.dp),
                    ) {
                        for (command in PullShipCommands.offered(pull)) {
                            val onClick = if (command.needsOpen && !canRequestMerge) {
                                null
                            } else {
                                {
                                    env.launch {
                                        requestDesktopCommand(
                                            repo = repo,
                                            command = command.command,
                                            target = PullDetail.commandTarget(pull),
                                            payload = PullDetail.commandPayload(pull),
                                            successMessage = command.successMessage,
                                            status = desktopStatus,
                                        )
                                    }
                                    Unit
                                }
                            }
                            FmToneButton(
                                label = command.chipLabel,
                                onPressed = onClick,
                                icon = command.icon,
                                tone = when (command.command) {
                                    "pull.merge", "pull.merge_delete" -> FmTone.Success
                                    "pull.fix_agent", "pull.reopen" -> FmTone.Accent
                                    "pull.close", "pull.delete" -> FmTone.Danger
                                    else -> FmTone.Neutral
                                },
                                tooltip = command.label,
                            )
                        }
                    }
                    FmInlineStatus(desktopStatus, Modifier.padding(top = 8.dp))
                }
            }
            item {
                Spacer(Modifier.height(12.dp))
                InfoCard("Review actions") {
                    Text(PullDetail.REVIEW_ACTIONS_COPY)
                    Spacer(Modifier.height(8.dp))
                    FmInboxPendingNote()
                    Spacer(Modifier.height(8.dp))
                    Text(THREAD_SUBSCRIPTION_NOTE)
                    Spacer(Modifier.height(12.dp))
                    FlowRow(
                        horizontalArrangement = Arrangement.spacedBy(10.dp),
                        verticalArrangement = Arrangement.spacedBy(10.dp),
                    ) {
                        CollabFilledButton(
                            label = "Review / comment",
                            icon = "rate_review_outlined",
                            onClick = { env.launch { showPublishedPullActions(repo, pull) } },
                        )
                        ThreadSubscriptionButton(env, repo, source = "pull", number = pull.number, subscribed = subscribed)
                    }
                    FmInlineStatus(env.status, Modifier.padding(top = 8.dp))
                }
            }
            item { NavigationBarSpacer() }
        }
    }
}

@Composable
private fun PullTimelineView(pull: PublishedPull, repo: RepoRef) {
    if (pull.events.isEmpty()) {
        Text(PullDetail.TIMELINE_EMPTY)
        return
    }
    Column(Modifier.fillMaxWidth()) {
        for (event in pull.events) {
            TimelineTile(
                icon = PullTimeline.icon(event),
                iconTint = toneColor(PullTimeline.tone(event)),
                title = event.displayTitle,
                author = event.displayAuthor,
                body = event.body,
                repo = repo,
            )
        }
    }
}

internal suspend fun CollabEnv.requestDesktopCommand(
    repo: Repository,
    command: String,
    target: String,
    payload: Map<String, Any?>,
    successMessage: String,
    failureMessage: String = DesktopCommand.FAILURE_MESSAGE,
    status: FmStatus = this.status,
) {
    if (!com.forkmesh.nativeapp.control.DesktopCommandContract.isValid(command, target)) {
        status.error(com.forkmesh.nativeapp.control.DesktopCommandContract.UNSUPPORTED_MESSAGE)
        return
    }
    if (!requireAccount("request desktop control")) return
    try {
        val session = auth?.session?.value
        val signer = identity
        val account = DesktopCommand.ownerAccount(session?.nodeName, session?.pubkey, signer?.publicKeyB64url)
        val repoOwner = com.forkmesh.nativeapp.code.OwnerProofs.desktopCommandOwner(repo, account)
        val ts = System.currentTimeMillis().toString()
        val result = withContext(Dispatchers.IO) {
            val canonical = DesktopCommand.canonical(repoOwner, repo.name, command, target, ts)
            val sig = signer!!.sign(canonical.toByteArray(Charsets.UTF_8))
            api.desktopCommand(repoOwner, repo.name, account, command, target, ts, sig, payload)
        }
        if (result.ok) status.show(successMessage) else status.error(failureMessage)
    } catch (error: CancellationException) {
        throw error
    } catch (error: Exception) {
        status.error(dartErrorText(error))
    }
}
