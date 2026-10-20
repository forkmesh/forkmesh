package com.forkmesh.nativeapp.feature.code

import androidx.compose.foundation.background
import com.forkmesh.nativeapp.ui.kit.FmSheet
import androidx.compose.foundation.clickable
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.RowScope
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.IconButtonDefaults
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.Stable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.LocalWindowInfo
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.code.RepoExplorer
import com.forkmesh.nativeapp.model.RepoBranch
import com.forkmesh.nativeapp.model.RepoCodeSearchMatch
import com.forkmesh.nativeapp.model.RepoTree
import com.forkmesh.nativeapp.model.RepoTreeEntry
import com.forkmesh.nativeapp.model.Repository
import com.forkmesh.nativeapp.ui.kit.FmBottomBar
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenu
import com.forkmesh.nativeapp.ui.kit.FmEmptyState
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmIconTile
import com.forkmesh.nativeapp.ui.kit.FmListRow
import com.forkmesh.nativeapp.ui.kit.FmMessenger
import com.forkmesh.nativeapp.ui.kit.FmRowDivider
import com.forkmesh.nativeapp.ui.kit.FmSkeletonList
import com.forkmesh.nativeapp.ui.kit.FmTextField
import com.forkmesh.nativeapp.ui.kit.FmTone
import com.forkmesh.nativeapp.ui.kit.FmToneButton
import com.forkmesh.nativeapp.ui.kit.FmTooltip
import com.forkmesh.nativeapp.ui.kit.LocalFmMessenger
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmRadius
import com.forkmesh.nativeapp.ui.theme.FmSpace
import com.forkmesh.nativeapp.ui.theme.FmType
import kotlinx.coroutines.launch

@Stable
internal class CodeTabState(val repo: Repository) {
    var path by mutableStateOf("")
        private set

    var selectedRef by mutableStateOf(repo.defaultBranch)
        private set

    val branches = AsyncValue<List<RepoBranch>>()
    val tree = AsyncValue<RepoTree>()

    fun ensureLoaded() {
        if (!branches.started) branches.reload { repoApi.branches(repo.owner, repo.name) }
        if (!tree.started) loadTree("")
    }

    private fun loadTree(path: String) {
        val ref = selectedRef
        tree.reload { repoApi.tree(repo.owner, repo.name, path = path, ref = ref) }
    }

    fun openDir(path: String, refresh: Boolean = false) {
        if (refresh) repoApi.clearRepoCache(repo.owner, repo.name)
        this.path = path
        loadTree(path)
    }

    fun switchRef(ref: String) {
        if (ref == selectedRef) return
        repoApi.clearRepoCache(repo.owner, repo.name)
        selectedRef = ref
        path = ""
        loadTree("")
    }

    fun up() {
        if (path.isEmpty()) return
        openDir(RepoExplorer.parentPath(path))
    }
}

