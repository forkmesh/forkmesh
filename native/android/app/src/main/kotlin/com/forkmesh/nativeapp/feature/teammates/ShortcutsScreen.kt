package com.forkmesh.nativeapp.feature.teammates

import androidx.compose.foundation.background
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
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.material3.CircularProgressIndicator
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
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.feature.agents.opsIo
import com.forkmesh.nativeapp.feature.agents.rememberOpsTick
import com.forkmesh.nativeapp.feature.code.DetailScaffold
import com.forkmesh.nativeapp.feature.code.FmOutlinedButton
import com.forkmesh.nativeapp.teammates.ShortcutEntry
import com.forkmesh.nativeapp.teammates.ShortcutProgress
import com.forkmesh.nativeapp.teammates.ShortcutRowStatus
import com.forkmesh.nativeapp.teammates.ShortcutRun
import com.forkmesh.nativeapp.teammates.TeamErrorText
import com.forkmesh.nativeapp.ui.kit.FmCard
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenu
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenuItem
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmLinearProgressIndicator
import com.forkmesh.nativeapp.ui.kit.FmSectionHeader
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmSpace
import com.forkmesh.nativeapp.ui.theme.FmType
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch

internal class ShortcutsScreen(private val opened: String) : Screen() {
    override val title: String get() = "Shortcuts"
    private var repo by mutableStateOf(opened)
    private var pending by mutableStateOf<PendingTeamConfirmation?>(null)
    private var pendingScript by mutableStateOf(false)
    private var jumpToOutput by mutableStateOf(0)
    private var banner by mutableStateOf("")
    private var error by mutableStateOf("")
    private var shown by mutableStateOf<Pair<ShortcutEntry, String>?>(null)

