package com.forkmesh.nativeapp.feature.agents

import com.forkmesh.nativeapp.composer.PromptDraftStore
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.platform.LocalUriHandler
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.agents.AgentAssistantMessage
import com.forkmesh.nativeapp.agents.AgentCredentials
import com.forkmesh.nativeapp.agents.AgentDesktopHandoff
import com.forkmesh.nativeapp.agents.AgentDesktopMoveException
import com.forkmesh.nativeapp.agents.AgentDone
import com.forkmesh.nativeapp.agents.AgentEvent
import com.forkmesh.nativeapp.agents.AgentModelCatalog
import com.forkmesh.nativeapp.agents.AgentNotice
import com.forkmesh.nativeapp.agents.AgentProviderKind
import com.forkmesh.nativeapp.agents.AgentRunHandle
import com.forkmesh.nativeapp.agents.AgentStartException
import com.forkmesh.nativeapp.agents.AgentStopReason
import com.forkmesh.nativeapp.agents.AgentToolCallEvent
import com.forkmesh.nativeapp.agents.AgentToolResultEvent
import com.forkmesh.nativeapp.agents.AgentUserMessage
import com.forkmesh.nativeapp.agents.CodingAgent
import com.forkmesh.nativeapp.agents.HubAgentHandoffDesktop
import com.forkmesh.nativeapp.agents.PromptAttachment
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.account.AccountFeature
import com.forkmesh.nativeapp.feature.code.DetailScaffold
import com.forkmesh.nativeapp.feature.code.FmFilledButton
import com.forkmesh.nativeapp.localrepos.GitWorkspace
import com.forkmesh.nativeapp.localrepos.LocalRepoRecord
import com.forkmesh.nativeapp.localrepos.LocalRepoStore
import com.forkmesh.nativeapp.localrepos.PullRequestService
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmInlineStatus
import com.forkmesh.nativeapp.ui.kit.FmMessageActions
import com.forkmesh.nativeapp.ui.kit.FmMessenger
import com.forkmesh.nativeapp.ui.kit.FmSegmentTab
import com.forkmesh.nativeapp.ui.kit.FmSegmentTabBar
import com.forkmesh.nativeapp.ui.kit.FmStatus
import com.forkmesh.nativeapp.ui.kit.FmTextField
import com.forkmesh.nativeapp.ui.kit.LinkedText
import com.forkmesh.nativeapp.ui.kit.LocalFmMessenger
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmRadius
import com.forkmesh.nativeapp.ui.theme.FmSpace
import com.forkmesh.nativeapp.ui.theme.FmType
import java.io.File
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

