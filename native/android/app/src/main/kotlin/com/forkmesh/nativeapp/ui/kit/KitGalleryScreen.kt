package com.forkmesh.nativeapp.ui.kit

import android.app.Activity
import android.content.pm.ApplicationInfo
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.asPaddingValues
import androidx.compose.foundation.layout.consumeWindowInsets
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.navigationBars
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawing
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.LazyListScope
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Button
import androidx.compose.material3.Checkbox
import androidx.compose.material3.ListItem
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.RadioButton
import androidx.compose.material3.Switch
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
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.forkmesh.nativeapp.feature.chat.RelayConnection
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmRadius
import com.forkmesh.nativeapp.ui.theme.FmSpace
import com.forkmesh.nativeapp.ui.theme.ForkMeshTheme
import kotlinx.coroutines.launch

class KitGalleryScreen(
    initialDark: Boolean = false,
    private val initialSection: Int = 0,
    private val openOnStart: String? = null,
) : Screen() {
    private var dark by mutableStateOf(initialDark)

    override val title: String get() = "UI kit"

    @Composable
    override fun Content(navigator: Navigator) {
        ForkMeshTheme(dark = dark) {
            val colors = Fm.colors
            Column(
                Modifier
                    .fillMaxSize()
                    .background(colors.bgBase),
            ) {
                Box(
                    Modifier
                        .background(colors.bgRaised)
                        .windowInsetsPadding(fmSafeAreaInsets(bottom = false)),
                ) {
                    FmPanelHeader(
                        title = "UI kit",
                        subtitle = "Shared Compose components",
                        trailing = {
                            FmIconButton(
                                icon = if (dark) "light_mode_outlined" else "dark_mode_outlined",
                                label = if (dark) "Light theme" else "Dark theme",
                                onPressed = { dark = !dark },
                            )
                        },
                    )
                }
                FmSegmentTabBar(
                    tabs = listOf(FmSegmentTab("Light"), FmSegmentTab("Dark")),
                    index = if (dark) 1 else 0,
                    onChanged = { dark = it == 1 },
                )
                Gallery(initialSection, openOnStart)
            }
        }
    }

    companion object {
        const val EXTRA = "fm_kit_gallery"
        const val EXTRA_THEME = "fm_kit_theme"
        const val EXTRA_SECTION = "fm_kit_section"
        const val EXTRA_OPEN = "fm_kit_open"

        fun fromIntent(activity: Activity): KitGalleryScreen? {
            val debuggable = (activity.applicationInfo.flags and ApplicationInfo.FLAG_DEBUGGABLE) != 0
            val intent = activity.intent ?: return null
            if (!debuggable || !intent.getBooleanExtra(EXTRA, false)) return null
            return KitGalleryScreen(
                initialDark = intent.getStringExtra(EXTRA_THEME) == "dark",
                initialSection = intent.getIntExtra(EXTRA_SECTION, 0),
                openOnStart = intent.getStringExtra(EXTRA_OPEN),
            )
        }
    }
}

@Composable
private fun ColumnScope.Gallery(initialSection: Int, openOnStart: String?) {
    val messenger = LocalFmMessenger.current
    val scope = rememberCoroutineScope()
    val cardStatus = rememberFmStatus()
    val overlayStatus = rememberFmStatus()
    val listState = rememberLazyListState(initialFirstVisibleItemIndex = initialSection)
    LaunchedEffect(openOnStart) {
        if (openOnStart != null) openDemo(messenger, overlayStatus, openOnStart)
    }
    LazyColumn(
        Modifier
            .weight(1f)
            .fillMaxWidth(),
        state = listState,
        contentPadding = WindowInsets.navigationBars.asPaddingValues(),
    ) {
        section("0 · Typography") { TypographyDemo() }
        section("1 · Palette") { PaletteDemo() }
        section("2 · Cards & headers") {
            CardsDemo(onCardTap = { cardStatus.show("FmCard tapped") })
            FmInlineStatus(cardStatus, Modifier.padding(horizontal = FmSpace.x4))
        }
        section("3 · Explorer") { ExplorerDemo() }
        section("4 · Tone & dialog buttons") { ButtonsDemo() }
        section("5 · Stock Material 3") { MaterialDemo() }
        section("6 · Fields") { FieldsDemo() }
        section("7 · Banners, notes, avatars") { BannersDemo() }
        section("8 · Loading & motion") { LoadingDemo() }
        section("9 · Inline status, overlays & bars") {
            Wrap {
                for (name in listOf("status", "error", "action", "confirm", "prompt", "alert", "sheet", "guest")) {
                    FmToneButton(name, onPressed = { scope.launch { openDemo(messenger, overlayStatus, name) } }, tone = FmTone.Accent)
                }
            }
            FmInlineStatus(overlayStatus, Modifier.padding(horizontal = FmSpace.x4))
            Spacer(Modifier.height(FmSpace.x3))
            Column(Modifier.consumeWindowInsets(WindowInsets.safeDrawing)) {
                FmBottomBar(
                    leading = { FmBranchLabel("main", onTap = {}, trailingIcon = "expand_more") },
                    actions = {
                        FmToneButton("Pull", onPressed = {}, icon = "download_rounded")
                        FmToneButton("Push", onPressed = {}, tone = FmTone.Accent, filled = true)
                    },
                )
                Spacer(Modifier.height(FmSpace.x3))
                FmBottomActionBar {
                    Button(onClick = {}, modifier = Modifier.fillMaxWidth()) { Text("Commit 3 files") }
                }
            }
        }
    }
}

