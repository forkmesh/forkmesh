package com.forkmesh.nativeapp.feature.codeinsight

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.liveRegion
import androidx.compose.ui.semantics.LiveRegionMode
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.codeinsight.InsightHistoryDepth
import com.forkmesh.nativeapp.codeinsight.InsightLabels
import com.forkmesh.nativeapp.codeinsight.InsightSnapshot
import com.forkmesh.nativeapp.codeinsight.InsightTarget
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.code.CodeAppBar
import com.forkmesh.nativeapp.feature.code.CodeFeature
import com.forkmesh.nativeapp.feature.code.ErrorCard
import com.forkmesh.nativeapp.feature.code.repoApi
import com.forkmesh.nativeapp.feature.localrepos.LocalFileEditorScreen
import com.forkmesh.nativeapp.localrepos.LocalRepoRecord
import com.forkmesh.nativeapp.localrepos.LocalRepoStore
import com.forkmesh.nativeapp.model.Repository
import com.forkmesh.nativeapp.navigation.RepoRef
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenu
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenuItem
import com.forkmesh.nativeapp.ui.kit.FmEmptyState
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmInlineStatusLine
import com.forkmesh.nativeapp.ui.kit.FmLinearProgressIndicator
import com.forkmesh.nativeapp.ui.kit.FmListRow
import com.forkmesh.nativeapp.ui.kit.FmModalBottomSheet
import com.forkmesh.nativeapp.ui.kit.FmSectionHeader
import com.forkmesh.nativeapp.ui.kit.FmTone
import com.forkmesh.nativeapp.ui.kit.FmToneButton
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmSpace
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

