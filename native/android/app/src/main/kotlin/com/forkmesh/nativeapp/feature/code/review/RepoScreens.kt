package com.forkmesh.nativeapp.feature.code.review

import android.net.Uri
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.rememberScrollState
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
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.code.BranchOpKind
import com.forkmesh.nativeapp.code.BranchOpRequest
import com.forkmesh.nativeapp.code.BranchOpResult
import com.forkmesh.nativeapp.code.BranchOpResume
import com.forkmesh.nativeapp.code.BranchOpState
import com.forkmesh.nativeapp.code.BranchOpWording
import com.forkmesh.nativeapp.code.BranchOpsClient
import com.forkmesh.nativeapp.code.CodeMapCell
import com.forkmesh.nativeapp.code.CodeMapLayout
import com.forkmesh.nativeapp.code.CodeMapNode
import com.forkmesh.nativeapp.code.CodeMapRect
import com.forkmesh.nativeapp.code.CodeOutline
import com.forkmesh.nativeapp.code.CommitGraph
import com.forkmesh.nativeapp.code.CommitGraphRow
import com.forkmesh.nativeapp.code.ConflictResolution
import com.forkmesh.nativeapp.code.FindInText
import com.forkmesh.nativeapp.code.FindOptions
import com.forkmesh.nativeapp.code.GoToLine
import com.forkmesh.nativeapp.collab.RepoAbout
import com.forkmesh.nativeapp.collab.RepoArchiveBuilder
import com.forkmesh.nativeapp.collab.RepoArchiveResult
import com.forkmesh.nativeapp.collab.RepoForkRow
import com.forkmesh.nativeapp.collab.RepoSettingsRules
import com.forkmesh.nativeapp.feature.agents.AgentsFeature
import com.forkmesh.nativeapp.feature.code.CodeFeature
import com.forkmesh.nativeapp.feature.code.DetailScaffold
import com.forkmesh.nativeapp.feature.code.EmptyCard
import com.forkmesh.nativeapp.feature.code.ErrorCard
import com.forkmesh.nativeapp.feature.code.InfoCard
import com.forkmesh.nativeapp.feature.code.Kv
import com.forkmesh.nativeapp.feature.code.LoadingCard
import com.forkmesh.nativeapp.feature.code.MobileCard
import com.forkmesh.nativeapp.json.firstStr
import com.forkmesh.nativeapp.model.RepoBranch
import com.forkmesh.nativeapp.model.Repository
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmInlineStatus
import com.forkmesh.nativeapp.ui.kit.FmLinearProgressIndicator
import com.forkmesh.nativeapp.ui.kit.FmStatusBadge
import com.forkmesh.nativeapp.ui.kit.FmSwitch
import com.forkmesh.nativeapp.ui.kit.FmTextField
import com.forkmesh.nativeapp.ui.kit.FmTone
import com.forkmesh.nativeapp.ui.kit.FmToneButton
import com.forkmesh.nativeapp.ui.kit.LocalFmMessenger
import com.forkmesh.nativeapp.ui.kit.rememberFmStatus
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmSpace
import kotlinx.coroutines.launch

internal class RepoArchiveScreen(private val repo: Repository) : Screen() {
    override val title: String get() = "Download ZIP"

    @Composable
    override fun Content(navigator: Navigator) {
        DetailScaffold("Download ZIP · ${repo.name}", navigator) {
            ScreenList {
                item { ArchiveCard(repo, repo.defaultBranch) }
            }
        }
    }
}

internal class BranchesScreen(private val repo: Repository) : Screen() {
    override val title: String get() = "Branches"