@Composable
internal fun CodeTab(state: CodeTabState, navigator: Navigator) {
    state.branches.Bind()
    state.tree.Bind()
    val c = Fm.colors
    val messenger = LocalFmMessenger.current
    val scope = rememberCoroutineScope()
    val openFile: (String, String) -> Unit = { path, _ -> navigator.push(RepoFileScreen(state.repo, path, state.selectedRef)) }
    Column(
        Modifier
            .fillMaxSize()
            .background(c.listSurface),
    ) {
        val loading = state.tree.waiting || !state.tree.started
        val tree = state.tree.data
        val error = state.tree.error
        val source = if (loading) "" else tree?.source ?: ""
        LazyColumn(
            Modifier
                .weight(1f)
                .fillMaxWidth(),
        ) {
            item {
                ExplorerHeader(state, source = source, showDescription = state.path.isEmpty())
            }
            when {
                loading -> item { LoadingCard("Loading files…", Modifier.padding(FmSpace.x4)) }
                error != null -> item {
                    ErrorCard(
                        message = "Could not load files: $error",
                        onRetry = { state.openDir(state.path, refresh = true) },
                        modifier = Modifier.padding(FmSpace.x4),
                    )
                }
                tree == null || tree.entries.isEmpty() -> item {
                    EmptyCard("No files found in this folder.", Modifier.padding(FmSpace.x4))
                }
                else -> {
                    item { HorizontalDivider(thickness = 1.dp, color = c.border) }
                    itemsIndexed(tree.entries) { index, entry ->
                        Column {
                            FileRow(entry) { if (entry.isDirectory) state.openDir(entry.path) else openFile(entry.path, entry.sha) }
                            if (index != tree.entries.lastIndex) FmRowDivider()
                        }
                    }
                }
            }
            item { Spacer(Modifier.height(FmSpace.x4)) }
        }
        ExplorerPathBar(
            path = state.path,
            onUp = if (state.path.isEmpty()) null else state::up,
            onPath = { state.openDir(it) },
            onGoToFile = {
                scope.launch {
                    val selected = messenger.showGoToFileSheet(state.repo, state.selectedRef) ?: return@launch
                    openFile(selected, "")
                }
            },
        )
    }
}

@Composable
private fun ExplorerHeader(state: CodeTabState, source: String, showDescription: Boolean) {
    val c = Fm.colors
    Column(
        Modifier
            .fillMaxWidth()
            .padding(horizontal = FmSpace.x4, vertical = FmSpace.x3),
    ) {
        if (showDescription && state.repo.description.isNotEmpty()) {
            Text(
                state.repo.description,
                maxLines = 2,
                overflow = TextOverflow.Ellipsis,
                style = Fm.text.bodyMedium.copy(color = c.textSecondary, lineHeight = FmType.small * 1.35f),
            )
            Spacer(Modifier.height(FmSpace.x3))
        }
        Row(verticalAlignment = Alignment.CenterVertically) {
            Box(Modifier.weight(1f, fill = false)) { BranchSelector(state) }
            if (RepoExplorer.sourceLabel(source) != null) {
                Spacer(Modifier.width(FmSpace.x2))
                Box(Modifier.weight(1f, fill = false)) { SourceChip(source) }
            }
        }
    }
}

@Composable
private fun BranchSelector(state: CodeTabState) {
    val c = Fm.colors
    val branches = state.branches
    val names = RepoExplorer.branchMenuNames(state.selectedRef, branches.data.orEmpty())
    val canSwitch = names.isNotEmpty()
    var open by remember { mutableStateOf(false) }
    Box {
        FmTooltip("Switch branch") {
            BranchPill(
                selectedRef = state.selectedRef,
                canSwitch = canSwitch,
                loading = branches.waiting || !branches.started,
                hasError = branches.error != null,
                modifier = Modifier
                    .clip(RoundedCornerShape(FmRadius.md))
                    .clickable(enabled = canSwitch, onClickLabel = "Switch branch") { open = true },
            )
        }
        FmDropdownMenu(expanded = open, onDismissRequest = { open = false }) {
            for (name in names) {
                val current = name == state.selectedRef
                DropdownMenuItem(
                    text = {
                        Text(
                            name,
                            maxLines = 1,
                            overflow = TextOverflow.Ellipsis,
                            style = Fm.text.bodyMedium.copy(fontWeight = FmType.medium),
                        )
                    },
                    leadingIcon = {
                        FmIcon(
                            if (current) "check_circle" else "circle_outlined",
                            size = 16.dp,
                            tint = if (current) c.accent else c.textTertiary,
                        )
                    },
                    onClick = {
                        open = false
                        state.switchRef(name)
                    },
                )
            }
        }
    }
}