internal class AgentScreen(
    private val record: LocalRepoRecord,
    initialPrompt: String = "",
    initialAttachments: List<PromptAttachment> = emptyList(),
    private val autoStart: Boolean = false,
) : Screen() {
    private var prompt by mutableStateOf(initialPrompt)
    private var attachments by mutableStateOf(initialAttachments)
    private var credentials by mutableStateOf<AgentCredentials?>(null)
    private var loading by mutableStateOf(true)
    private var error by mutableStateOf<String?>(null)
    private var revision by mutableIntStateOf(0)
    private var prBusy by mutableStateOf(false)
    private var moving by mutableStateOf(false)
    private var movedBranch by mutableStateOf<String?>(null)
    private val status = FmStatus()
    private var autoStarted = false
    private val prOpenedFor = HashSet<String>()
    private val workspace: GitWorkspace by lazy {
        Services.get<LocalRepoStore>()?.workspace(record) ?: GitWorkspace(File(record.path))
    }

    override val title: String get() = record.name

    private val sessions get() = AgentsFeature.sessions
    private val handle: AgentRunHandle? get() = sessions.lastRun?.takeIf { it.task.repoName == record.name }

    private fun prCandidate(): AgentRunHandle? {
        val run = handle ?: return null
        if (!run.isComplete || Services.get<PullRequestService>() == null || !record.isPublished) return null
        if (run.branch in prOpenedFor) return null
        val done = run.events.filterIsInstance<AgentDone>().lastOrNull() ?: return null
        if (done.reason == AgentStopReason.Error || done.reason == AgentStopReason.Refused) return null
        return run
    }

    @Composable
    override fun Content(navigator: Navigator) {
        val c = Fm.colors
        val scope = rememberCoroutineScope()
        val messenger = LocalFmMessenger.current
        val requireAccount = AccountFeature.rememberRequireAccount()
        DisposableEffect(Unit) {
            val listener: () -> Unit = { revision++ }
            sessions.addListener(listener)
            onDispose { sessions.removeListener(listener) }
        }

        suspend fun openSetup() {
            if (!requireAccount("set up the coding agent")) return
            val saved = showAgentSetupSheet(messenger, credentials)
            if (saved != null) credentials = saved
        }

        suspend fun send() {
            val text = prompt.trim()
            val images = attachments.toList()
            if (text.isEmpty() && images.isEmpty()) return
            if (!requireAccount("run the coding agent")) return
            val current = credentials
            if (current == null) {
                openSetup()
                return
            }
            val task = sessions.submit(record.name, text, images = images)
            prompt = ""
            attachments = emptyList()
            if (task == null) return
            error = null
            try {
                withContext(Dispatchers.IO) {
                    sessions.start(task, workspace, { toolset -> CodingAgent(current.build(), toolset) }, AgentsFeature.author())
                }
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (failure: AgentStartException) {
                error = failure.message
            } catch (failure: Exception) {
                error = failure.message ?: failure.toString()
            }
        }

        LaunchedEffect(Unit) {
            credentials = withContext(Dispatchers.IO) { runCatching { AgentsFeature.credentials.read() }.getOrNull() }
            loading = false
            if (autoStart && !autoStarted && (prompt.isNotBlank() || attachments.isNotEmpty())) {
                autoStarted = true
                send()
            }
        }

        val running = sessions.isRunning
        revision.let { }
        val events = handle?.events ?: emptyList()
        DetailScaffold(
            record.name,
            navigator,
            actions = {
                if (running) FmIconButton("stop_circle_outlined", "Stop", onPressed = { sessions.cancelActive() }, color = c.danger)
                val watch = AgentWatchEntry.openDevice
                if (events.isNotEmpty() && watch != null) {
                    FmIconButton("graphic_eq", "Watch, hear and feel it", onPressed = { watch(navigator, record.name) }, color = c.textSecondary)
                }
                if (!moving && Services.get<PullRequestService>() != null && AgentDesktopHandoff.canMove(handle, record, movedBranch)) {
                    FmIconButton("desktop_windows_outlined", "Continue on desktop", onPressed = {
                        scope.launch { if (requireAccount("move the agent to your desktop")) moveToDesktop(messenger) }
                    }, color = c.textSecondary)
                }
                FmIconButton("tune", "Agent settings", onPressed = { scope.launch { openSetup() } }, color = c.textSecondary)
            },
        ) {
            Column(Modifier.fillMaxSize()) {
                error?.let {
                    Row(Modifier.fillMaxWidth().padding(FmSpace.x4), verticalAlignment = Alignment.CenterVertically) {
                        Text(it, Modifier.weight(1f), style = Fm.text.bodySmall.copy(color = c.danger))
                        FmIconButton("close", "Dismiss", onPressed = { error = null }, color = c.textSecondary)
                    }
                }
                FmInlineStatus(status, Modifier.padding(horizontal = FmSpace.x4, vertical = FmSpace.x2))
                val candidate = prCandidate()
                if (candidate != null) {
                    Row(
                        Modifier.fillMaxWidth().background(c.bgOverlay).padding(FmSpace.x3),
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        Text("The work is on ${candidate.branch}.", Modifier.weight(1f), style = Fm.text.bodySmall.copy(color = c.textSecondary))
                        FmFilledButton(if (prBusy) "Preparing…" else "Open pull request", onClick = if (prBusy) null else ({
                            scope.launch { if (requireAccount("open a pull request")) offerPullRequest(messenger, candidate) }
                        }))
                    }
                }
                Box(Modifier.weight(1f).fillMaxWidth()) {
                    when {
                        loading -> Text("Loading session…", Modifier.padding(FmSpace.x5), style = Fm.text.bodyMedium.copy(color = c.textSecondary))
                        credentials == null -> NotConfigured { scope.launch { openSetup() } }
                        events.isEmpty() -> Idle()
                        else -> Transcript(events)
                    }
                }
                AgentPromptComposer(
                    text = prompt,
                    onTextChange = { prompt = it },
                    draftSurface = PromptDraftStore.agentSurface("device/${record.name}"),
                    hint = if (running) "Add an instruction while it works…" else "Tell the agent what to do",
                    onSend = { scope.launch { send() } },
                    running = running,
                    onStop = { sessions.cancelActive() },
                    attachments = attachments,
                    onAttachmentsChanged = { attachments = it },
                    modelLabel = credentials?.let { AgentModelCatalog.shortLabel(it.model) } ?: "Choose a model",
                    onPickModel = { scope.launch { openSetup() } },
                    extraActions = {
                        FmIconButton("attach_file", "Reference a file", onPressed = {
                            scope.launch {
                                val path = pickFile(messenger, workspace)
                                if (path != null) prompt = (if (prompt.isEmpty() || prompt.endsWith(" ") || prompt.endsWith("\n")) prompt else "$prompt ") + "`$path` "
                            }
                        }, color = c.textSecondary)
                    },
                )
            }
        }
    }

    private suspend fun moveToDesktop(messenger: FmMessenger) {
        val run = handle ?: return
        val service = Services.get<PullRequestService>() ?: return
        val desktop = HubAgentHandoffDesktop { AgentsFeature.hub }
        val name = desktop.desktopName
        val body = (if (sessions.isRunning) "The agent stops here. " else "") +
            "Its work is committed and pushed, and the desktop picks it up on the same branch with the conversation so far."
        if (!messenger.showConfirmDialog("Continue on $name?", body, confirmLabel = "Continue on desktop")) return
        moving = true
        error = null
        status.clear()
        try {
            val move = withContext(Dispatchers.IO) {
                AgentDesktopHandoff.move(run, sessions, workspace, record, service, desktop, AgentsFeature.author())
            }
            movedBranch = move.branch
            status.show(move.message)
        } catch (cancelled: CancellationException) {
            throw cancelled
        } catch (failure: AgentDesktopMoveException) {
            error = failure.message
        } catch (failure: Exception) {
            error = failure.message ?: failure.toString()
        } finally {
            moving = false
        }
    }

    private suspend fun offerPullRequest(messenger: FmMessenger, run: AgentRunHandle) {
        val service = Services.get<PullRequestService>() ?: return
        prBusy = true
        val preparation = try {
            withContext(Dispatchers.IO) { service.prepare(workspace, record, run.branch, run.baseBranch) }
        } catch (cancelled: CancellationException) {
            throw cancelled
        } catch (failure: Exception) {
            error = "Could not inspect the branch: ${failure.message ?: failure}"
            prBusy = false
            return
        }
        prBusy = false
        val suggested = run.task.prompt.split('\n').first().trim()
        if (!preparation.isReady) {
            messenger.showConfirmDialog("Cannot open a pull request", preparation.summary, hideCancel = true)
            return
        }
        val title = messenger.showPromptDialog(
            "Open pull request",
            body = preparation.summary,
            hint = "Title",
            initialValue = suggested,
            confirmLabel = "Open",
        ) ?: return
        prBusy = true
        status.clear()
        try {
            withContext(Dispatchers.IO) { service.submit(workspace, preparation, title.ifEmpty { suggested }) }
            prOpenedFor.add(run.branch)
            status.show("Pull request opened: ${preparation.summary}")
        } catch (cancelled: CancellationException) {
            throw cancelled
        } catch (failure: Exception) {
            error = failure.message ?: failure.toString()
        } finally {
            prBusy = false
        }
    }

    @Composable
    private fun NotConfigured(onConfigure: () -> Unit) {
        val c = Fm.colors
        Column(Modifier.fillMaxSize().padding(FmSpace.x5), verticalArrangement = Arrangement.Center, horizontalAlignment = Alignment.CenterHorizontally) {
            FmIcon("smart_toy_outlined", size = 40.dp, tint = c.textTertiary)
            Spacer(Modifier.height(FmSpace.x3))
            Text("Set up the coding agent", style = Fm.text.titleMedium.copy(color = c.textPrimary))
            Spacer(Modifier.height(FmSpace.x2))
            Text(
                "It runs on this phone with your own Claude or OpenAI key, which stays in this device's keystore and goes only to that provider.",
                style = Fm.text.bodySmall.copy(color = c.textSecondary),
            )
            Spacer(Modifier.height(FmSpace.x4))
            FmFilledButton("Add a key", onClick = onConfigure, icon = "key_outlined")
        }
    }

    @Composable
    private fun Idle() {
        val c = Fm.colors
        Column(Modifier.fillMaxSize().padding(FmSpace.x5), verticalArrangement = Arrangement.Center, horizontalAlignment = Alignment.CenterHorizontally) {
            Text("What should the agent change?", style = Fm.text.titleMedium.copy(color = c.textPrimary))
            Spacer(Modifier.height(FmSpace.x2))
            Text(
                "It works on its own branch in this repository, with no shell: it reads, edits, searches and commits. " +
                    "Afterwards it can open a pull request.",
                style = Fm.text.bodySmall.copy(color = c.textSecondary),
            )
        }
    }

    @Composable
    private fun Transcript(events: List<AgentEvent>) {
        val list = rememberLazyListState()
        LaunchedEffect(events.size) { if (events.isNotEmpty()) list.animateScrollToItem(events.size - 1) }
        LazyColumn(
            Modifier.fillMaxSize(),
            state = list,
            contentPadding = PaddingValues(FmSpace.x4),
            verticalArrangement = Arrangement.spacedBy(FmSpace.x2),
        ) {
            itemsIndexed(events) { _, event -> EventTile(event) }
        }
    }
}

@Composable
internal fun EventTile(event: AgentEvent) {
    val c = Fm.colors
    when (event) {
        is AgentUserMessage -> Box(Modifier.fillMaxWidth(), contentAlignment = Alignment.CenterEnd) {
            FmMessageActions(event.text, Modifier.clip(RoundedCornerShape(FmRadius.md))) {
                Column(Modifier.clip(RoundedCornerShape(FmRadius.md)).background(c.accent.copy(alpha = .12f)).padding(FmSpace.x3)) {
                    LinkedText(event.text, null, style = Fm.text.bodyMedium.copy(color = c.textPrimary))
                    if (event.imageCount > 0) {
                        Text("${event.imageCount} photo${if (event.imageCount == 1) "" else "s"}", style = Fm.text.labelSmall.copy(color = c.textTertiary))
                    }
                }
            }
        }
        is AgentAssistantMessage -> FmMessageActions(event.text, Modifier.fillMaxWidth()) {
            LinkedText(event.text, null, style = Fm.text.bodyMedium.copy(color = c.textPrimary))
        }
        is AgentToolCallEvent -> {
            val target = event.call.arguments["path"] ?: event.call.arguments["pattern"] ?: ""
            Row(verticalAlignment = Alignment.CenterVertically) {
                FmIcon("build_outlined", size = 14.dp, tint = c.textTertiary)
                Spacer(Modifier.width(FmSpace.x1))
                Text("${event.call.name} $target".trim(), style = Fm.text.labelSmall.copy(color = c.textSecondary, fontFamily = FmType.mono))
            }
        }
        is AgentToolResultEvent -> {
            var open by remember { mutableStateOf(false) }
            Column(
                Modifier
                    .fillMaxWidth()
                    .clip(RoundedCornerShape(FmRadius.sm))
                    .background(c.bgOverlay)
                    .clickable { open = !open }
                    .padding(FmSpace.x2),
            ) {
                val firstLine = event.result.output.lineSequence().firstOrNull() ?: ""
                Text(
                    if (open) event.result.output else firstLine,
                    maxLines = if (open) Int.MAX_VALUE else 1,
                    style = Fm.text.labelSmall.copy(color = if (event.result.isError) c.danger else c.textSecondary, fontFamily = FmType.mono),
                )
            }
        }
        is AgentNotice -> Text(event.text, style = Fm.text.bodySmall.copy(color = if (event.isError) c.danger else c.textTertiary))
        is AgentDone -> {
            val label = when (event.reason) {
                AgentStopReason.Completed -> "Finished"
                AgentStopReason.Cancelled -> "Stopped"
                AgentStopReason.TurnLimit -> "Stopped at the turn limit"
                AgentStopReason.Error -> "Stopped on an error"
                AgentStopReason.Refused -> "The model declined"
            }
            Text(
                "$label · ${event.turns} turns · ${event.inputTokens + event.outputTokens} tokens",
                style = Fm.text.labelSmall.copy(color = c.textTertiary),
            )
        }
    }
}

internal suspend fun showAgentSetupSheet(messenger: FmMessenger, initial: AgentCredentials?): AgentCredentials? =
    messenger.showModalBottomSheet(isScrollControlled = true) { close ->
        AgentSetupSheet(initial, close)
    }

@Composable
private fun AgentSetupSheet(initial: AgentCredentials?, close: (AgentCredentials?) -> Unit) {
    val c = Fm.colors
    val scope = rememberCoroutineScope()
    val uri = LocalUriHandler.current
    var kind by remember { mutableStateOf(initial?.kind ?: AgentProviderKind.Anthropic) }
    var key by remember { mutableStateOf(initial?.apiKey ?: "") }
    var model by remember { mutableStateOf(initial?.model ?: kind.models.first().id) }
    var busy by remember { mutableStateOf(false) }
    var problem by remember { mutableStateOf<String?>(null) }
    Column(Modifier.fillMaxWidth().verticalScroll(rememberScrollState()).padding(FmSpace.x4)) {
        Text("Coding agent", style = Fm.text.titleMedium.copy(color = c.textPrimary))
        Spacer(Modifier.height(FmSpace.x3))
        FmSegmentTabBar(
            AgentProviderKind.entries.map { FmSegmentTab(it.label) },
            AgentProviderKind.entries.indexOf(kind),
            onChanged = {
                kind = AgentProviderKind.entries[it]
                model = kind.models.first().id
                problem = null
            },
        )
        Spacer(Modifier.height(FmSpace.x3))
        FmTextField(
            value = key,
            onValueChange = {
                key = it
                problem = null
            },
            modifier = Modifier.fillMaxWidth(),
            hint = "API key",
            helperText = "Stored in this device's keystore; sent only to ${kind.label}.",
            errorText = problem,
            visualTransformation = PasswordVisualTransformation(),
        )
        Text(
            "Get a key",
            Modifier.clickable { runCatching { uri.openUri(kind.consoleUrl) } }.padding(vertical = FmSpace.x2),
            style = Fm.text.bodySmall.copy(color = c.accent),
        )
        Spacer(Modifier.height(FmSpace.x2))
        for (option in kind.models) {
            Row(
                Modifier.fillMaxWidth().clickable { model = option.id }.padding(vertical = FmSpace.x2),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Column(Modifier.weight(1f)) {
                    Text(option.label, style = Fm.text.bodyMedium.copy(color = c.textPrimary))
                    Text(option.note, style = Fm.text.bodySmall.copy(color = c.textSecondary))
                }
                if (option.id == model) FmIcon("check", size = 18.dp, tint = c.accent)
            }
        }
        Spacer(Modifier.height(FmSpace.x3))
        Row(verticalAlignment = Alignment.CenterVertically) {
            if (initial != null) {
                com.forkmesh.nativeapp.feature.code.FmOutlinedButton("Remove key", onClick = {
                    scope.launch {
                        withContext(Dispatchers.IO) { AgentsFeature.credentials.clear() }
                        close(null)
                    }
                })
            }
            Spacer(Modifier.weight(1f))
            FmFilledButton(if (busy) "Checking…" else "Save", onClick = if (busy) null else ({
                if (!kind.looksLikeKey(key)) {
                    problem = "That does not look like a ${kind.label} key."
                } else {
                    busy = true
                    scope.launch {
                        val credentials = AgentCredentials(kind, key.trim(), model)
                        val verdict = withContext(Dispatchers.IO) { AgentsFeature.credentials.verify(credentials) }
                        busy = false
                        if (verdict != null) {
                            problem = verdict
                        } else {
                            withContext(Dispatchers.IO) { AgentsFeature.credentials.write(credentials) }
                            close(credentials)
                        }
                    }
                }
            }))
        }
    }
}

private suspend fun pickFile(messenger: FmMessenger, workspace: GitWorkspace): String? =
    messenger.showModalBottomSheet(isScrollControlled = true) { close ->
        var paths by remember { mutableStateOf<List<String>?>(null) }
        var filter by remember { mutableStateOf("") }
        LaunchedEffect(Unit) {
            paths = withContext(Dispatchers.IO) {
                val found = ArrayList<String>()
                val queue = ArrayDeque(listOf(""))
                while (queue.isNotEmpty() && found.size < 2000) {
                    val dir = queue.removeFirst()
                    for (entry in runCatching { workspace.listFiles(dir) }.getOrDefault(emptyList())) {
                        if (entry.isDirectory) queue.add(entry.path) else found.add(entry.path)
                    }
                }
                found.sorted()
            }
        }
        Column(Modifier.fillMaxWidth().padding(FmSpace.x4)) {
            Text("Reference a file", style = Fm.text.titleMedium.copy(color = Fm.colors.textPrimary))
            Spacer(Modifier.height(FmSpace.x2))
            FmTextField(value = filter, onValueChange = { filter = it }, modifier = Modifier.fillMaxWidth(), hint = "Filter")
            Spacer(Modifier.height(FmSpace.x2))
            val shown = (paths ?: emptyList()).filter { filter.isBlank() || it.contains(filter.trim(), ignoreCase = true) }.take(200)
            LazyColumn(Modifier.fillMaxWidth().height(360.dp)) {
                itemsIndexed(shown) { _, path ->
                    Text(
                        path,
                        Modifier.fillMaxWidth().clickable { close(path) }.padding(vertical = FmSpace.x2),
                        style = Fm.text.bodySmall.copy(color = Fm.colors.textPrimary, fontFamily = FmType.mono),
                    )
                }
            }
        }
    }