    @OptIn(ExperimentalLayoutApi::class)
    @Composable
    override fun Content(navigator: Navigator) {
        val scope = rememberCoroutineScope()
        val messenger = LocalFmMessenger.current
        var branches by remember { mutableStateOf<List<RepoBranch>?>(null) }
        var error by remember { mutableStateOf<String?>(null) }
        var graph by remember { mutableStateOf<List<Pair<CommitGraphRow, Map<String, Any?>>>>(emptyList()) }
        var base by remember { mutableStateOf("") }
        var head by remember { mutableStateOf("") }
        var running by remember { mutableStateOf<BranchOpRequest?>(null) }
        var progress by remember { mutableStateOf("") }
        var outcome by remember { mutableStateOf<BranchOpResult?>(null) }
        val client = remember { BranchOpsClient.from(CodeServices.api) }
        suspend fun load() {
            io {
                runCatching { CodeServices.api.branches(repo.owner, repo.name) }
                    .onSuccess { list ->
                        branches = list
                        error = null
                        if (base.isEmpty()) base = list.firstOrNull { it.isDefault }?.name ?: repo.defaultBranch
                        if (head.isEmpty()) head = list.firstOrNull { !it.isDefault }?.name ?: base
                    }
                    .onFailure { error = it.message ?: "$it" }
                val history = runCatching { CodeServices.settings(repo).history("") }.getOrDefault(emptyList()).take(80)
                graph = CommitGraph.layoutJson(history).zip(history)
            }
        }
        fun perform(request: BranchOpRequest, resume: BranchOpResume? = null) {
            if (running != null) return
            scope.launch {
                if (resume == null && request.op != BranchOpKind.Update) {
                    val confirmed = messenger.showConfirmDialog(
                        BranchOpWording.confirmTitle(request),
                        BranchOpWording.confirmBody(request),
                        confirmLabel = BranchOpWording.label(request.op, request.target, request.deleteBranch),
                        destructive = request.op == BranchOpKind.Delete || request.deleteBranch,
                    )
                    if (!confirmed) return@launch
                }
                running = request
                outcome = null
                progress = BranchOpWording.progress(request)
                try {
                    val result = io {
                        client.run(repo.owner, repo.name, request, resume) { answer ->
                            progress = answer.message.ifEmpty { BranchOpWording.progress(request) }
                        }
                    }
                    outcome = result
                    if (!result.stillWorking && result.answer.state != BranchOpState.Failed) load()
                } finally {
                    running = null
                }
            }
        }
        LaunchedEffect(Unit) { load() }
        DetailScaffold("Branches · ${repo.name}", navigator, actions = {
            FmIconButton("refresh", "Refresh", onPressed = { scope.launch { load() } })
        }) {
            ScreenList {
                error?.let { item { ErrorCard(it, { scope.launch { load() } }) } }
                val list = branches
                if (list == null && error == null) item { LoadingCard("Loading branches…") }
                running?.let { request ->
                    item {
                        InfoCard(BranchOpWording.label(request.op, request.target, request.deleteBranch)) {
                            Note(progress)
                            FmLinearProgressIndicator(Modifier.fillMaxWidth().padding(top = FmSpace.x2))
                        }
                    }
                }
                outcome?.let { result ->
                    item { BranchOpOutcomeCard(repo, result, navigator, onRetry = { perform(result.request, result.resume) }, onDismiss = { outcome = null }) }
                }
                if (list != null) {
                    item {
                        InfoCard("Compare") {
                            FmTextField(base, { base = it }, hint = "Base")
                            FmTextField(head, { head = it }, hint = "Compare")
                            FmToneButton("Show changes", onPressed = {
                                if (base.isNotBlank() && head.isNotBlank() && base != head) {
                                    navigator.push(CodeFeature.branchDiffScreen(repo, head.trim(), base.trim(), "$head → $base"))
                                }
                            }, tone = FmTone.Accent)
                        }
                    }
                    val default = list.firstOrNull { it.isDefault }
                    val defaultName = default?.name ?: repo.defaultBranch
                    items(list, key = { it.name }) { branch ->
                        InfoCard(branch.name) {
                            Row(horizontalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                                if (branch.isDefault) FmStatusBadge("default", Fm.colors.accent)
                                if (branch.hasWorktree) FmStatusBadge("worktree", Fm.colors.textSecondary)
                                Mono(branch.sha.take(8), Fm.colors.textTertiary)
                            }
                            if (!branch.isDefault) {
                                val idle = running == null
                                val known = branch.sha.isNotEmpty() && defaultName.isNotEmpty()
                                FlowRow(
                                    horizontalArrangement = Arrangement.spacedBy(FmSpace.x2),
                                    verticalArrangement = Arrangement.spacedBy(FmSpace.x2),
                                ) {
                                    FmToneButton("Changes", onPressed = {
                                        navigator.push(CodeFeature.branchDiffScreen(repo, branch.name, base.ifEmpty { repo.defaultBranch }, branch.name))
                                    })
                                    FmToneButton("Check conflicts", onPressed = { navigator.push(ConflictsScreen(repo, branch.name)) })
                                    if (known && default != null && default.sha.isNotEmpty()) {
                                        FmToneButton(
                                            BranchOpWording.label(BranchOpKind.Merge, defaultName),
                                            onPressed = if (idle) ({ perform(BranchOpRequest.merge(branch, default)) }) else null,
                                            icon = "call_merge",
                                            tone = FmTone.Success,
                                        )
                                        FmToneButton(
                                            BranchOpWording.label(BranchOpKind.Merge, defaultName, deleteBranch = true),
                                            onPressed = if (idle) ({ perform(BranchOpRequest.merge(branch, default, deleteBranch = true)) }) else null,
                                            icon = "call_merge",
                                            tone = FmTone.Success,
                                        )
                                    }
                                    if (known) {
                                        FmToneButton(
                                            BranchOpWording.label(BranchOpKind.Update, defaultName),
                                            onPressed = if (idle) ({ perform(BranchOpRequest.update(branch, defaultName)) }) else null,
                                            icon = "sync",
                                            tone = FmTone.Accent,
                                        )
                                        FmToneButton(
                                            BranchOpWording.label(BranchOpKind.Delete, defaultName),
                                            onPressed = if (idle) ({ perform(BranchOpRequest.delete(branch, defaultName)) }) else null,
                                            icon = "delete_outline",
                                            tone = FmTone.Danger,
                                        )
                                    }
                                }
                            }
                        }
                    }
                }
                if (graph.isNotEmpty()) {
                    item { Text("Commit graph", style = Fm.text.titleSmall, color = Fm.colors.textPrimary) }
                    val width = CommitGraph.width(graph.map { it.first })
                    items(graph, key = { it.first.hash }) { (row, commit) ->
                        Row(verticalAlignment = Alignment.CenterVertically) {
                            GraphCell(row, width)
                            Column(Modifier.padding(start = FmSpace.x2)) {
                                Text(commit.firstStr("subject", "message").lineSequence().firstOrNull().orEmpty(), color = Fm.colors.textPrimary, maxLines = 1)
                                Mono("${row.hash.take(8)} ${commit.firstStr("authorName", "author")}", Fm.colors.textTertiary)
                            }
                        }
                    }
                }
            }
        }
    }
}

