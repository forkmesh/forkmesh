package com.forkmesh.nativeapp.feature.agents

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
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
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.agents.AGENT_LOGIN_PROVIDERS
import com.forkmesh.nativeapp.agents.AgentCredentials
import com.forkmesh.nativeapp.agents.AgentLaunchException
import com.forkmesh.nativeapp.agents.AgentLaunchOptions
import com.forkmesh.nativeapp.agents.AgentLaunchTarget
import com.forkmesh.nativeapp.agents.AgentModelCatalog
import com.forkmesh.nativeapp.agents.AgentModelChoice
import com.forkmesh.nativeapp.agents.AgentRunMode
import com.forkmesh.nativeapp.agents.AgentStartPlan
import com.forkmesh.nativeapp.agents.AgentStartTarget
import com.forkmesh.nativeapp.agents.PromptAttachment
import com.forkmesh.nativeapp.agents.RunnerBranchName
import com.forkmesh.nativeapp.model.RepoBranch
import com.forkmesh.nativeapp.agents.agentProviderLabel
import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.account.AccountFeature
import com.forkmesh.nativeapp.feature.code.DetailScaffold
import com.forkmesh.nativeapp.localrepos.LocalRepoStore
import com.forkmesh.nativeapp.ui.kit.FmCard
import com.forkmesh.nativeapp.ui.kit.FmField
import com.forkmesh.nativeapp.ui.kit.FmMessenger
import com.forkmesh.nativeapp.ui.kit.FmStatus
import com.forkmesh.nativeapp.ui.kit.FmSegmentTab
import com.forkmesh.nativeapp.ui.kit.FmSegmentTabBar
import com.forkmesh.nativeapp.ui.kit.FmTone
import com.forkmesh.nativeapp.ui.kit.FmToneButton
import com.forkmesh.nativeapp.ui.kit.LocalFmMessenger
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmRadius
import com.forkmesh.nativeapp.ui.theme.FmSpace
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

internal class AgentStartScreen(
    private val initialPrompt: String = "",
    private val initialAttachments: List<PromptAttachment> = emptyList(),
) : Screen() {
    override val title: String get() = "Start an agent"

    @Composable
    override fun Content(navigator: Navigator) {
        DetailScaffold(title, navigator) {
            Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(FmSpace.x4)) {
                AgentStartCard(
                    navigator,
                    initialPrompt = initialPrompt,
                    initialAttachments = initialAttachments,
                    autofocus = true,
                    onStarted = { AgentsFeature.ring() },
                )
            }
        }
    }
}

private class StartCardState(initialPrompt: String, initialAttachments: List<PromptAttachment>) {
    var prompt by mutableStateOf(initialPrompt)
    var attachments by mutableStateOf(initialAttachments)
    var targets by mutableStateOf<List<AgentStartTarget>?>(null)
    var plan by mutableStateOf<AgentStartPlan?>(null)
    var mode by mutableStateOf<AgentRunMode?>(null)
    var nodeChoice by mutableStateOf("")
    var branches by mutableStateOf<List<RepoBranch>>(emptyList())
    var baseBranch by mutableStateOf("")
    var busy by mutableStateOf(false)
    var nodeModel by mutableStateOf(AgentModelCatalog.DEFAULT_NODE_MODEL)
    var deviceCredentials by mutableStateOf<AgentCredentials?>(null)
    var error by mutableStateOf<String?>(null)
    var notice by mutableStateOf<String?>(null)
    var probe by mutableIntStateOf(0)

    val effectiveMode: AgentRunMode get() = mode ?: plan?.defaultMode ?: AgentRunMode.Device
    val node: AgentLaunchTarget? get() = plan?.options?.targetNamed(nodeChoice) ?: plan?.node

    fun canRunOnDevice(): Boolean {
        val target = plan?.target ?: return false
        return target.isMirrored || (target.isOnMesh && AgentsFeature.cloneToDevice != null)
    }

    val modelLabel: String
        get() = if (effectiveMode == AgentRunMode.Node) nodeModel.label
        else deviceCredentials?.let { AgentModelCatalog.shortLabel(it.model) } ?: "Choose a model"
}