    @Composable
    override fun Content(navigator: Navigator) {
        val c = Fm.colors
        val store = remember { TeammatesFeature.shortcuts }
        val tick = rememberOpsTick(store)
        val scope = rememberCoroutineScope()
        val list = rememberLazyListState()
        LaunchedEffect(jumpToOutput) {
            if (jumpToOutput > 0) list.animateScrollToItem(2)
        }
        suspend fun load(target: String) {
            opsIo { store.refresh(target) }
            val picked = store.catalog?.pick(target) ?: return
            if (picked != target) {
                repo = picked
                opsIo { store.refresh(picked) }
            }
        }
        LaunchedEffect(Unit) { load(repo) }
        fun open(entry: ShortcutEntry) {
            scope.launch {
                try {
                    shown = entry to opsIo { store.text(repo, entry) }
                } catch (cancelled: CancellationException) {
                    throw cancelled
                } catch (failure: Exception) {
                    error = TeamErrorText.describe(failure)
                }
            }
        }
        fun prepare(entry: ShortcutEntry) {
            scope.launch {
                try {
                    pending = PendingTeamConfirmation("Run ${entry.name}", opsIo { store.prepare(repo, entry) })
                    pendingScript = entry.isScript
                    error = ""
                } catch (cancelled: CancellationException) {
                    throw cancelled
                } catch (failure: Exception) {
                    error = TeamErrorText.describe(failure)
                }
            }
        }
        DetailScaffold(title, navigator, actions = {
            FmIconButton("refresh", "Refresh", onPressed = { scope.launch { load(repo) } }, color = c.textPrimary)
        }) {
            tick.let { }
            val catalog = store.catalog
            LazyColumn(Modifier.fillMaxSize(), state = list, contentPadding = PaddingValues(FmSpace.x4), verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                item {
                    Column {
                        DesktopReachText(TeammatesFeature.hub.reach)
                        if (store.unsupported) OutdatedRow()
                        Banner(banner, false)
                        Banner(error.ifEmpty { if (store.unsupported) "" else store.error }, true)
                        if (store.loading) CircularProgressIndicator(Modifier.padding(FmSpace.x2))
                    }
                }
                item {
                    var choosing by remember { mutableStateOf(false) }
                    val repos = catalog?.repos.orEmpty()
                    Column {
                        Box {
                            FmOutlinedButton(repo.ifEmpty { "Choose a repository" }, onClick = if (repos.isEmpty()) null else ({ choosing = true }), icon = "folder_outlined")
                            FmDropdownMenu(expanded = choosing, onDismissRequest = { choosing = false }) {
                                for (choice in repos) {
                                    FmDropdownMenuItem(choice, icon = if (choice == repo) "check" else null, onClick = {
                                        choosing = false
                                        repo = choice
                                        shown = null
                                        scope.launch { opsIo { store.refresh(choice) } }
                                    })
                                }
                            }
                        }
                        if (catalog != null && opened.isNotEmpty() && repos.none { it.equals(opened, ignoreCase = true) }) {
                            Text("The desktop has no working tree of $opened.", style = Fm.text.bodySmall.copy(color = c.textSecondary))
                        }
                    }
                }
                val shownRuns = store.newestRuns.filter { it.running }.ifEmpty { listOfNotNull(store.lastRun) }
                if (shownRuns.isNotEmpty()) {
                    item { FmSectionHeader("Output") }
                    items(shownRuns, key = { "output-${it.id}" }) { run -> ShortcutRunCard(run) }
                }
                if (catalog != null) {
                    if (catalog.shortcuts.isNotEmpty()) {
                        item { FmSectionHeader("Shortcuts", count = catalog.shortcuts.size) }
                        items(catalog.shortcuts, key = { it.id }) { entry -> ShortcutRow(entry, store.status(repo, entry), ::open, ::prepare) }
                    }
                    if (catalog.skills.isNotEmpty()) {
                        item { FmSectionHeader("Skills", count = catalog.skills.size) }
                        items(catalog.skills, key = { it.id }) { entry -> ShortcutRow(entry, store.status(repo, entry), ::open, ::prepare) }
                    }
                    if (catalog.entries.isEmpty() && !store.loading) {
                        item { Text("No shortcuts or skills here yet.", style = Fm.text.bodyMedium.copy(color = c.textSecondary)) }
                    }
                }
                shown?.let { (entry, text) ->
                    item { FmSectionHeader(entry.name) }
                    if (!entry.runnable) {
                        item {
                            Text(
                                if (entry.kind == "script") "Script shortcuts run on the desktop itself." else "This one has nothing to send to an agent.",
                                style = Fm.text.bodySmall.copy(color = c.textSecondary),
                            )
                        }
                    }
                    item { FmCard(Modifier.fillMaxWidth()) { SelectionContainer { Text(text, style = Fm.text.bodySmall.copy(color = c.textPrimary)) } } }
                }
            }
            pending?.let { confirm ->
                TeamConfirmDialog(
                    confirm,
                    perform = { store.run(it) },
                    onDone = {
                        banner = it.ifEmpty { "Done." }
                        if (pendingScript) jumpToOutput++
                    },
                    onError = { error = it },
                    onDismiss = { pending = null },
                )
            }
        }
    }
}