@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun BranchOpOutcomeCard(repo: Repository, result: BranchOpResult, navigator: Navigator, onRetry: () -> Unit, onDismiss: () -> Unit) {
    val answer = result.answer
    val color = when {
        result.stillWorking -> Fm.colors.warning
        answer.state == BranchOpState.Failed -> Fm.colors.danger
        answer.state == BranchOpState.Queued -> Fm.colors.textSecondary
        else -> Fm.colors.success
    }
    InfoCard(BranchOpWording.label(result.request.op, result.request.target, result.request.deleteBranch)) {
        Note(result.outcome, color)
        if (answer.conflicts.isNotEmpty()) {
            Note("Conflicting files:")
            for (path in answer.conflicts.take(20)) Mono(path, Fm.colors.danger)
            if (answer.conflicts.size > 20) Note("and ${answer.conflicts.size - 20} more.")
        }
        FlowRow(
            Modifier.padding(top = FmSpace.x2),
            horizontalArrangement = Arrangement.spacedBy(FmSpace.x2),
            verticalArrangement = Arrangement.spacedBy(FmSpace.x2),
        ) {
            if (answer.isConflict) {
                FmToneButton("Check conflicts", onPressed = { navigator.push(ConflictsScreen(repo, result.request.branch)) }, tone = FmTone.Accent)
            }
            if (result.offersActions) {
                FmToneButton("Actions", onPressed = { navigator.push(ActionsScreen(repo, answer.mergeOid)) }, icon = "play_circle_outline", tone = FmTone.Accent)
            }
            if (result.canResume) FmToneButton("Check again", onPressed = onRetry, icon = "refresh")
            FmToneButton("Dismiss", onPressed = onDismiss)
        }
    }
}