@Composable
internal fun AgentStartCard(
    navigator: Navigator,
    onStarted: () -> Unit,
    initialPrompt: String = "",
    initialAttachments: List<PromptAttachment> = emptyList(),
    autofocus: Boolean = false,
) {
    val c = Fm.colors
    val scope = rememberCoroutineScope()
    val messenger = LocalFmMessenger.current
    val requireAccount = AccountFeature.rememberRequireAccount()
    val state = remember { StartCardState(initialPrompt, initialAttachments) }

    suspend fun selectTarget(target: AgentStartTarget) {
        val generation = ++state.probe
        state.plan = AgentStartPlan(target, loading = target.isOnMesh)
        state.mode = null
        state.nodeChoice = ""
        state.branches = emptyList()
        state.baseBranch = ""
        state.error = null
        state.notice = null
        if (!target.isOnMesh) return
        var options: AgentLaunchOptions? = null
        var failure = ""
        try {
            options = withContext(Dispatchers.IO) { AgentsFeature.queue.launchOptions(target.owner, target.name) }
        } catch (cancelled: CancellationException) {
            throw cancelled
        } catch (error: AgentLaunchException) {
            failure = if (error.status == 403 || error.status == 401) "" else error.message
        } catch (error: Exception) {
            failure = error.message ?: error.toString()
        }
        if (generation != state.probe) return
        state.plan = AgentStartPlan(target, options, failure)
        val branches = withContext(Dispatchers.IO) {
            runCatching { Services.require<ApiService>().branches(target.owner, target.name) }.getOrDefault(emptyList())
        }
        if (generation != state.probe) return
        state.branches = branches
        state.baseBranch = (branches.firstOrNull { it.isDefault } ?: branches.firstOrNull())?.name ?: ""
    }

    LaunchedEffect(Unit) {
        state.nodeModel = withContext(Dispatchers.IO) { AgentsFeature.nodeModel.read() }
        state.deviceCredentials = withContext(Dispatchers.IO) { runCatching { AgentsFeature.credentials.read() }.getOrNull() }
        val repos = withContext(Dispatchers.IO) { runCatching { Services.require<ApiService>().repositories() }.getOrDefault(emptyList()) }
        val records = withContext(Dispatchers.IO) {
            runCatching { Services.get<LocalRepoStore>()?.let { it.ensureLoaded(); it.list() } ?: emptyList() }.getOrDefault(emptyList())
        }
        val targets = AgentStartTarget.fold(repos, records)
        state.targets = targets
        if (targets.isNotEmpty()) selectTarget(targets.first())
    }

    suspend fun startNodeRun(plan: AgentStartPlan) {
        if (!requireAccount("start an agent")) return
        val target = plan.target
        val text = state.prompt
        val photos = state.attachments
        val result = withContext(Dispatchers.IO) {
            AgentsFeature.queue.launch(
                target.owner,
                target.name,
                prompt = text,
                provider = AgentStartPlan.providerFor(state.nodeModel),
                model = state.nodeModel.alias,
                targetNode = state.node?.node ?: "",
                title = AgentStartPlan.titleFor(text),
                orgScope = plan.options?.orgScope ?: false,
                attachments = photos,
                baseBranch = state.baseBranch,
                branchName = RunnerBranchName.slug(AgentStartPlan.titleFor(text)),
            )
        }
        result.session?.let { AgentsFeature.sentPhotos.record(it.sessionId, text.trim().ifEmpty { "See the attached photos." }, photos) }
        state.prompt = ""
        state.attachments = emptyList()
        state.notice = result.label
        AgentsFeature.ring()
        onStarted()
    }

    fun start() {
        var plan = state.plan ?: return
        if (state.busy || (state.prompt.isBlank() && state.attachments.isEmpty())) return
        scope.launch {
            if (!AgentsFeature.signedIn()) {
                if (!requireAccount("start an agent")) return@launch
                selectTarget(plan.target)
                plan = state.plan ?: return@launch
                if (!plan.canRunOnNode && !state.canRunOnDevice()) return@launch
            }
            state.busy = true
            state.error = null
            state.notice = null
            try {
                if (state.effectiveMode == AgentRunMode.Node) {
                    startNodeRun(plan)
                } else {
                    val target = plan.target
                    val clone = AgentsFeature.cloneToDevice
                    val cloneStatus = FmStatus()
                    val record = if (!requireAccount("run a coding agent")) {
                        null
                    } else {
                        target.record ?: if (target.isOnMesh && clone != null) clone(messenger, cloneStatus, target.publicOwner, target.name, target.repo?.description ?: "") else null
                    }
                    if (record == null) {
                        val message = cloneStatus.message
                        if (message != null && cloneStatus.isError) state.error = message else if (message != null) state.notice = message
                    }
                    if (record != null) {
                        navigator.push(AgentScreen(record, state.prompt, state.attachments, autoStart = true))
                        state.prompt = ""
                        state.attachments = emptyList()
                        onStarted()
                    }
                }
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (error: Exception) {
                state.error = error.message ?: error.toString()
            } finally {
                state.busy = false
            }
        }
    }

    val plan = state.plan
    val signedIn = AgentsFeature.signedIn()
    val mode = state.effectiveMode
    val canRun = !state.busy && plan != null && !plan.loading &&
        (!signedIn || (if (mode == AgentRunMode.Node) plan.canRunOnNode else state.canRunOnDevice()))

    FmCard(radius = FmRadius.lg) {
        Column(Modifier.fillMaxWidth()) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                FmIcon("auto_awesome_outlined", size = 20.dp, tint = c.accent)
                Spacer(Modifier.width(FmSpace.x2))
                Text("Start an agent", style = Fm.text.titleSmall.copy(color = c.textPrimary))
            }
            Spacer(Modifier.height(FmSpace.x2))
            Text(
                "Describe the change. It is sent to a mirror, which works on its own branch and opens a pull request when it is done.",
                style = Fm.text.bodySmall.copy(color = c.textSecondary),
            )
            Spacer(Modifier.height(FmSpace.x4))
            FmField("Repository") {
                val targets = state.targets
                PickerRow(
                    icon = "folder_outlined",
                    label = when {
                        targets == null -> "Loading repositories…"
                        else -> plan?.target?.label ?: "No repository available"
                    },
                    detail = plan?.target?.detail ?: "",
                    onTap = if (targets.isNullOrEmpty()) null else ({
                        scope.launch {
                            val picked = pickTarget(messenger, targets, plan?.target)
                            if (picked != null) selectTarget(picked)
                        }
                    }),
                )
            }
            Spacer(Modifier.height(FmSpace.x3))
            if (state.effectiveMode == AgentRunMode.Node && state.branches.size > 1) {
                FmField("Base branch") {
                    PickerRow(
                        icon = "account_tree_outlined",
                        label = state.baseBranch.ifEmpty { "Default branch" },
                        detail = "The agent branches from here, and Merge & clean up merges back into it.",
                        onTap = {
                            scope.launch {
                                val picked = pickBranch(messenger, state.branches, state.baseBranch)
                                if (picked != null) state.baseBranch = picked
                            }
                        },
                    )
                }
                Spacer(Modifier.height(FmSpace.x3))
            }
            RunsOn(navigator, state, signedIn) {
                scope.launch {
                    val targets = state.plan?.options?.targets ?: return@launch
                    val picked = pickNode(messenger, targets, state.node)
                    if (picked != null) state.nodeChoice = picked.node
                }
            }
            state.notice?.let {
                Spacer(Modifier.height(FmSpace.x3))
                NoteLine(it, c.success, "check_circle_outline")
            }
            state.error?.let {
                Spacer(Modifier.height(FmSpace.x3))
                NoteLine(it, c.danger, "error_outline")
            }
            Spacer(Modifier.height(FmSpace.x4))
            AgentPromptComposer(
                text = state.prompt,
                onTextChange = { state.prompt = it },
                hint = "Describe the change",
                onSend = if (canRun) ({ start() }) else null,
                busy = state.busy,
                attachments = state.attachments,
                onAttachmentsChanged = { state.attachments = it },
                modelLabel = state.modelLabel,
                onPickModel = {
                    scope.launch {
                        if (state.effectiveMode == AgentRunMode.Device) {
                            val saved = showAgentSetupSheet(messenger, state.deviceCredentials)
                            if (saved != null) state.deviceCredentials = saved
                        } else {
                            val choices = AgentModelCatalog.nodeChoices(state.plan?.options?.models ?: emptyMap())
                            val picked = pickModel(messenger, choices, state.nodeModel)
                            if (picked != null) {
                                state.nodeModel = picked
                                withContext(Dispatchers.IO) { AgentsFeature.nodeModel.write(picked) }
                            }
                        }
                    }
                },
                autofocus = autofocus,
                padForKeyboard = false,
                modifier = Modifier.clip(RoundedCornerShape(FmRadius.md)),
            )
        }
    }
}

