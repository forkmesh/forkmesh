package com.forkmesh.nativeapp.feature.agents

import com.forkmesh.nativeapp.agents.PromptAttachment
import com.forkmesh.nativeapp.composer.PromptDraftStore
import android.net.Uri
import android.provider.OpenableColumns
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Arrangement
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
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.material3.ExperimentalMaterial3Api
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
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.agents.AgentAttachmentMedia
import com.forkmesh.nativeapp.agents.AgentAttachmentRef
import com.forkmesh.nativeapp.control.AgentTranscriptDelta
import com.forkmesh.nativeapp.control.ControlErrorText
import com.forkmesh.nativeapp.control.DesktopTranscriptStream
import com.forkmesh.nativeapp.control.LocalAgentRow
import com.forkmesh.nativeapp.control.LocalControlService
import com.forkmesh.nativeapp.control.LocalControlStatus
import com.forkmesh.nativeapp.control.MeshControlHost
import com.forkmesh.nativeapp.control.TranscriptFollower
import com.forkmesh.nativeapp.control.CONTROL_DEFAULT_PORT
import com.forkmesh.nativeapp.feature.code.DetailScaffold
import com.forkmesh.nativeapp.feature.code.FmFilledButton
import com.forkmesh.nativeapp.feature.code.FmOutlinedButton
import com.forkmesh.nativeapp.ui.kit.FmCard
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenu
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenuItem
import com.forkmesh.nativeapp.ui.kit.FmEmptyState
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmInlineStatus
import com.forkmesh.nativeapp.ui.kit.FmStatus
import com.forkmesh.nativeapp.ui.kit.FmPullToRefreshBox
import com.forkmesh.nativeapp.ui.kit.FmTextField
import com.forkmesh.nativeapp.ui.kit.FmTone
import com.forkmesh.nativeapp.ui.kit.FmToneButton
import com.forkmesh.nativeapp.ui.kit.rememberFmStatus
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmRadius
import com.forkmesh.nativeapp.ui.theme.FmSpace
import com.forkmesh.nativeapp.ui.theme.FmType
import com.forkmesh.nativeapp.feature.surfaces.SurfacesFeature
import com.forkmesh.nativeapp.shell.LaunchFlight
import java.io.File
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

@Composable
internal fun rememberControlTick(service: LocalControlService?): Int {
    var tick by remember { mutableIntStateOf(0) }
    DisposableEffect(service) {
        val listener: () -> Unit = { tick++ }
        service?.addListener(listener)
        onDispose { service?.removeListener(listener) }
    }
    return tick
}

@Composable
internal fun LocalDesktopChip(navigator: Navigator) {
    val service = AgentsFeature.localControl ?: return
    rememberControlTick(service)
    val label = when (service.status) {
        LocalControlStatus.Connected -> service.desktopName.ifEmpty { "Desktop" } + " · on this network"
        LocalControlStatus.AwaitingApproval -> "Approve on ${service.desktopName}"
        LocalControlStatus.Connecting -> "Connecting to ${service.desktopName}…"
        else -> "Desktop on this network"
    }
    FmToneButton(
        label,
        onPressed = { navigator.push(LocalDesktopScreen()) },
        icon = "lan_outlined",
        tone = if (service.connected) FmTone.Success else FmTone.Neutral,
    )
}

internal fun controlError(error: Throwable): String = ControlErrorText.describe(error)

internal class LocalDesktopScreen : Screen() {
    override val title: String get() = "Desktop on this network"