@Composable
private fun BranchPill(selectedRef: String, canSwitch: Boolean, loading: Boolean, hasError: Boolean, modifier: Modifier) {
    val c = Fm.colors
    val muted = !canSwitch || loading || hasError
    Row(
        modifier
            .height(36.dp)
            .background(c.bgOverlay, RoundedCornerShape(FmRadius.md))
            .padding(horizontal = FmSpace.x3),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        FmIcon("account_tree_outlined", size = 16.dp, tint = if (muted) c.textTertiary else c.success)
        Spacer(Modifier.width(FmSpace.x2))
        Text(
            RepoExplorer.branchPillLabel(selectedRef),
            modifier = Modifier.weight(1f, fill = false),
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
            style = Fm.text.labelMedium.copy(color = c.textPrimary),
        )
        Spacer(Modifier.width(FmSpace.x1))
        if (loading) {
            SmallSpinner(14.dp, c.textTertiary)
        } else {
            FmIcon(if (canSwitch) "keyboard_arrow_down_rounded" else "lock_outline", size = 17.dp, tint = c.textTertiary)
        }
    }
}

@Composable
private fun FileRow(entry: RepoTreeEntry, onTap: () -> Unit) {
    val c = Fm.colors
    FmListRow(
        title = entry.name,
        subtitle = RepoExplorer.fileRowMeta(entry),
        leading = {
            FmIconTile(
                if (entry.isDirectory) "folder_outlined" else RepoExplorer.fileGlyph(entry.name),
                tint = if (entry.isDirectory) c.accent else null,
            )
        },
        onTap = onTap,
    )
}

@Composable
private fun ExplorerPathBar(path: String, onUp: (() -> Unit)?, onPath: (String) -> Unit, onGoToFile: () -> Unit) {
    val c = Fm.colors
    FmBottomBar(
        leading = {
            Row(verticalAlignment = Alignment.CenterVertically) {
                FmIconButton(
                    "arrow_back_rounded",
                    "Up",
                    onPressed = onUp,
                    colors = IconButtonDefaults.iconButtonColors(
                        contentColor = c.textSecondary,
                        disabledContentColor = c.textDisabled,
                    ),
                )
                EndAlignedScroller(key = path, modifier = Modifier.weight(1f)) {
                    RepoExplorer.breadcrumbs(path).forEachIndexed { index, crumb ->
                        if (index > 0) FmIcon("chevron_right", size = 16.dp, tint = c.textDisabled)
                        Crumb(crumb.label, crumb.current) { onPath(crumb.target) }
                    }
                }
            }
        },
        actions = {
            FmToneButton("Go to file", onPressed = onGoToFile, icon = "search", tone = FmTone.Neutral)
        },
    )
}

@Composable
private fun Crumb(label: String, current: Boolean, onTap: () -> Unit) {
    val c = Fm.colors
    Text(
        label,
        modifier = Modifier
            .clip(RoundedCornerShape(FmRadius.sm))
            .clickable(onClick = onTap)
            .padding(horizontal = FmSpace.x1, vertical = FmSpace.x2),
        maxLines = 1,
        overflow = TextOverflow.Ellipsis,
        style = Fm.text.bodyMedium.copy(
            color = if (current) c.textPrimary else c.textSecondary,
            fontSize = FmType.small,
            fontWeight = if (current) FmType.emphasis else FmType.regular,
        ),
    )
}

@Composable
private fun EndAlignedScroller(key: String, modifier: Modifier, content: @Composable RowScope.() -> Unit) {
    val scroll = rememberScrollState()
    LaunchedEffect(key, scroll.maxValue) {
        if (scroll.maxValue > 0) scroll.scrollTo(scroll.maxValue)
    }
    Row(modifier.horizontalScroll(scroll), verticalAlignment = Alignment.CenterVertically, content = content)
}

internal suspend fun FmMessenger.showGoToFileSheet(repo: Repository, ref: String): String? =
    showModalBottomSheet<String>(isScrollControlled = true, containerColor = Color.Transparent) { close ->
        GoToFileSheet(repo, ref, close)
    }