class CodeInsightScreen(
    val plugin: CodeInsightPlugin,
    val repo: RepoRef?,
    private val content: @Composable (CodeInsightContext, Navigator) -> Unit,
) : Screen() {
    override val title: String get() = plugin.title

    private var target by mutableStateOf<InsightTarget?>(null)
    private var picking by mutableStateOf(false)
    private var summary by mutableStateOf("")
    private var resolved = false
    private var held: InsightTarget? = null
    private var preferredLocal: String? = null

    fun withLocalCopy(name: String): CodeInsightScreen {
        preferredLocal = name
        return this
    }

    override fun onDisposed() {
        held?.let(CodeInsightSession::release)
        held = null
    }

    private fun choose(next: InsightTarget) {
        picking = false
        if (target == next) return
        summary = ""
        target = next
    }

    private fun hold(next: InsightTarget) {
        if (held == next) return
        held?.let(CodeInsightSession::release)
        held = next
        CodeInsightSession.acquire(next)
    }

    private fun resolveInitial() {
        if (resolved) return
        resolved = true
        val local = preferredLocal
        val initial = when {
            local != null -> InsightTarget.Local(local)
            repo != null -> InsightTarget.Account(repo.owner, repo.name)
            else -> CodeInsightSession.lastTarget()?.takeIf { it !is InsightTarget.Local || localStore()?.find(it.name) != null }
        }
        if (initial == null) picking = true else target = initial
    }

    @Composable
    override fun Content(navigator: Navigator) {
        LaunchedEffect(Unit) { withContext(Dispatchers.IO) { localStore()?.ensureLoaded() }; resolveInitial() }
        val current = target
        LaunchedEffect(current) { if (current != null) hold(current) }
        val depth by CodeInsightSession.depth.collectAsState()
        val c = Fm.colors
        Column(Modifier.fillMaxSize().background(c.bgBase)) {
            CodeAppBar(
                title = plugin.title,
                navigator = navigator,
                titleStyle = Fm.text.titleSmall.copy(color = c.textPrimary),
                actions = { HistoryMenu(depth) },
            )
            if (current == null) {
                Box(Modifier.weight(1f).fillMaxWidth()) {
                    FmEmptyState(plugin.icon, "Choose a repository", message = "${plugin.title} analyzes the code of one repository on this phone.")
                }
                Row(Modifier.fillMaxWidth().padding(FmSpace.x4), horizontalArrangement = Arrangement.Center) {
                    FmToneButton("Choose repository", onPressed = { picking = true }, icon = "folder_outlined", tone = FmTone.Accent, filled = true)
                }
            } else {
                val state by CodeInsightSession.state(current).collectAsState()
                Header(current, state)
                Box(Modifier.weight(1f).fillMaxWidth()) {
                    Body(current, state, navigator)
                }
            }
        }
        if (picking) {
            RepositoryPicker(current, onPick = ::choose, onDismiss = { picking = false })
        }
    }

    @Composable
    private fun HistoryMenu(depth: InsightHistoryDepth) {
        var open by remember { mutableStateOf(false) }
        Box {
            FmIconButton("history", "History depth: ${depth.label}", onPressed = { open = true }, color = Fm.colors.textPrimary, iconSize = 24.dp)
            FmDropdownMenu(expanded = open, onDismissRequest = { open = false }) {
                for (option in InsightHistoryDepth.entries) {
                    FmDropdownMenuItem(
                        text = option.label,
                        icon = if (option == depth) "check" else null,
                        onClick = {
                            open = false
                            CodeInsightSession.setDepth(option, target)
                        },
                    )
                }
            }
        }
    }

    @Composable
    private fun Header(current: InsightTarget, state: CodeInsightState) {
        val c = Fm.colors
        Column(
            Modifier.fillMaxWidth().padding(horizontal = FmSpace.x4, vertical = FmSpace.x2),
            verticalArrangement = Arrangement.spacedBy(FmSpace.x1),
        ) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                FmToneButton(
                    label = targetTitle(current),
                    onPressed = { picking = true },
                    icon = if (current is InsightTarget.Local) "phone_android" else "cloud_outlined",
                    tooltip = "Choose repository",
                    modifier = Modifier.weight(1f, fill = false),
                )
                Spacer(Modifier.weight(1f))
                FmToneButton(
                    label = "Re-analyze",
                    onPressed = if (state.running) null else ({ CodeInsightSession.reanalyze(current) }),
                    icon = "refresh",
                )
            }
            val snapshot = state.snapshot
            val line = if (snapshot != null) {
                InsightLabels.statusLine(state.sourceLabel, snapshot.files.size, summary)
            } else {
                state.sourceLabel
            }
            if (line.isNotEmpty()) {
                Text(line, style = Fm.text.bodySmall, color = c.textSecondary, maxLines = 2, overflow = TextOverflow.Ellipsis)
            }
            if (state.running) {
                FmLinearProgressIndicator(progress = state.fraction?.let { fraction -> { fraction } })
                Text(
                    state.stage,
                    style = Fm.text.bodySmall,
                    color = c.textTertiary,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                    modifier = Modifier.semantics { liveRegion = LiveRegionMode.Polite },
                )
            }
            val error = state.error
            if (error != null && snapshot != null && snapshot.error.isEmpty()) FmInlineStatusLine(message = error, isError = true)
            state.warning?.let { FmInlineStatusLine(message = it, isError = false) }
        }
    }

    @Composable
    private fun Body(current: InsightTarget, state: CodeInsightState, navigator: Navigator) {
        val snapshot = state.snapshot
        val error = state.error
        when {
            snapshot != null && snapshot.error.isEmpty() -> {
                val context = remember(snapshot, current, state.sourceLabel) { contextFor(current, snapshot, state.sourceLabel, navigator) }
                content(context, navigator)
            }
            state.running -> Box(Modifier.fillMaxSize())
            error != null -> Box(Modifier.padding(FmSpace.x4)) { ErrorCard(error, onRetry = { CodeInsightSession.reanalyze(current) }) }
            else -> Box(Modifier.fillMaxSize())
        }
    }

    private fun contextFor(current: InsightTarget, snapshot: InsightSnapshot, sourceLabel: String, navigator: Navigator) = CodeInsightContext(
        snapshot = snapshot,
        repoTitle = targetTitle(current),
        branch = snapshot.branch,
        sourceLabel = sourceLabel,
        openFile = { path, line -> openFile(current, snapshot, path, line, navigator) },
        loadTrend = { path -> CodeInsightSession.trend(current, path) },
        reanalyze = { CodeInsightSession.reanalyze(current) },
        setSummary = { text -> if (target == current) summary = text },
    )

    @Suppress("UNUSED_PARAMETER")
    private fun openFile(current: InsightTarget, snapshot: InsightSnapshot, path: String, line: Int, navigator: Navigator) {
        when (current) {
            is InsightTarget.Account -> navigator.push(
                CodeFeature.repoFileScreen(Repository(current.owner, current.name), path, snapshot.branch),
            )
            is InsightTarget.Local -> {
                val store = localStore() ?: return
                val record = store.find(current.name) ?: return
                val workspace = store.workspace(record)
                navigator.push(LocalFileEditorScreen(workspace, path, record.name) { workspace.dispose() })
            }
        }
    }

    @OptIn(ExperimentalMaterial3Api::class)
    @Composable
    private fun RepositoryPicker(current: InsightTarget?, onPick: (InsightTarget) -> Unit, onDismiss: () -> Unit) {
        var locals by remember { mutableStateOf<List<LocalRepoRecord>>(emptyList()) }
        var accounts by remember { mutableStateOf<List<Repository>?>(null) }
        var problem by remember { mutableStateOf<String?>(null) }
        LaunchedEffect(Unit) {
            locals = withContext(Dispatchers.IO) { localStore()?.let { it.ensureLoaded(); it.records }.orEmpty() }
            try {
                accounts = withContext(Dispatchers.IO) { repoApi.repositories() }
            } catch (error: Exception) {
                accounts = emptyList()
                problem = CodeInsightSession.describe(error)
            }
        }
        val c = Fm.colors
        FmModalBottomSheet(onDismissRequest = onDismiss, showDragHandle = true) {
            Text(
                "Choose a repository",
                style = Fm.text.titleMedium,
                color = c.textPrimary,
                modifier = Modifier.padding(horizontal = FmSpace.x4, vertical = FmSpace.x2),
            )
            LazyColumn(Modifier.fillMaxWidth().heightIn(max = 520.dp)) {
                if (locals.isNotEmpty()) {
                    item(key = "local-header") {
                        FmSectionHeader("On this phone", Modifier.padding(horizontal = FmSpace.x4, vertical = FmSpace.x2), icon = "phone_android", count = locals.size)
                    }
                    items(locals, key = { "local:" + it.name }) { record ->
                        val option = InsightTarget.Local(record.name, record.publishedAs)
                        PickerRow(
                            title = record.name,
                            subtitle = record.publishedAs ?: "Local only",
                            selected = current?.key == option.key,
                            onTap = { onPick(option) },
                        )
                    }
                }
                item(key = "account-header") {
                    FmSectionHeader("Account repositories", Modifier.padding(horizontal = FmSpace.x4, vertical = FmSpace.x2), icon = "cloud_outlined", count = accounts?.size)
                }
                val list = accounts
                when {
                    list == null -> item(key = "loading") {
                        FmLinearProgressIndicator(Modifier.padding(horizontal = FmSpace.x4, vertical = FmSpace.x2))
                    }
                    list.isEmpty() -> item(key = "empty") {
                        Text(
                            problem ?: "No repositories in this account.",
                            style = Fm.text.bodySmall,
                            color = c.textSecondary,
                            modifier = Modifier.padding(horizontal = FmSpace.x4, vertical = FmSpace.x2),
                        )
                    }
                    else -> items(list, key = { "repo:" + it.owner + "/" + it.name }) { repository ->
                        val option = InsightTarget.Account(repository.owner, repository.name)
                        PickerRow(
                            title = repository.name,
                            subtitle = repository.fullName,
                            selected = current?.key == option.key,
                            onTap = { onPick(option) },
                        )
                    }
                }
                item(key = "bottom") { Spacer(Modifier.padding(bottom = FmSpace.x5)) }
            }
        }
    }

    @Composable
    private fun PickerRow(title: String, subtitle: String, selected: Boolean, onTap: () -> Unit) {
        FmListRow(
            title = title,
            subtitle = subtitle,
            onTap = onTap,
            showChevron = false,
            trailing = if (selected) {
                { FmIcon("check", size = 20.dp, tint = Fm.colors.accent) }
            } else {
                null
            },
            modifier = Modifier.semantics { contentDescription = if (selected) "$title, selected" else title },
        )
    }

    private fun targetTitle(current: InsightTarget): String = when (current) {
        is InsightTarget.Local -> current.name
        is InsightTarget.Account -> current.slug
    }

    private fun localStore(): LocalRepoStore? = Services.get<LocalRepoStore>()
}