private suspend fun openDemo(messenger: FmMessenger, status: FmStatus, name: String) {
    when (name) {
        "status" -> status.show("Cloned forkmesh/forkmesh to forkmesh-2.")
        "error" -> status.error("Could not reach forkmesh.com.")
        "action" -> status.show("Pull request opened: native ui kit", actionLabel = "View") { status.show("Action pressed") }
        "confirm" -> {
            val ok = messenger.showConfirmDialog(
                title = "Discard changes?",
                body = "Changes to src/main.kt are dropped and cannot be recovered.",
                confirmLabel = "Discard",
                destructive = true,
            )
            status.show("Confirmed: $ok")
        }
        "prompt" -> {
            val path = messenger.showPromptDialog(title = "New file", hint = "docs/notes.md", confirmLabel = "Create")
            status.show("Prompt: ${path ?: "cancelled"}")
        }
        "alert" -> messenger.showDialog<Unit> { done ->
            FmAlertDialog(
                onDismissRequest = { done(null) },
                title = "Delete jane-org?",
                actions = {
                    FmTextButton(onClick = { done(null) }) { Text("Cancel") }
                    FmTextButton(onClick = { done(Unit) }, contentColor = Fm.colors.danger) { Text("Delete") }
                },
            ) {
                Text(
                    "This dissolves the organization, its members, teams, and repo links. " +
                        "The linked repos themselves are not deleted. This cannot be undone.",
                )
            }
        }
        "sheet" -> {
            val picked = messenger.showModalBottomSheet<String>(showDragHandle = true) { close ->
                Column(Modifier.padding(bottom = FmSpace.x4)) {
                    Text(
                        "Tools",
                        style = Fm.text.titleLarge,
                        modifier = Modifier.padding(start = FmSpace.x4, end = FmSpace.x4, bottom = FmSpace.x2),
                    )
                    FmListRow("Activity", leading = { FmIconTile("bolt_outlined") }, onTap = { close("Activity") })
                    FmRowDivider()
                    FmListRow(
                        "World",
                        subtitle = "world.forkmesh.com",
                        leading = { FmIconTile("public_rounded", tint = Fm.colors.accent) },
                        onTap = { close("World") },
                    )
                }
            }
            status.show("Sheet: ${picked ?: "dismissed"}")
        }
        "guest" -> {
            val choice = messenger.showGuestSignInSheet("open an issue")
            status.show("Guest sheet: ${choice ?: "keep exploring"}")
        }
    }
}

private fun LazyListScope.section(title: String, content: @Composable ColumnScope.() -> Unit) {
    item(key = title) {
        Column(
            Modifier
                .fillMaxWidth()
                .padding(bottom = FmSpace.x3),
        ) {
            FmGroupHeader(
                label = title,
                color = Fm.colors.accent,
                padding = PaddingValues(start = FmSpace.x4, top = FmSpace.x5, end = FmSpace.x3, bottom = FmSpace.x2),
            )
            content()
        }
    }
}

@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun Wrap(content: @Composable () -> Unit) {
    FlowRow(
        Modifier
            .fillMaxWidth()
            .padding(horizontal = FmSpace.x4, vertical = FmSpace.x1),
        horizontalArrangement = Arrangement.spacedBy(FmSpace.x2),
        verticalArrangement = Arrangement.spacedBy(FmSpace.x2),
        itemVerticalAlignment = Alignment.CenterVertically,
    ) {
        content()
    }
}