@Composable
private fun ColumnScope.GoToFileSheet(repo: Repository, ref: String, close: (String?) -> Unit) {
    val c = Fm.colors
    var text by remember { mutableStateOf("") }
    var lastQuery by remember { mutableStateOf("") }
    val results = remember { AsyncValue<List<RepoCodeSearchMatch>>() }
    results.Bind()
    fun search() {
        val query = RepoExplorer.searchQuery(text) ?: return
        lastQuery = query
        results.reload { repoApi.searchRepo(repo.owner, repo.name, query, ref = ref).code }
    }
    val maxHeight = with(LocalDensity.current) { (LocalWindowInfo.current.containerSize.height * .78f).toDp() }
    val shape = RoundedCornerShape(topStart = FmSheet.corner, topEnd = FmSheet.corner)
    Column(
        Modifier
            .fillMaxWidth()
            .heightIn(max = maxHeight)
            .background(c.bgRaised, shape)
            .drawBehind { drawLine(c.border, Offset(0f, 0f), Offset(size.width, 0f), 1.dp.toPx()) },
    ) {
        Column(Modifier.padding(FmSpace.x4)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text("Go to file", Modifier.weight(1f), style = Fm.text.titleLarge)
                FmIconButton("close", "Close", onPressed = { close(null) }, iconSize = 24.dp)
            }
            Spacer(Modifier.height(FmSpace.x3))
            Row(verticalAlignment = Alignment.CenterVertically) {
                Box(Modifier.weight(1f)) {
                    FmTextField(
                        value = text,
                        onValueChange = { text = it },
                        hint = "Search file names or code...",
                        autofocus = true,
                        leading = { FmIcon("search") },
                        keyboardOptions = KeyboardOptions(imeAction = ImeAction.Search),
                        keyboardActions = KeyboardActions(onSearch = { search() }),
                    )
                }
                Spacer(Modifier.width(FmSpace.x2))
                FmFilledButton("Search", onClick = { search() })
            }
            Spacer(Modifier.height(FmSpace.x2))
            Text(
                "Searching ${repo.fullName} on $ref.",
                style = Fm.text.bodySmall.copy(color = c.textTertiary),
            )
        }
        Box(Modifier.weight(1f, fill = false)) {
            val matches = results.data
            val error = results.error
            when {
                !results.started -> FmEmptyState(
                    icon = "search",
                    title = "Search this repository",
                    message = "Type at least two characters to find files quickly.",
                )
                results.waiting -> FmSkeletonList(rows = 5, label = "Searching files")
                error != null -> ErrorCard(message = "Could not search files: $error", onRetry = { search() })
                matches.isNullOrEmpty() -> FmEmptyState(
                    icon = "search_off",
                    title = "No file matches",
                    message = "No code results for \"$lastQuery\".",
                )
                else -> LazyColumn(contentPadding = PaddingValues(start = FmSpace.x2, end = FmSpace.x2, bottom = FmSpace.x5)) {
                    itemsIndexed(matches) { index, match ->
                        Column {
                            if (index > 0) FmRowDivider(indent = 54.dp)
                            SearchFileRow(match) { close(match.path) }
                        }
                    }
                }
            }
        }
    }
}

@Composable
private fun SearchFileRow(match: RepoCodeSearchMatch, onTap: () -> Unit) {
    val c = Fm.colors
    FmListRow(
        title = match.path,
        titleContent = {
            Text(
                match.path,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
                style = Fm.text.labelMedium.copy(color = c.textPrimary),
            )
        },
        subtitle = RepoExplorer.searchRowSubtitle(match),
        leading = { FmIconTile(RepoExplorer.fileGlyph(RepoExplorer.fileName(match.path)), size = 34.dp, iconSize = 17.dp) },
        padding = PaddingValues(horizontal = FmSpace.x2, vertical = FmSpace.x2),
        minHeight = 52.dp,
        onTap = onTap,
    )
}