    @Composable
    override fun Content(navigator: Navigator) {
        val c = Fm.colors
        val service = AgentsFeature.localControl
        val tick = rememberControlTick(service)
        DetailScaffold(
            service?.desktopName?.takeIf { service.connected } ?: title,
            navigator,
            actions = {
                if (service?.connected == true) {
                    FmIconButton("refresh", "Refresh", onPressed = { service.refreshAgents() }, color = c.textPrimary)
                }
            },
        ) {
            if (service == null) {
                FmEmptyState(
                    icon = "lan_outlined",
                    title = "Local control is not available",
                    message = "This device has no identity key yet. Sign in first.",
                )
                return@DetailScaffold
            }
            tick.let { }
            LazyColumn(
                Modifier.fillMaxSize(),
                contentPadding = PaddingValues(FmSpace.x4),
                verticalArrangement = Arrangement.spacedBy(FmSpace.x3),
            ) {
                item { ConnectionCard(service) }
                if (!service.connected) item { HostsCard(service) }
                if (service.connected) {
                    item { NewAgentCard(service) }
                    item { AgentsCard(navigator, service) }
                    item { FilesCard(service) }
                }
            }
        }
    }

    @Composable
    private fun ConnectionCard(service: LocalControlService) {
        val c = Fm.colors
        val (heading, body) = when (service.status) {
            LocalControlStatus.Connected -> "Connected to ${service.desktopName}" to
                "Directly over this network, encrypted end to end. Agents, files and transcripts move without the relay."
            LocalControlStatus.AwaitingApproval -> "Approve this phone on ${service.desktopName}" to
                "The desktop is asking whether to let this phone control it. Allow it only if it shows the same code."
            LocalControlStatus.Connecting -> "Connecting to ${service.desktopName}…" to ""
            LocalControlStatus.Failed -> "Not connected" to service.error
            LocalControlStatus.Idle -> "Not connected" to
                "Pick a desktop below. Once it approves this phone, the two reconnect by themselves whenever they share a network."
        }
        FmCard(radius = FmRadius.lg) {
            Column(Modifier.fillMaxWidth()) {
                Text(heading, style = Fm.text.titleSmall.copy(color = c.textPrimary))
                if (body.isNotEmpty()) {
                    Spacer(Modifier.height(FmSpace.x2))
                    Text(body, style = Fm.text.bodySmall.copy(color = if (service.status == LocalControlStatus.Failed) c.danger else c.textSecondary))
                }
                if (service.pairingCode.isNotEmpty()) {
                    Spacer(Modifier.height(FmSpace.x3))
                    Text(
                        service.pairingCode.chunked(3).joinToString(" "),
                        style = Fm.text.displaySmall.copy(color = c.textPrimary, fontFamily = FmType.mono),
                    )
                }
                Spacer(Modifier.height(FmSpace.x3))
                Row(horizontalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                    val host = service.host
                    if (service.status == LocalControlStatus.Failed && host != null) {
                        FmOutlinedButton("Try again", onClick = { service.connect(host) })
                    }
                    if (service.status != LocalControlStatus.Idle && service.status != LocalControlStatus.Failed) {
                        FmOutlinedButton(if (service.connected) "Disconnect" else "Cancel", onClick = { service.disconnect() })
                    }
                    if (service.connected && service.isPaired(service.desktopKey)) {
                        FmOutlinedButton("Forget this desktop", onClick = { service.forget(service.desktopKey) })
                    }
                }
            }
        }
    }