@Composable
private fun TypographyDemo() {
    val t = Fm.text
    Column(Modifier.padding(horizontal = FmSpace.x4), verticalArrangement = Arrangement.spacedBy(FmSpace.x1)) {
        listOf(
            "Hero 40" to t.displayLarge,
            "displaySmall 28" to t.displaySmall,
            "titleLarge 20" to t.titleLarge,
            "titleMedium 17" to t.titleMedium,
            "titleSmall 15" to t.titleSmall,
            "bodyLarge 15 relaxed" to t.bodyLarge,
            "bodyMedium 13 snug" to t.bodyMedium,
            "bodySmall 12 muted" to t.bodySmall,
            "labelLarge 15" to t.labelLarge,
            "labelMedium 13" to t.labelMedium,
            "LABELSMALL 11 TERTIARY" to t.labelSmall,
        ).forEach { (name, style) ->
            Text(name, style = style, maxLines = 1, overflow = TextOverflow.Ellipsis)
        }
        Text("mono 12  a1b2c3d4 src/main.kt", style = Fm.mono())
    }
}

@Composable
private fun PaletteDemo() {
    val c = Fm.colors
    val swatches = listOf(
        "bgBase" to c.bgBase, "bgRaised" to c.bgRaised, "bgOverlay" to c.bgOverlay, "bgElevated" to c.bgElevated,
        "primary" to c.textPrimary, "secondary" to c.textSecondary, "tertiary" to c.textTertiary,
        "disabled" to c.textDisabled, "accent" to c.accent, "accentSubtle" to c.accentSubtle,
        "success" to c.success, "warning" to c.warning, "danger" to c.danger, "offline" to c.offline,
        "border" to c.border, "underline" to c.activeUnderline,
    )
    Column(Modifier.padding(horizontal = FmSpace.x4), verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
        swatches.chunked(4).forEach { row ->
            Row(horizontalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                row.forEach { (name, color) ->
                    Column(Modifier.weight(1f)) {
                        Box(
                            Modifier
                                .fillMaxWidth()
                                .height(28.dp)
                                .clip(RoundedCornerShape(FmRadius.sm))
                                .background(color)
                                .border(1.dp, c.border, RoundedCornerShape(FmRadius.sm)),
                        )
                        Text(
                            name,
                            style = Fm.text.bodySmall.copy(fontSize = 10.sp),
                            maxLines = 1,
                            overflow = TextOverflow.Ellipsis,
                        )
                    }
                }
            }
        }
    }
}

@Composable
private fun CardsDemo(onCardTap: () -> Unit) {
    val c = Fm.colors
    FmSectionHeader("Repositories", Modifier.padding(horizontal = FmSpace.x4), icon = "folder_outlined", count = 12)
    Spacer(Modifier.height(FmSpace.x2))
    FmCard(margin = PaddingValues(horizontal = FmSpace.x4)) {
        Column {
            Text("FmCard", style = Fm.text.titleSmall)
            Text("Raised, borderless, md corners.", style = Fm.text.bodySmall)
        }
    }
    Spacer(Modifier.height(FmSpace.x2))
    FmCard(margin = PaddingValues(horizontal = FmSpace.x4), onTap = onCardTap, color = c.bgOverlay) {
        Text("Tappable FmCard on bgOverlay", style = Fm.text.bodyMedium)
    }
    Wrap {
        FmStatusBadge("public", c.success)
        FmStatusBadge("draft", c.warning)
        FmStatusBadge("closed", c.danger)
        FmStatusBadge("mirror", c.accent)
    }
    Box(Modifier.background(c.bgRaised)) {
        FmPanelHeader(
            title = "forkmesh/forkmesh",
            subtitle = "main · 3 contributors",
            leading = { FmIconTile("folder_outlined", tint = c.accent) },
            trailing = { FmIconButton("more_vert", "More", onPressed = {}) },
        )
    }
    FmEmptyState(icon = "inbox_outlined", title = "No pull requests", message = "Branches you push show up here.")
}