@Composable
private fun GraphCell(row: CommitGraphRow, lanes: Int) {
    val laneColor = Fm.colors.textTertiary
    val dotColor = if (row.isMerge) Fm.colors.accent else Fm.colors.textPrimary
    Canvas(Modifier.width((lanes * 14).dp).height(40.dp)) {
        val step = size.width / lanes
        fun x(lane: Int) = step * lane + step / 2
        val mid = size.height / 2
        for (lane in row.lanesIn) drawLine(laneColor, Offset(x(lane), 0f), Offset(x(lane), mid), 2f)
        for (lane in row.lanesOut) drawLine(laneColor, Offset(x(lane), mid), Offset(x(lane), size.height), 2f)
        for (edge in row.edges) drawLine(laneColor, Offset(x(edge.from), mid), Offset(x(edge.to), size.height), 2f)
        drawCircle(dotColor, radius = 5f, center = Offset(x(row.column), mid))
        if (row.overflow) drawCircle(laneColor, radius = 3f, center = Offset(size.width - 3f, 3f))
    }
}

internal class ConflictsScreen(private val repo: Repository, private val branch: String) : Screen() {
    override val title: String get() = "Conflicts"

    @Composable
    override fun Content(navigator: Navigator) {
        var base by remember { mutableStateOf("") }
        var clean by remember { mutableStateOf<Boolean?>(null) }
        var conflicts by remember { mutableStateOf<List<String>>(emptyList()) }
        var error by remember { mutableStateOf<String?>(null) }
        LaunchedEffect(Unit) {
            io {
                runCatching {
                    if (base.isEmpty()) base = CodeServices.api.branches(repo.owner, repo.name).firstOrNull { it.isDefault }?.name ?: "main"
                    val reply = CodeServices.api.jsonExchange(
                        "GET",
                        "/api/repo/${CodeServices.api.readOwner(repo.owner, repo.name)}/${repo.name}/merge-preview",
                        listOf("base" to base, "head" to branch, "ref" to ""),
                    )
                    if (!reply.ok) throw IllegalStateException(reply.error.ifEmpty { "The server answered ${reply.statusCode}." })
                    clean = reply.body["clean"] == true
                    conflicts = (reply.body["conflicts"] as? List<*>).orEmpty().map { it.toString() }.filter { it.isNotEmpty() }.take(200)
                }.onFailure { error = it.message ?: "$it" }
            }
        }
        DetailScaffold("Conflicts · $branch", navigator) {
            ScreenList {
                item {
                    InfoCard("$branch into ${base.ifEmpty { "…" }}") {
                        when {
                            error != null -> Note(error!!, Fm.colors.danger)
                            clean == true -> Note("Merges cleanly.", Fm.colors.success)
                            clean == false -> Note("${conflicts.size} conflicting file${if (conflicts.size == 1) "" else "s"}.", Fm.colors.danger)
                            else -> Note("Checking…")
                        }
                    }
                }
                items(conflicts) { path ->
                    MobileCard(icon = "description_outlined", iconColor = Fm.colors.danger, title = path, subtitle = "", onTap = {
                        navigator.push(CodeFeature.repoFileScreen(repo, path, branch))
                    })
                }
                if (conflicts.isNotEmpty()) {
                    item {
                        InfoCard("Resolve") {
                            Note("An agent merges ${base.ifEmpty { "the base branch" }} into $branch, resolves each conflict and pushes the branch.")
                            FmToneButton("Resolve with an agent", onPressed = {
                                navigator.push(AgentsFeature.startSheetScreen(ConflictResolution.agentPrompt(branch, base, conflicts)))
                            }, icon = "smart_toy_outlined", tone = FmTone.Accent)
                        }
                    }
                }
            }
        }
    }
}