@Composable
private fun ShortcutRow(entry: ShortcutEntry, status: ShortcutRowStatus, onOpen: (ShortcutEntry) -> Unit, onRun: (ShortcutEntry) -> Unit) {
    val c = Fm.colors
    FmCard(Modifier.fillMaxWidth(), onTap = { onOpen(entry) }) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            FmIcon(if (entry.isSkill) "auto_awesome" else if (entry.kind == "script") "terminal" else "chat_bubble_outline", size = 20.dp, tint = c.textTertiary)
            Spacer(Modifier.width(FmSpace.x3))
            Column(Modifier.weight(1f)) {
                Text(entry.name.ifEmpty { entry.file }, style = Fm.text.bodyMedium.copy(color = c.textPrimary, fontWeight = FmType.emphasis))
                val detail = listOf(entry.description, entry.agent, entry.model, if (entry.uses > 0) "${entry.uses} uses" else "").filter { it.isNotEmpty() }
                if (detail.isNotEmpty()) Text(detail.joinToString(" · "), style = Fm.text.bodySmall.copy(color = c.textSecondary), maxLines = 2)
                if (status.ran) {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        if (status.running) {
                            CircularProgressIndicator(Modifier.size(10.dp), strokeWidth = 1.5.dp)
                        } else {
                            FmIcon(if (status.ok) "check_circle" else "error", size = 12.dp, tint = if (status.ok) c.success else c.danger)
                        }
                        Spacer(Modifier.width(FmSpace.x1))
                        Text(
                            status.line.ifEmpty { if (status.running) "running" else "no output" },
                            style = Fm.text.bodySmall.copy(color = c.textSecondary, fontFamily = FontFamily.Monospace),
                            maxLines = 1,
                            overflow = TextOverflow.Ellipsis,
                        )
                    }
                }
                ShortcutTiming(status)
            }
            if (entry.runnable) FmOutlinedButton("Run", onClick = { onRun(entry) }, icon = "play_arrow")
        }
    }
}

@Composable
private fun ShortcutRunCard(run: ShortcutRun) {
    val c = Fm.colors
    var expanded by remember { mutableStateOf(false) }
    FmCard(Modifier.fillMaxWidth(), onTap = { expanded = !expanded }) {
        Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                if (run.running) {
                    CircularProgressIndicator(Modifier.size(16.dp), strokeWidth = 2.dp)
                } else {
                    FmIcon(if (run.ok) "check_circle" else "error", size = 18.dp, tint = if (run.ok) c.success else c.danger)
                }
                Spacer(Modifier.width(FmSpace.x2))
                Text(run.title, style = Fm.text.bodyMedium.copy(color = c.textPrimary, fontWeight = FmType.emphasis), modifier = Modifier.weight(1f))
                Text(if (run.running) "running" else run.message, style = Fm.text.bodySmall.copy(color = c.textSecondary))
            }
            val now = rememberNowMs(run.running && run.startedAtMs > 0)
            run.progress(now)?.let { ShortcutProgressBar(it) }
            val lines = run.output.trimEnd('\n').split('\n')
            val shown = if (expanded) lines else lines.takeLast(40)
            SelectionContainer {
                Text(
                    shown.joinToString("\n") + if (run.running) "\n\u2588" else "",
                    style = Fm.text.bodySmall.copy(color = Color(0xFFE6E6E6), fontFamily = FontFamily.Monospace),
                    modifier = Modifier.fillMaxWidth().background(Color.Black).padding(FmSpace.x2),
                )
            }
            if (!expanded && lines.size > shown.size) {
                Text("Tap for all ${lines.size} lines", style = Fm.text.bodySmall.copy(color = c.textTertiary))
            }
        }
    }
}

@Composable
private fun rememberNowMs(active: Boolean): Long {
    var now by remember { mutableStateOf(System.currentTimeMillis()) }
    LaunchedEffect(active) {
        while (active) {
            now = System.currentTimeMillis()
            delay(1000)
        }
    }
    return now
}

@Composable
private fun ShortcutTiming(status: ShortcutRowStatus) {
    val now = rememberNowMs(status.running && status.startedAtMs > 0)
    val progress = status.progress(now)
    if (progress != null) {
        ShortcutProgressBar(progress)
    } else {
        status.tookText?.let { Text(it, style = Fm.text.bodySmall.copy(color = Fm.colors.textTertiary)) }
    }
}

@Composable
private fun ShortcutProgressBar(progress: ShortcutProgress) {
    Column(verticalArrangement = Arrangement.spacedBy(2.dp)) {
        val fraction = progress.fraction
        FmLinearProgressIndicator(Modifier.height(4.dp), progress = fraction?.let { value -> { value } })
        Text(
            progress.text,
            style = Fm.text.bodySmall.copy(color = Fm.colors.textSecondary),
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
        )
    }
}