    @Composable
    private fun HostsCard(service: LocalControlService) {
        val c = Fm.colors
        var address by remember { mutableStateOf("") }
        val hosts = service.hosts
        FmCard(radius = FmRadius.lg) {
            Column(Modifier.fillMaxWidth()) {
                Text("Desktops on this network", style = Fm.text.titleSmall.copy(color = c.textPrimary))
                Spacer(Modifier.height(FmSpace.x2))
                if (hosts.isEmpty()) {
                    Text(
                        "Looking for ForkMesh on this Wi-Fi… Open ForkMesh on the desktop, or enter its address below.",
                        style = Fm.text.bodySmall.copy(color = c.textSecondary),
                    )
                }
                for (host in hosts) {
                    Row(Modifier.fillMaxWidth().padding(vertical = FmSpace.x2), verticalAlignment = Alignment.CenterVertically) {
                        FmIcon("desktop_windows_outlined", size = 20.dp, tint = c.textSecondary)
                        Spacer(Modifier.width(FmSpace.x3))
                        Column(Modifier.weight(1f)) {
                            Text(host.label, style = Fm.text.bodyMedium.copy(color = c.textPrimary))
                            Text(
                                listOf(host.ownerUser, host.platform, host.address).filter { it.isNotEmpty() }.joinToString(" · "),
                                style = Fm.text.bodySmall.copy(color = c.textTertiary),
                            )
                        }
                        FmToneButton(if (service.isPaired(host.nodeId)) "Connect" else "Pair", onPressed = { service.connect(host) }, tone = FmTone.Accent)
                    }
                }
                Spacer(Modifier.height(FmSpace.x3))
                Row(verticalAlignment = Alignment.CenterVertically) {
                    FmTextField(address, { address = it.trim() }, Modifier.weight(1f), hint = "Desktop address (192.168.1.20)")
                    Spacer(Modifier.width(FmSpace.x2))
                    FmToneButton("Connect", onPressed = if (address.isEmpty()) null else ({
                        val host = address.substringBeforeLast(':').takeIf { address.count { ch -> ch == ':' } == 1 } ?: address
                        val port = address.substringAfterLast(':', "").toIntOrNull()?.takeIf { address.count { ch -> ch == ':' } == 1 } ?: CONTROL_DEFAULT_PORT
                        service.connect(MeshControlHost("", host, port, name = host))
                    }), tone = FmTone.Accent)
                }
            }
        }
    }

    @Composable
    private fun NewAgentCard(service: LocalControlService) {
        val c = Fm.colors
        val scope = rememberCoroutineScope()
        val status = rememberFmStatus()
        val snapshot = service.agents
        val repos = snapshot?.repositories ?: emptyList()
        var repo by remember { mutableStateOf<String?>(null) }
        var provider by remember { mutableStateOf<String?>(null) }
        var task by remember { mutableStateOf("") }
        var busy by remember { mutableStateOf(false) }
        FmCard(radius = FmRadius.lg) {
            Column(Modifier.fillMaxWidth()) {
                Text("Start an agent on ${service.desktopName}", style = Fm.text.titleSmall.copy(color = c.textPrimary))
                Spacer(Modifier.height(FmSpace.x2))
                if (snapshot != null && repos.isEmpty()) {
                    Text(
                        "The desktop has no repository checked out, so it has nowhere to run an agent.",
                        style = Fm.text.bodySmall.copy(color = c.textSecondary),
                    )
                    return@Column
                }
                val chosenRepo = repo ?: repos.firstOrNull()?.label
                val providers = snapshot?.providers ?: emptyList()
                val chosenProvider = provider ?: snapshot?.defaultProvider?.ifEmpty { null } ?: providers.firstOrNull()?.id
                Menu("Repository", chosenRepo ?: "", repos.map { it.label to it.label }) { repo = it }
                if (providers.isNotEmpty()) {
                    Spacer(Modifier.height(FmSpace.x2))
                    Menu("Agent", providers.firstOrNull { it.id == chosenProvider }?.label ?: "", providers.map { it.id to it.label }) { provider = it }
                }
                Spacer(Modifier.height(FmSpace.x2))
                FmTextField(task, { task = it }, Modifier.fillMaxWidth(), hint = "What should the agent do?", minLines = 2, maxLines = 6)
                Spacer(Modifier.height(FmSpace.x2))
                FmFilledButton(if (busy) "Starting…" else "Start on desktop", onClick = if (busy || task.isBlank() || chosenRepo == null) null else ({
                    val parts = chosenRepo.split('/', limit = 2)
                    busy = true
                    status.clear()
                    scope.launch {
                        try {
                            val id = withContext(Dispatchers.IO) { service.startAgent(parts[0], parts.getOrElse(1) { "" }, task.trim(), chosenProvider ?: "") }
                            task = ""
                            LaunchFlight.launch(SurfacesFeature.agents.value.active.size, "Started agent #$id on ${service.desktopName}.", fromComposer = false)
                            service.refreshAgents()
                        } catch (cancelled: CancellationException) {
                            throw cancelled
                        } catch (error: Exception) {
                            status.error(controlError(error))
                        } finally {
                            busy = false
                        }
                    }
                }), icon = "play_arrow")
                FmInlineStatus(status, Modifier.padding(top = FmSpace.x2))
            }
        }
    }