private val MAP_SLOTS = listOf(
    Color(0xFF6161F2), Color(0xFF148A45), Color(0xFFC93532), Color(0xFFB7791F), Color(0xFF2B8FB5),
    Color(0xFF8E44AD), Color(0xFFD35400), Color(0xFF16A085), Color(0xFF7F8C8D),
)

internal class CodeMapScreen(private val repo: Repository) : Screen() {
    override val title: String get() = "Code map"

    @Composable
    override fun Content(navigator: Navigator) {
        var root by remember { mutableStateOf<CodeMapNode?>(null) }
        var error by remember { mutableStateOf<String?>(null) }
        var focus by remember { mutableStateOf("") }
        LaunchedEffect(Unit) {
            io {
                runCatching { CodeServices.settings(repo).sizes() }
                    .onSuccess { root = it }
                    .onFailure { error = it.message ?: "$it" }
            }
        }
        DetailScaffold("Code map · ${repo.name}", navigator) {
            Column(Modifier.fillMaxSize()) {
                val tree = root
                when {
                    error != null -> Box(Modifier.padding(FmSpace.x4)) { ErrorCard(error!!, {}) }
                    tree == null -> Box(Modifier.padding(FmSpace.x4)) { LoadingCard("Measuring the repository…") }
                    else -> {
                        val node = tree.find(focus) ?: tree
                        Row(Modifier.horizontalScroll(rememberScrollState()).padding(FmSpace.x2), horizontalArrangement = Arrangement.spacedBy(FmSpace.x1)) {
                            for (crumb in CodeMapLayout.breadcrumb(node.path)) FmToneButton(crumb.name, onPressed = { focus = crumb.path })
                        }
                        Note("  ${node.fileCount} files · ${CodeMapLayout.sizeLabel(node.size)}")
                        BoxWithConstraints(Modifier.fillMaxWidth().weight(1f).padding(FmSpace.x2)) {
                            val density = LocalDensity.current
                            val w = with(density) { maxWidth.toPx().toDouble() }
                            val h = with(density) { maxHeight.toPx().toDouble() }
                            val cells: List<CodeMapCell> = remember(node, w, h) { CodeMapLayout.cells(node, CodeMapRect(0.0, 0.0, w, h)) }
                            val border = Fm.colors.bgBase
                            Canvas(
                                Modifier
                                    .fillMaxSize()
                                    .pointerInput(cells) {
                                        detectTapGestures { offset ->
                                            val hit = CodeMapLayout.hit(cells, offset.x.toDouble(), offset.y.toDouble()) ?: return@detectTapGestures
                                            if (hit.node.isFile) {
                                                navigator.push(CodeFeature.repoFileScreen(repo, hit.node.path))
                                            } else {
                                                focus = hit.node.path
                                            }
                                        }
                                    },
                            ) {
                                for (cell in cells) {
                                    val base = MAP_SLOTS[cell.colorSlot % MAP_SLOTS.size]
                                    val alpha = (0.35f + 0.18f * cell.depth).coerceAtMost(0.9f)
                                    val topLeft = Offset(cell.rect.x.toFloat(), cell.rect.y.toFloat())
                                    val area = Size(cell.rect.width.toFloat(), cell.rect.height.toFloat())
                                    drawRect(base.copy(alpha = alpha), topLeft, area)
                                    drawRect(border, topLeft, area, style = Stroke(1f))
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}

internal class RepoSettingsScreen(private val repo: Repository) : Screen() {
    override val title: String get() = "Settings"

    @Composable
    override fun Content(navigator: Navigator) {
        val scope = rememberCoroutineScope()
        val saveStatus = rememberFmStatus()
        val forkStatus = rememberFmStatus()
        val client = remember { CodeServices.settings(repo) }
        var original by remember { mutableStateOf<RepoAbout?>(null) }
        var edited by remember { mutableStateOf(RepoAbout()) }
        var canEdit by remember { mutableStateOf(false) }
        var forks by remember { mutableStateOf<Pair<Int, List<RepoForkRow>>?>(null) }
        var error by remember { mutableStateOf<String?>(null) }
        var busy by remember { mutableStateOf(false) }
        suspend fun load() {
            io {
                runCatching { client.about() }
                    .onSuccess {
                        original = it
                        edited = it
                        error = null
                    }
                    .onFailure { error = it.message ?: "$it" }
                canEdit = RepoSettingsRules.canEdit(repo.owner, CodeServices.account, CodeServices.isOwner(repo))
                forks = runCatching { client.forks() }.getOrNull()
            }
        }
        LaunchedEffect(Unit) { load() }
        DetailScaffold("Settings · ${repo.name}", navigator) {
            ScreenList {
                error?.let { item { ErrorCard(it, { scope.launch { load() } }) } }
                val old = original
                if (old == null && error == null) item { LoadingCard("Loading settings…") }
                if (old != null) {
                    item {
                        InfoCard("About") {
                            FmTextField(edited.description, { edited = edited.copy(description = it) }, hint = "Description", enabled = canEdit, maxLines = 4)
                            val websiteError = RepoSettingsRules.validateWebsite(edited.website)
                            FmTextField(edited.website, { edited = edited.copy(website = it) }, hint = "Website", enabled = canEdit, errorText = websiteError)
                            if (edited.isPrivate) Kv("Visibility", "Private")
                        }
                    }
                    item {
                        InfoCard("Fediverse") {
                            if (edited.fediverseHandle.isNotEmpty()) Kv(edited.fediverseHandle, "${edited.followers} followers")
                            ToggleRow("Federate", edited.federate, canEdit) { edited = edited.copy(federate = it) }
                            ToggleRow("Broadcast events", edited.broadcastEvents, canEdit) { edited = edited.copy(broadcastEvents = it) }
                            ToggleRow("Accept replies", edited.acceptComments, canEdit) { edited = edited.copy(acceptComments = it) }
                        }
                    }
                    if (canEdit) {
                        item {
                            FmToneButton(if (busy) "Saving…" else "Save", onPressed = {
                                if (busy || RepoSettingsRules.validateWebsite(edited.website) != null) return@FmToneButton
                                scope.launch {
                                    busy = true
                                    val result = io { runCatching { client.saveAbout(old, edited) } }
                                    busy = false
                                    result.fold(
                                        { saved -> saveStatus.show(if (saved) "Saved." else "Nothing changed.") },
                                        { saveStatus.error(it.message ?: "$it") },
                                    )
                                    if (result.getOrNull() == true) load()
                                }
                            }, tone = FmTone.Accent, filled = true)
                        }
                        if (saveStatus.message != null) item { FmInlineStatus(saveStatus) }
                    } else {
                        item { Note("Only the repository owner and organization owners can change these.") }
                    }
                }
                item {
                    InfoCard("Forks${forks?.let { " · ${it.first}" } ?: ""}") {
                        for (row in forks?.second.orEmpty().take(50)) Text("${row.owner}/${row.name}", color = Fm.colors.textPrimary)
                        FmToneButton("Fork to my managed mirror", onPressed = {
                            scope.launch {
                                val outcome = io { runCatching { client.fork().message }.fold({ it to false }, { (it.message ?: "$it") to true }) }
                                forkStatus.report(outcome.first, outcome.second)
                                load()
                            }
                        }, icon = "call_split")
                        FmInlineStatus(forkStatus)
                    }
                }
                item {
                    InfoCard("More") {
                        FmToneButton("Mirror to GitHub or GitLab", onPressed = { navigator.push(ProviderMirrorScreen(repo)) }, icon = "sync_alt")
                        FmToneButton("Webhooks", onPressed = { navigator.push(WebhooksScreen(repo)) })
                        FmToneButton("Actions", onPressed = { navigator.push(ActionsScreen(repo)) })
                        FmToneButton("Open this repository's coves", onPressed = {
                            navigator.push(com.forkmesh.nativeapp.feature.coves.CovesFeature.covesScreen(repo.owner, repo.name))
                        }, icon = "shield_outlined")
                        Note("Renaming, transferring and deleting a repository are done on the desktop that owns it.")
                    }
                }
            }
        }
    }
}

@Composable
private fun ToggleRow(label: String, checked: Boolean, enabled: Boolean, onChange: (Boolean) -> Unit) {
    Row(verticalAlignment = Alignment.CenterVertically) {
        Text(label, Modifier.weight(1f), color = Fm.colors.textPrimary)
        FmSwitch(checked, onChange, enabled = enabled)
    }
}

@Composable
internal fun ArchiveCard(repo: Repository, ref: String) {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    val status = rememberFmStatus()
    var progress by remember { mutableStateOf("") }
    var pending by remember { mutableStateOf<RepoArchiveResult?>(null) }
    val saver = rememberLauncherForActivityResult(ActivityResultContracts.CreateDocument("application/zip")) { uri: Uri? ->
        val result = pending ?: return@rememberLauncherForActivityResult
        pending = null
        if (uri == null) return@rememberLauncherForActivityResult
        scope.launch {
            val ok = io { runCatching { context.contentResolver.openOutputStream(uri)?.use { it.write(result.data) } != null }.getOrDefault(false) }
            if (ok) status.show("Saved ${result.files} files.") else status.error("The ZIP could not be saved.")
        }
    }
    InfoCard("Download ZIP") {
        Note("Builds a ZIP of ${ref.ifEmpty { "the default branch" }} on this phone, file by file.")
        if (progress.isNotEmpty()) Note(progress)
        FmToneButton("Build ZIP", onPressed = {
            scope.launch {
                status.clear()
                progress = "Starting…"
                val result = io {
                    runCatching {
                        RepoArchiveBuilder(CodeServices.archiveReader()).build(repo.owner, repo.name, ref) { files, bytes ->
                            progress = "$files files · ${CodeMapLayout.sizeLabel(bytes.toLong())}"
                        }
                    }
                }
                result.onSuccess {
                    progress = if (it.skipped.isEmpty()) "" else "Skipped ${it.skipped.size} large or unreadable files."
                    pending = it
                    saver.launch(it.fileName)
                }.onFailure {
                    progress = ""
                    status.error(it.message ?: "$it")
                }
            }
        }, icon = "download")
        FmInlineStatus(status)
    }
}

internal class FileToolsScreen(private val repo: Repository, private val path: String, private val ref: String) : Screen() {
    override val title: String get() = path.substringAfterLast('/')

    @Composable
    override fun Content(navigator: Navigator) {
        var text by remember { mutableStateOf<String?>(null) }
        var error by remember { mutableStateOf<String?>(null) }
        var query by remember { mutableStateOf("") }
        var caseSensitive by remember { mutableStateOf(false) }
        var wholeWord by remember { mutableStateOf(false) }
        var goTo by remember { mutableStateOf("") }
        var symbol by remember { mutableStateOf<String?>(null) }
        val finder = remember { FindInText() }
        var focusLine by remember { mutableStateOf<Int?>(null) }
        LaunchedEffect(Unit) {
            io {
                runCatching { CodeServices.api.blob(repo.owner, repo.name, path, ref).content }
                    .onSuccess { text = it }
                    .onFailure { error = it.message ?: "$it" }
            }
        }
        DetailScaffold(title, navigator) {
            val body = text
            Column(Modifier.fillMaxSize()) {
                when {
                    error != null -> Box(Modifier.padding(FmSpace.x4)) { ErrorCard(error!!, {}) }
                    body == null -> Box(Modifier.padding(FmSpace.x4)) { LoadingCard("Loading…") }
                    else -> {
                        val lines = remember(body) { body.split("\n") }
                        val symbols = remember(body) { CodeOutline.symbols(body) }
                        finder.options = FindOptions(caseSensitive, wholeWord)
                        remember(query, caseSensitive, wholeWord, body) { finder.search(query, body); query }
                        Column(Modifier.padding(FmSpace.x3), verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                            FmTextField(query, { query = it }, hint = "Find", trailing = { Note(finder.status) })
                            Row(horizontalArrangement = Arrangement.spacedBy(FmSpace.x2), verticalAlignment = Alignment.CenterVertically) {
                                FmToneButton("Aa", onPressed = { caseSensitive = !caseSensitive }, filled = caseSensitive)
                                FmToneButton("Word", onPressed = { wholeWord = !wholeWord }, filled = wholeWord)
                                FmToneButton("Prev", onPressed = { finder.previous(); focusLine = finder.currentMatch?.line })
                                FmToneButton("Next", onPressed = { finder.next(); focusLine = finder.currentMatch?.line })
                            }
                            Row(horizontalArrangement = Arrangement.spacedBy(FmSpace.x2), verticalAlignment = Alignment.CenterVertically) {
                                FmTextField(goTo, { goTo = it }, Modifier.weight(1f), hint = "Go to line")
                                FmToneButton("Go", onPressed = { focusLine = GoToLine.parse(goTo, lines.size) })
                            }
                        }
                        val refs = remember(symbol, body) { symbol?.let { name -> CodeOutline.references(name, body, symbols.firstOrNull { it.name == name }?.line) }.orEmpty() }
                        LazyColumn(Modifier.weight(1f).fillMaxWidth()) {
                            if (symbols.isNotEmpty()) {
                                item { Box(Modifier.padding(horizontal = FmSpace.x3)) { Text("Outline", style = Fm.text.titleSmall, color = Fm.colors.textPrimary) } }
                                items(symbols.take(300), key = { "${it.line}:${it.name}" }) { item ->
                                    Row(
                                        Modifier
                                            .fillMaxWidth()
                                            .clickable { focusLine = item.line; symbol = item.name }
                                            .padding(start = FmSpace.x3 + (item.depth * 12).dp, end = FmSpace.x3, top = 4.dp, bottom = 4.dp),
                                    ) {
                                        Mono("${item.kind} ${item.name}")
                                        Note("  :${item.line}")
                                    }
                                }
                            }
                            if (symbol != null) {
                                item { Box(Modifier.padding(FmSpace.x3)) { Text("References to $symbol · ${refs.size}", style = Fm.text.titleSmall, color = Fm.colors.textPrimary) } }
                                items(refs, key = { it.line }) { ref ->
                                    Row(Modifier.fillMaxWidth().clickable { focusLine = ref.line }.padding(horizontal = FmSpace.x3, vertical = 4.dp)) {
                                        Mono("${ref.line}${if (ref.isCall) " call" else ""}  ${ref.text}")
                                    }
                                }
                            }
                            val line = focusLine
                            if (line != null && line in 1..lines.size) {
                                item { Box(Modifier.padding(FmSpace.x3)) { Text("Line $line", style = Fm.text.titleSmall, color = Fm.colors.textPrimary) } }
                                itemsIndexed(lines.subList(maxOf(0, line - 6), minOf(lines.size, line + 5))) { index, content ->
                                    val number = maxOf(0, line - 6) + index + 1
                                    Row(
                                        Modifier
                                            .fillMaxWidth()
                                            .background(if (number == line) Fm.colors.accent.copy(alpha = 0.18f) else Fm.colors.bgBase)
                                            .horizontalScroll(rememberScrollState())
                                            .padding(horizontal = FmSpace.x3),
                                    ) {
                                        Mono("$number".padStart(5) + "  " + content)
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