@Composable
private fun RunsOn(navigator: Navigator, state: StartCardState, signedIn: Boolean, onPickNode: () -> Unit) {
    val c = Fm.colors
    val plan = state.plan ?: return
    val small = Fm.text.bodySmall.copy(color = c.textTertiary)
    if (plan.loading) {
        Text("Checking where this can run…", style = small)
        return
    }
    if (!signedIn) {
        Text("Sign in and the prompt goes to a mirror of this repository, which runs the agent and opens the pull request.", style = small)
        return
    }
    val deviceAvailable = state.canRunOnDevice()
    val node = state.node
    val mode = state.effectiveMode
    Column {
        if (plan.canRunOnNode && deviceAvailable) {
            FmSegmentTabBar(
                listOf(FmSegmentTab("On a node"), FmSegmentTab("On this device")),
                if (mode == AgentRunMode.Node) 0 else 1,
                onChanged = { state.mode = if (it == 0) AgentRunMode.Node else AgentRunMode.Device },
            )
            Spacer(Modifier.height(FmSpace.x2))
        }
        val targets = plan.options?.targets ?: emptyList()
        if (mode == AgentRunMode.Node && node != null && targets.size > 1) {
            PickerRow(
                icon = "dns_outlined",
                label = node.node,
                detail = "${if (node.isMirror) "Mirror" else "Source node"} · ${if (node.online) "online" else "offline — queue for later"}",
                onTap = onPickNode,
            )
            Spacer(Modifier.height(FmSpace.x2))
        }
        Text(plan.runsOnText(mode, node, deviceAvailable), style = small)
        val signIns = if (mode == AgentRunMode.Node && plan.canRunOnNode && node != null) {
            (if (node.isRunner) node.needsLogin else node.providers).filter { it in AGENT_LOGIN_PROVIDERS }
        } else {
            emptyList()
        }
        if (signIns.isNotEmpty() && node != null) {
            Spacer(Modifier.height(FmSpace.x2))
            for (provider in signIns) {
                FmToneButton(
                    "Sign in to ${agentProviderLabel(provider)} on ${node.signInNode}",
                    onPressed = { AgentsFeature.openSignIn(navigator, node, provider) },
                    icon = "key_outlined",
                    tone = FmTone.Accent,
                )
                Spacer(Modifier.height(FmSpace.x1))
            }
            Text("Needed once per node, when its agent is not signed in yet.", style = small)
        }
        if (plan.error.isNotEmpty()) {
            Spacer(Modifier.height(FmSpace.x2))
            Text(plan.error, style = Fm.text.bodySmall.copy(color = c.warning))
        }
    }
}