@Composable
private fun ExplorerDemo() {
    val c = Fm.colors
    var tab by remember { mutableIntStateOf(0) }
    FmSegmentTabBar(
        tabs = listOf(FmSegmentTab("Changes", 6), FmSegmentTab("History", 50), FmSegmentTab("Branches")),
        index = tab,
        onChanged = { tab = it },
    )
    Column(Modifier.background(c.listSurface)) {
        FmGroupHeader("Staged", count = 2, color = c.success, actions = {
            FmToneButton("Unstage all", onPressed = {}, height = 28.dp)
        })
        FmListRow(
            "src",
            subtitle = "3 files · 2d ago",
            leading = { FmIconTile("folder_outlined", tint = c.accent) },
            onTap = {},
        )
        FmRowDivider()
        FmListRow(
            "README.md",
            subtitle = "2.0 KB · 3d ago",
            leading = { FmIconTile("description_outlined") },
            trailing = { FmChangeBadge(FmChangeKind.Modified) },
            onTap = {},
            onLongPress = {},
        )
        FmRowDivider()
        FmListRow(
            "feature/native-kit",
            subtitle = "Ahead 3 · behind 1",
            titleColor = c.success,
            leading = { FmIconTile("call_split_rounded", tint = c.success) },
            trailing = { FmCountPill(3, active = true) },
            showChevron = false,
        )
        FmGroupHeader("Changes", count = 4, actions = {
            FmIconButton("refresh", "Refresh", onPressed = {})
        })
        Row(
            Modifier.padding(start = FmSpace.x4, end = FmSpace.x4, bottom = FmSpace.x3),
            horizontalArrangement = Arrangement.spacedBy(FmSpace.x2),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            FmChangeKind.entries.forEach { FmChangeBadge(it) }
            FmCountPill(50)
            FmCountPill(1, active = true)
        }
    }
}

@Composable
private fun ButtonsDemo() {
    val c = Fm.colors
    Wrap { FmTone.entries.forEach { FmToneButton(it.name, onPressed = {}, tone = it) } }
    Wrap { FmTone.entries.forEach { FmToneButton(it.name, onPressed = {}, tone = it, filled = true) } }
    Wrap {
        FmToneButton("Stage all", onPressed = {}, icon = "add", tone = FmTone.Success)
        FmToneButton("Disabled", onPressed = null, tone = FmTone.Danger)
        FmToneButton("Tooltip", onPressed = {}, tooltip = "Long-press shows this")
    }
    Wrap {
        FmIconButton("refresh", "Refresh", onPressed = {})
        FmIconButton("delete_outline", "Delete", onPressed = {}, color = c.danger)
        FmIconButton("more_vert", "Disabled", onPressed = null)
        FmBranchLabel("main")
        FmBranchLabel("feature/kit", onTap = {}, trailingIcon = "expand_more")
    }
    Column(Modifier.padding(horizontal = FmSpace.x4)) {
        FmDialogActions(onCancel = {}, onConfirm = {})
        FmDialogActions(onCancel = {}, onConfirm = {}, confirmLabel = "Delete", destructive = true)
        FmDialogActions(onCancel = {}, onConfirm = {}, busy = true)
    }
}

@Composable
private fun MaterialDemo() {
    Wrap {
        Button(onClick = {}) { Text("Filled") }
        OutlinedButton(onClick = {}) { Text("Outlined") }
        FmTextButton(onClick = {}) { Text("Text") }
        FmElevatedButton(onClick = {}) { Text("Elevated") }
    }
    var on by remember { mutableStateOf(true) }
    var checked by remember { mutableStateOf(true) }
    var menu by remember { mutableStateOf(false) }
    Wrap {
        Switch(checked = on, onCheckedChange = { on = it })
        Switch(checked = !on, onCheckedChange = { on = !it })
        Checkbox(checked = checked, onCheckedChange = { checked = it })
        RadioButton(selected = checked, onClick = { checked = !checked })
        FmCircularProgressIndicator(Modifier.size(28.dp), strokeWidth = 3.dp)
        Box {
            FmTextButton(onClick = { menu = true }) { Text("Menu") }
            FmDropdownMenu(expanded = menu, onDismissRequest = { menu = false }) {
                FmDropdownMenuItem("Copy clone URL", onClick = { menu = false }, icon = "content_copy")
                FmDropdownMenuItem("Delete", onClick = { menu = false }, icon = "delete_outline")
            }
        }
    }
    Column(Modifier.padding(horizontal = FmSpace.x4, vertical = FmSpace.x2), verticalArrangement = Arrangement.spacedBy(FmSpace.x3)) {
        FmLinearProgressIndicator()
        FmLinearProgressIndicator(progress = { .6f })
    }
    Wrap {
        FmFloatingActionButton(onClick = {}) { FmIcon("add") }
        FmExtendedFloatingActionButton("New issue", onClick = {}, icon = "add")
    }
    ListItem(
        headlineContent = { Text("Stock ListItem") },
        supportingContent = { Text("FmListTileDefaults.colors()") },
        leadingContent = { FmIcon("folder_outlined", size = 20.dp) },
        colors = FmListTileDefaults.colors(),
    )
}