    @Composable
    private fun Menu(label: String, value: String, options: List<Pair<String, String>>, onPick: (String) -> Unit) {
        var open by remember { mutableStateOf(false) }
        Column {
            PickerRow("expand_more", value.ifEmpty { label }, label, onTap = { open = true })
            FmDropdownMenu(expanded = open, onDismissRequest = { open = false }) {
                for ((id, text) in options) {
                    FmDropdownMenuItem(text = text, onClick = {
                        open = false
                        onPick(id)
                    })
                }
            }
        }
    }

    @Composable
    private fun AgentsCard(navigator: Navigator, service: LocalControlService) {
        val c = Fm.colors
        val sessions = service.agents?.sessions ?: emptyList()
        FmCard(radius = FmRadius.lg) {
            Column(Modifier.fillMaxWidth()) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text("Agents on the desktop", Modifier.weight(1f), style = Fm.text.titleSmall.copy(color = c.textPrimary))
                    val watch = AgentWatchEntry.openControl
                    if (watch != null && sessions.isNotEmpty()) {
                        val live = sessions.filter { it.session.isActive }
                        FmToneButton("Watch", onPressed = {
                            val rows = (live.ifEmpty { sessions.take(3) }).map {
                                com.forkmesh.nativeapp.control.UnifiedAgentRow.fromDesktop(it, com.forkmesh.nativeapp.control.ControlTransport.Lan, service.desktopName)
                            }
                            watch(navigator, if (live.isNotEmpty()) "Watch agents" else "Replay agents", rows)
                        }, icon = "graphic_eq")
                    }
                }
                Spacer(Modifier.height(FmSpace.x2))
                if (sessions.isEmpty()) {
                    Text(
                        if (service.agentsLoading) "Loading…" else "No agent sessions on the desktop yet.",
                        style = Fm.text.bodySmall.copy(color = c.textSecondary),
                    )
                }
                for (row in sessions) {
                    Row(
                        Modifier.fillMaxWidth().padding(vertical = FmSpace.x1),
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        Column(Modifier.weight(1f)) {
                            Text(row.title, maxLines = 1, overflow = TextOverflow.Ellipsis, style = Fm.text.bodyMedium.copy(color = c.textPrimary))
                            Text(row.repoLabel, style = Fm.text.bodySmall.copy(color = c.textTertiary))
                        }
                        AgentStatusBadge(row.session)
                        FmIconButton("chevron_right", "Open", onPressed = { navigator.push(LocalAgentScreen(row.session.id)) }, color = c.textTertiary)
                    }
                }
            }
        }
    }

    @Composable
    private fun FilesCard(service: LocalControlService) {
        val c = Fm.colors
        val context = LocalContext.current
        val scope = rememberCoroutineScope()
        val status = rememberFmStatus()
        val picker = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri: Uri? ->
            if (uri == null) return@rememberLauncherForActivityResult
            status.clear()
            scope.launch {
                try {
                    val saved = withContext(Dispatchers.IO) {
                        var name = "file"
                        context.contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use { cursor ->
                            if (cursor.moveToFirst()) name = cursor.getString(0) ?: name
                        }
                        val temp = File(context.cacheDir, "outgoing-${System.nanoTime()}")
                        try {
                            context.contentResolver.openInputStream(uri)?.use { input -> temp.outputStream().use { input.copyTo(it) } }
                            service.sendFile(temp, name)
                        } finally {
                            temp.delete()
                        }
                    }
                    status.show("Sent $saved to ${service.desktopName}.")
                } catch (cancelled: CancellationException) {
                    throw cancelled
                } catch (error: Exception) {
                    status.error(controlError(error))
                }
            }
        }
        FmCard(radius = FmRadius.lg) {
            Column(Modifier.fillMaxWidth()) {
                Text("Files", style = Fm.text.titleSmall.copy(color = c.textPrimary))
                Spacer(Modifier.height(FmSpace.x1))
                Text(
                    "Files you send land in the desktop's Downloads, under \"ForkMesh from phone\". Files the desktop sends land here.",
                    style = Fm.text.bodySmall.copy(color = c.textSecondary),
                )
                Spacer(Modifier.height(FmSpace.x2))
                val progress = service.uploadProgress
                if (progress != null) {
                    Text("Sending ${service.uploadName}… ${(progress * 100).toInt()}%", style = Fm.text.bodySmall.copy(color = c.textSecondary))
                } else {
                    FmOutlinedButton("Send a file to the desktop", onClick = { picker.launch(arrayOf("*/*")) }, icon = "upload_file_outlined")
                }
                FmInlineStatus(status, Modifier.padding(top = FmSpace.x2))
                for (file in service.receivedFiles) {
                    Row(Modifier.fillMaxWidth().padding(vertical = FmSpace.x1), verticalAlignment = Alignment.CenterVertically) {
                        FmIcon("insert_drive_file_outlined", size = 18.dp, tint = c.textTertiary)
                        Spacer(Modifier.width(FmSpace.x2))
                        Text(file.name, Modifier.weight(1f), maxLines = 1, overflow = TextOverflow.Ellipsis, style = Fm.text.bodySmall.copy(color = c.textPrimary))
                        Text(AgentAttachmentMedia.sizeLabel(file.size), style = Fm.text.labelSmall.copy(color = c.textTertiary))
                    }
                }
            }
        }
    }
}