@Composable
internal fun PickerRow(icon: String, label: String, detail: String, onTap: (() -> Unit)?) {
    val c = Fm.colors
    Row(
        Modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(FmRadius.md))
            .background(c.bgOverlay)
            .then(if (onTap != null) Modifier.clickable(onClick = onTap) else Modifier)
            .padding(FmSpace.x3),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        FmIcon(icon, size = 18.dp, tint = c.textTertiary)
        Spacer(Modifier.width(FmSpace.x2))
        Column(Modifier.weight(1f)) {
            Text(label, style = Fm.text.bodyMedium.copy(color = c.textPrimary))
            if (detail.isNotEmpty()) {
                Spacer(Modifier.height(FmSpace.x1))
                Text(detail, style = Fm.text.bodySmall.copy(color = c.textTertiary))
            }
        }
        if (onTap != null) FmIcon("expand_more", size = 18.dp, tint = c.textTertiary)
    }
}

@Composable
private fun SheetOption(title: String, detail: String, selected: Boolean, onTap: () -> Unit) {
    val c = Fm.colors
    Row(
        Modifier
            .fillMaxWidth()
            .heightIn(min = 56.dp)
            .clickable(onClick = onTap)
            .padding(horizontal = FmSpace.x4, vertical = FmSpace.x2),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Column(Modifier.weight(1f)) {
            Text(title, style = Fm.text.bodyLarge.copy(color = c.textPrimary))
            if (detail.isNotEmpty()) Text(detail, style = Fm.text.bodySmall.copy(color = c.textSecondary))
        }
        if (selected) FmIcon("check", size = 18.dp, tint = c.accent)
    }
}