@Composable
private fun FieldsDemo() {
    Column(Modifier.padding(horizontal = FmSpace.x4), verticalArrangement = Arrangement.spacedBy(FmSpace.x3)) {
        var name by remember { mutableStateOf("") }
        FmField("Repository name") { FmTextField(name, { name = it }, hint = "my-project") }
        var description by remember { mutableStateOf("Native Android port of the mobile app") }
        FmField("Description") {
            FmTextField(description, { description = it }, helperText = "Shown on the repository page", maxLines = 3)
        }
        FmField("Owner") { FmTextField("jane", {}, errorText = "That name is taken") }
        FmField("Disabled") { FmTextField("read-only", {}, enabled = false) }
        var bare by remember { mutableStateOf("") }
        FmTextField(bare, { bare = it }, hint = "A lone field keeps a bare placeholder")
        Column {
            FmKeyValueRow("Owner", "jane")
            FmKeyValueRow("Head", "a1b2c3d4 · Merge main into pr/42 and resolve the conflicts")
        }
    }
}

@Composable
private fun BannersDemo() {
    FmConnectionBanner(RelayConnection.Offline, onRetry = {})
    GuestModeBanner(onSignIn = {})
    Column(Modifier.padding(FmSpace.x4), verticalArrangement = Arrangement.spacedBy(FmSpace.x3)) {
        GuestComposeNotice(action = "reply", onLogIn = {})
        FmNeedsDesktopNote()
        FmInboxPendingNote()
        Row(horizontalArrangement = Arrangement.spacedBy(FmSpace.x3), verticalAlignment = Alignment.CenterVertically) {
            AvatarWithDot(RelayConnection.Connected, "jane")
            AvatarWithDot(RelayConnection.Connecting, "sam")
            AvatarWithDot(RelayConnection.Offline, "")
            FmAvatar("forkbot", 28.dp, textStyle = Fm.text.labelMedium)
        }
        ComposeIdentityBar(name = "jane", verb = "Replying")
    }
}

@Composable
private fun LoadingDemo() {
    val c = Fm.colors
    Column(Modifier.padding(horizontal = FmSpace.x4), verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
        FmSkeleton(width = 180.dp)
        FmSkeleton(height = 13.dp)
        Text("Reduced motion: ${Fm.reducedMotion}", style = Fm.text.bodySmall)
    }
    Box(
        Modifier
            .padding(top = FmSpace.x2)
            .fillMaxWidth()
            .height(150.dp)
            .background(c.listSurface),
    ) {
        FmSkeletonList(rows = 5)
    }
    var loaded by remember { mutableStateOf(false) }
    Wrap {
        FmToneButton(if (loaded) "Swap to skeleton" else "Swap to empty state", onPressed = { loaded = !loaded }, tone = FmTone.Accent)
    }
    FmSwap(stateKey = loaded, modifier = Modifier.background(c.listSurface)) { showEmpty ->
        if (showEmpty) {
            FmEmptyState("inbox_outlined", "Nothing here", message = "FmSwap cross-faded this in.")
        } else {
            FmSkeletonList(rows = 2)
        }
    }
    var generation by remember { mutableIntStateOf(0) }
    Wrap { FmToneButton("Fade in again", onPressed = { generation++ }) }
    FmSliverFadeIn(stateKey = generation) {
        FmListRow(
            "FmSliverFadeIn",
            subtitle = "generation $generation",
            leading = { FmIconTile("bolt_outlined", tint = c.accent) },
            showChevron = false,
        )
    }
    FmEdgeFadeScroll(Modifier.padding(vertical = FmSpace.x2)) { state ->
        Row(
            Modifier
                .horizontalScroll(state)
                .padding(horizontal = FmSpace.x4),
            horizontalArrangement = Arrangement.spacedBy(FmSpace.x2),
        ) {
            listOf("All", "Mine", "Starred", "Forks", "Mirrors", "Archived", "Public", "Private").forEach {
                FmToneButton(it, onPressed = {}, tone = if (it == "All") FmTone.Accent else FmTone.Neutral, filled = it == "All")
            }
        }
    }
    Spacer(Modifier.height(FmSpace.x2))
}