internal class LocalAgentScreen(private val sessionId: Int) : Screen() {
    private var transcript by mutableStateOf("")
    private var attachments by mutableStateOf<List<AgentAttachmentRef>>(emptyList())
    private var message by mutableStateOf("")
    private var photos by mutableStateOf<List<PromptAttachment>>(emptyList())
    private var busy by mutableStateOf(false)
    private var refreshing by mutableStateOf(false)
    private var streaming by mutableStateOf(false)
    private val status = FmStatus()
    private val sendStatus = FmStatus()

    override val title: String get() = "Agent #$sessionId"

    @OptIn(ExperimentalMaterial3Api::class)
    @Composable
    override fun Content(navigator: Navigator) {
        val c = Fm.colors
        val scope = rememberCoroutineScope()
        val service = AgentsFeature.localControl ?: return
        val tick = rememberControlTick(service)
        val row: LocalAgentRow? = service.agents?.sessions?.firstOrNull { it.session.id == sessionId }
        LaunchedEffect(sessionId) {
            val follower = TranscriptFollower()
            val deltas = Channel<AgentTranscriptDelta>(Channel.UNLIMITED)
            val stream = DesktopTranscriptStream(AgentsFeature.control.hub, sessionId, onEnded = { deltas.close() }) {
                deltas.trySend(it)
            }
            try {
                streaming = withContext(Dispatchers.IO) { stream.start() }
                if (streaming) {
                    for (delta in deltas) {
                        follower.apply(delta)
                        transcript = follower.text
                        attachments = follower.attachments
                    }
                }
            } finally {
                streaming = false
                withContext(NonCancellable + Dispatchers.IO) { stream.stop() }
            }
        }
        LaunchedEffect(tick, streaming) {
            if (streaming) return@LaunchedEffect
            try {
                val detail = withContext(Dispatchers.IO) { service.transcriptDetail(sessionId) }
                transcript = detail.text
                attachments = detail.attachments
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (_: Exception) {
            }
        }
        LaunchedEffect(row?.session?.isActive, streaming) {
            while (!streaming && row?.session?.isActive == true) {
                delay(4000)
                runCatching { withContext(Dispatchers.IO) { service.transcriptDetail(sessionId) } }.getOrNull()?.let {
                    transcript = it.text
                    attachments = it.attachments
                }
            }
        }
        fun act(done: String, target: FmStatus = status, onFailure: () -> Unit = {}, block: () -> Unit) {
            busy = true
            status.clear()
            sendStatus.clear()
            scope.launch {
                try {
                    withContext(Dispatchers.IO) { block() }
                    target.show(done)
                    service.refreshAgents()
                } catch (cancelled: CancellationException) {
                    throw cancelled
                } catch (error: Exception) {
                    onFailure()
                    target.error(controlError(error))
                } finally {
                    busy = false
                }
            }
        }
        DetailScaffold(row?.title ?: title, navigator) {
            Column(Modifier.fillMaxSize()) {
                Row(Modifier.fillMaxWidth().padding(FmSpace.x3), horizontalArrangement = Arrangement.spacedBy(FmSpace.x2), verticalAlignment = Alignment.CenterVertically) {
                    if (row != null) AgentStatusBadge(row.session)
                    Spacer(Modifier.weight(1f))
                    if (row?.stoppable == true) FmOutlinedButton("Stop", onClick = if (busy) null else ({ act("Stopped.") { service.stopAgent(sessionId) } }))
                    if (row?.canContinue == true) FmOutlinedButton("Continue", onClick = if (busy) null else ({ act("Continuing on the desktop.") { service.continueAgent(sessionId) } }))
                }
                FmInlineStatus(status, Modifier.padding(horizontal = FmSpace.x3).padding(bottom = FmSpace.x2))
                val list = rememberLazyListState()
                FmPullToRefreshBox(
                    isRefreshing = refreshing,
                    onRefresh = {
                        refreshing = true
                        service.refreshAgents()
                        scope.launch {
                            try {
                                runCatching { withContext(Dispatchers.IO) { service.transcriptDetail(sessionId) } }.getOrNull()?.let {
                                    transcript = it.text
                                    attachments = it.attachments
                                }
                            } finally {
                                refreshing = false
                            }
                        }
                    },
                    modifier = Modifier.weight(1f).fillMaxWidth(),
                ) {
                    LazyColumn(Modifier.fillMaxSize(), state = list, contentPadding = PaddingValues(FmSpace.x4)) {
                        item {
                            SelectionContainer {
                                Text(
                                    transcript.ifEmpty { "Nothing in the transcript yet." },
                                    style = Fm.text.bodySmall.copy(fontFamily = FmType.mono, color = c.textPrimary),
                                )
                            }
                        }
                        if (attachments.isNotEmpty()) item {
                            Spacer(Modifier.height(FmSpace.x3))
                            AgentAttachmentStrip(attachments)
                        }
                    }
                }
                FmInlineStatus(sendStatus, Modifier.padding(horizontal = FmSpace.x3, vertical = FmSpace.x1))
                AgentPromptComposer(
                    text = message,
                    onTextChange = { message = it },
                    draftSurface = PromptDraftStore.agentSurface("lan/$sessionId"),
                    hint = "Message this agent",
                    attachments = photos,
                    onAttachmentsChanged = { photos = it },
                    onSend = {
                        val text = message.trim()
                        val sending = photos
                        if (text.isNotEmpty() || sending.isNotEmpty()) {
                            message = ""
                            photos = emptyList()
                            act("Sent to the agent.", sendStatus, onFailure = {
                                if (message.isEmpty()) message = text
                                if (photos.isEmpty()) photos = sending
                            }) { service.sendPrompt(sessionId, text, sending) }
                        }
                    },
                    busy = busy,
                )
            }
        }
    }
}