@Composable
private fun SheetTitle(text: String) {
    Text(text, Modifier.padding(FmSpace.x4), style = Fm.text.titleMedium.copy(color = Fm.colors.textPrimary))
}

private suspend fun pickTarget(messenger: FmMessenger, targets: List<AgentStartTarget>, selected: AgentStartTarget?): AgentStartTarget? =
    messenger.showModalBottomSheet(isScrollControlled = true) { close ->
        Column(Modifier.verticalScroll(rememberScrollState()).padding(bottom = FmSpace.x5)) {
            SheetTitle("Repository")
            val mirrored = targets.filter { it.isMirrored }
            val remote = targets.filter { !it.isMirrored }
            if (mirrored.isNotEmpty()) {
                Text("On this device", Modifier.padding(horizontal = FmSpace.x4), style = Fm.text.labelMedium.copy(color = Fm.colors.textTertiary))
                for (target in mirrored) SheetOption(target.label, target.detail, target.key == selected?.key) { close(target) }
            }
            if (remote.isNotEmpty()) {
                Text("On the mesh", Modifier.padding(horizontal = FmSpace.x4), style = Fm.text.labelMedium.copy(color = Fm.colors.textTertiary))
                for (target in remote) SheetOption(target.label, target.detail, target.key == selected?.key) { close(target) }
            }
        }
    }

private suspend fun pickBranch(messenger: FmMessenger, branches: List<RepoBranch>, selected: String): String? =
    messenger.showModalBottomSheet(isScrollControlled = true) { close ->
        Column(Modifier.verticalScroll(rememberScrollState()).padding(bottom = FmSpace.x5)) {
            SheetTitle("Base branch")
            for (branch in branches) {
                SheetOption(branch.name, if (branch.isDefault) "Default branch" else "", branch.name == selected) { close(branch.name) }
            }
        }
    }

private suspend fun pickNode(messenger: FmMessenger, targets: List<AgentLaunchTarget>, selected: AgentLaunchTarget?): AgentLaunchTarget? =
    messenger.showModalBottomSheet(isScrollControlled = true) { close ->
        Column(Modifier.verticalScroll(rememberScrollState()).padding(bottom = FmSpace.x5)) {
            SheetTitle("Run on")
            for (target in targets) {
                val detail = listOf(
                    if (target.isMirror) "Mirror" else "Source node",
                    target.statusLabel,
                    target.providers.joinToString(", ") { agentProviderLabel(it) },
                ).filter { it.isNotEmpty() }.joinToString(" · ")
                SheetOption(target.node, detail, target.node == selected?.node) { close(target) }
            }
        }
    }

private suspend fun pickModel(messenger: FmMessenger, choices: List<AgentModelChoice>, selected: AgentModelChoice): AgentModelChoice? =
    messenger.showModalBottomSheet(isScrollControlled = true) { close ->
        Column(Modifier.verticalScroll(rememberScrollState()).padding(bottom = FmSpace.x5)) {
            SheetTitle("Model")
            for (choice in choices) {
                val detail = listOf(if (choice.provider == "codex") "Codex" else "Claude Code", choice.note).filter { it.isNotEmpty() }.joinToString(" · ")
                SheetOption(choice.label, detail, choice.alias == selected.alias) { close(choice) }
            }
        }
    }
