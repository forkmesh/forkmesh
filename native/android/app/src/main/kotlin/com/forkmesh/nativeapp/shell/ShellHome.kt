package com.forkmesh.nativeapp.shell

import com.forkmesh.nativeapp.ui.kit.FmSafeInput
import com.forkmesh.nativeapp.ui.theme.FmRadius
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.ui.draw.clip
import androidx.compose.foundation.layout.size
import androidx.compose.ui.semantics.testTag
import androidx.compose.ui.focus.onFocusChanged
import androidx.compose.ui.focus.focusRequester
import com.forkmesh.nativeapp.ui.kit.fmGlass
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.layout.boundsInRoot
import androidx.compose.ui.layout.onGloballyPositioned
import androidx.compose.ui.input.nestedscroll.nestedScroll
import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.grid.GridCells
import androidx.compose.foundation.lazy.grid.GridItemSpan
import androidx.compose.foundation.lazy.grid.LazyVerticalGrid
import androidx.compose.foundation.lazy.grid.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.foundation.layout.offset
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.geometry.isSpecified
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.DpOffset
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.account.AuthService
import com.forkmesh.nativeapp.feature.account.settings.SettingsArea
import com.forkmesh.nativeapp.feature.code.CodeFeature
import com.forkmesh.nativeapp.feature.insights.StatusPageFeature
import com.forkmesh.nativeapp.feature.market.PackageGateFeature
import com.forkmesh.nativeapp.feature.navigation.NavigationFeature
import com.forkmesh.nativeapp.feature.navigation.ShellHomeMetrics
import com.forkmesh.nativeapp.feature.navigation.ShellHomeTile
import com.forkmesh.nativeapp.feature.navigation.ShellMenuBadge
import com.forkmesh.nativeapp.feature.navigation.ShellMenuEntry
import com.forkmesh.nativeapp.feature.navigation.rememberDesktopOffline
import com.forkmesh.nativeapp.feature.navigation.rememberHubItemRunner
import com.forkmesh.nativeapp.market.PackageGate
import com.forkmesh.nativeapp.navigation.HubCatalog
import com.forkmesh.nativeapp.navigation.Route
import com.forkmesh.nativeapp.ui.kit.FmActionMenu
import com.forkmesh.nativeapp.ui.kit.FmActionMenuButton
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenu
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenuItem
import com.forkmesh.nativeapp.ui.kit.FmInlineStatus
import com.forkmesh.nativeapp.ui.kit.FmModalBottomSheet
import com.forkmesh.nativeapp.ui.kit.rememberFmStatus
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmHaptics
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmSpace
import kotlinx.coroutines.CoroutineScope

private class HomeTileModel(
    val key: String,
    val id: String,
    val title: String,
    val icon: String,
    val badge: Int,
    val selected: Boolean,
    val running: Boolean,
    val desktopOffline: Boolean,
    val kind: Route.Kind? = null,
    val onClick: () -> Unit,
)

internal class ShellRepoGroup(val title: String, val entries: List<ShellMenuEntry>)

private fun homeGroupIcon(id: String): String = when (id) {
    "main" -> "arrow_circle_up"
    "repo" -> "folder"
    "workspace" -> "work"
    "operate" -> "settings_suggest"
    "business" -> "trending_up"
    "personal" -> "favorite"
    "insights" -> "lightbulb"
    "app" -> "apps"
    else -> "grid_view"
}

@OptIn(ExperimentalFoundationApi::class)
@Composable
internal fun ShellHomeGrid(
    main: List<ShellMenuEntry>,
    hubBadges: Map<Route.Kind, ShellMenuBadge>,
    scope: CoroutineScope,
    contentPadding: PaddingValues,
    modifier: Modifier = Modifier,
    repoGroup: ShellRepoGroup? = null,
    barCells: List<ShellBarCell> = emptyList(),
) {
    ShellBarStore.load()
    val hideDocked by SettingsArea.homeHidesBarIcons.collectAsState()
    val barLayout by ShellBarStore.layout.collectAsState()
    val barStyle by ShellBarStore.style.collectAsState()
    var tileMenu by remember { mutableStateOf<String?>(null) }
    val auth = Services.get<AuthService>()
    val guest = auth?.isGuest?.collectAsState()?.value == true
    val session = auth?.session?.collectAsState()?.value
    val surfaces by PackageGateFeature.surfaces.collectAsState()
    val style by ShellBarStore.home.collectAsState()
    val uses by ShellBarStore.uses.collectAsState()
    val recent by ShellBarStore.recent.collectAsState()
    val groups = HubCatalog.pages(HubCatalog.groups(guest, session?.isAdmin == true, surfaces.hubItems, NavigationFeature::isRegistered))
    val status = rememberFmStatus()
    val runHubItem = rememberHubItemRunner(scope, status)
    val desktopOffline = rememberDesktopOffline()
    val packagesVersion by StatusPageFeature.version.collectAsState()
    val packages = StatusPageFeature.store
    val haptics = FmHaptics.current
    var settings by remember { mutableStateOf(false) }
    var barSettings by remember { mutableStateOf(false) }
    var backgroundMenu by remember { mutableStateOf(false) }
    var pressAt by remember { mutableStateOf(Offset.Unspecified) }
    var hideMenu by remember { mutableStateOf<String?>(null) }
    val tiles = LinkedHashMap<String, HomeTileModel>()
    val sections = ArrayList<ShellHomeSection>()
    val mainTiles = main.map { entry ->
        HomeTileModel("main:${entry.id}", entry.id, entry.title, entry.icon, entry.badge, entry.selected, entry.running, desktopOffline && PackageGate.reliesOnDesktop(entry.id), onClick = entry.onClick)
    }
    sections += ShellHomeSection("main", "Go to", mainTiles.map { ShellHomeEntry(it.id, it.title, it.badge, active = it.running) })
    mainTiles.forEach { tiles.putIfAbsent(it.id, it) }
    if (repoGroup != null) {
        val models = repoGroup.entries.map { entry ->
            HomeTileModel("repo:${entry.id}", entry.id, entry.title, entry.icon, entry.badge, entry.selected, entry.running, false, onClick = entry.onClick)
        }
        models.forEach { tiles.putIfAbsent(it.id, it) }
        sections += ShellHomeSection("repo", repoGroup.title, models.map { ShellHomeEntry(it.id, it.title, it.badge, active = it.running) })
    }
    groups.forEach { group ->
        val models = group.items.map { item ->
            val badge = item.route?.let { hubBadges[it.kind] }
            HomeTileModel(
                "${group.id}:${item.id}",
                item.id,
                item.title,
                item.icon,
                badge?.count ?: 0,
                false,
                badge?.running == true,
                desktopOffline && PackageGate.reliesOnDesktop(item.id, item.route?.kind),
                item.route?.kind,
            ) { runHubItem(item) }
        }
        models.forEach { tiles.putIfAbsent(it.id, it) }
        sections += ShellHomeSection(group.id, group.title, models.map { ShellHomeEntry(it.id, it.title, it.badge, active = it.running) })
    }
    val menuOrder by ShellBarStore.menu.collectAsState()
    val placed = remember(sections.map { s -> s.id to s.entries.map { it.id } }, menuOrder) { ShellBarStore.arrangedSections(sections) }
    var query by remember { mutableStateOf("") }
    var searchShown by remember { mutableStateOf(false) }
    val needle = query.trim()
    val shownSections = if (needle.isEmpty()) placed else placed.mapNotNull { section ->
        val entries = if (section.title.contains(needle, ignoreCase = true)) section.entries else section.entries.filter { it.title.contains(needle, ignoreCase = true) }
        if (entries.isEmpty()) null else ShellHomeSection(section.id, section.title, entries)
    }
    val docked = if (hideDocked) remember(barCells, barLayout, barStyle) { ShellBarStore.barIds(barCells) } else emptySet()
    val arranged = style.copy(hideBarIcons = hideDocked).arranged(shownSections, uses, recent, docked, searching = needle.isNotEmpty())
    val homeBounds = remember { HashMap<String, Pair<String, androidx.compose.ui.geometry.Rect>>() }
    var lastTarget by remember { mutableStateOf<String?>(null) }
    val density = LocalDensity.current
    var pulled by remember { mutableStateOf(0f) }
    val pull = remember {
        object : androidx.compose.ui.input.nestedscroll.NestedScrollConnection {
            override fun onPostScroll(consumed: Offset, available: Offset, source: androidx.compose.ui.input.nestedscroll.NestedScrollSource): Offset {
                if (source != androidx.compose.ui.input.nestedscroll.NestedScrollSource.UserInput) return Offset.Zero
                pulled = if (available.y > 0f) pulled + available.y else 0f
                if (ShellBarOrder.revealsSearch(with(density) { pulled.toDp().value.toDouble() }, searchShown || query.isNotEmpty())) {
                    haptics.selection()
                    searchShown = true
                    pulled = 0f
                }
                return Offset.Zero
            }

            override suspend fun onPreFling(available: androidx.compose.ui.unit.Velocity): androidx.compose.ui.unit.Velocity {
                pulled = 0f
                return androidx.compose.ui.unit.Velocity.Zero
            }
        }
    }
    val ids = tiles.keys.toList()
    val most = ShellBarOrder.mostUses(uses, ids)
    BoxWithConstraints(modifier) {
        val rows = ShellHomeStyle.rows(arranged, style.across)
        val headerHeight = if (style.headed) 40.dp else if (style.grouped) FmSpace.x2 else 0.dp
        val bottom = contentPadding.calculateBottomPadding()
        val fitted = ShellBarOrder.fittedTile(
            height = maxHeight.value.toDouble(),
            fixed = (bottom + headerHeight * arranged.size + FmSpace.x3).value.toDouble(),
            rows = rows,
            tile = ShellHomeMetrics.tileHeight.value.toDouble(),
            minimum = 50.0,
        ).dp
        LazyVerticalGrid(
            columns = GridCells.Fixed(style.across),
            modifier = Modifier
                .fillMaxSize()
                .nestedScroll(pull)
                .semantics { contentDescription = "All pages" }
                .pointerInput(Unit) {
                    detectTapGestures(onLongPress = { at ->
                        haptics.palette()
                        pressAt = at
                        backgroundMenu = true
                    })
                },
            contentPadding = PaddingValues(
                start = contentPadding.calculateLeftPadding(androidx.compose.ui.unit.LayoutDirection.Ltr),
                end = contentPadding.calculateRightPadding(androidx.compose.ui.unit.LayoutDirection.Ltr),
                top = if (style.headed) 0.dp else FmSpace.x4,
                bottom = bottom,
            ),
        ) {
            if (status.message != null) {
                item(key = "menu-status", span = { GridItemSpan(maxLineSpan) }) { FmInlineStatus(status) }
            }
            if (searchShown || query.isNotEmpty()) {
                item(key = "home-search", span = { GridItemSpan(maxLineSpan) }) {
                    ShellHomeSearchField(
                        query,
                        onQuery = { query = it },
                        onSubmit = {
                            val first = arranged.firstOrNull()?.ids?.firstOrNull()?.let { tiles[it] }
                            if (first != null) {
                                haptics.selection()
                                ShellBarStore.use(first.id)
                                first.onClick()
                            } else if (needle.isNotEmpty()) {
                                NavigationFeature.open(Route.Search(needle, com.forkmesh.nativeapp.navigation.SearchScope.Everything))
                            }
                        },
                        onBlur = { if (query.isEmpty()) searchShown = false },
                        onHomeSettings = { settings = true },
                        onBarSettings = { barSettings = true },
                    )
                }
            }
            arranged.forEach { section ->
                if (style.headed) {
                    item(key = "section:${section.id}", span = { GridItemSpan(maxLineSpan) }) {
                        Box {
                            Row(
                                Modifier
                                    .fillMaxWidth()
                                    .combinedClickable(onClick = {}, onLongClick = {
                                        haptics.palette()
                                        hideMenu = section.id
                                    })
                                    .padding(start = FmSpace.x3, end = FmSpace.x3, top = FmSpace.x4, bottom = FmSpace.x2)
                                    .semantics { heading() },
                                verticalAlignment = Alignment.CenterVertically,
                            ) {
                                Text(section.title.uppercase(), style = Fm.text.labelSmall.copy(fontSize = 12.sp, letterSpacing = 1.2.sp), color = Fm.colors.textTertiary)
                                Spacer(Modifier.width(FmSpace.x3))
                                Box(Modifier.weight(1f).height(1.dp).background(Fm.colors.border))
                            }
                            FmDropdownMenu(expanded = hideMenu == section.id, onDismissRequest = { hideMenu = null }, offset = DpOffset(FmSpace.x3, 0.dp)) {
                                FmDropdownMenuItem("Hide ${section.title}", onClick = {
                                    hideMenu = null
                                    ShellBarStore.editHome { setShown(section.id, false) }
                                }, icon = "visibility_off")
                                FmDropdownMenuItem("Home screen settings…", onClick = {
                                    hideMenu = null
                                    settings = true
                                }, icon = "tune")
                            }
                        }
                    }
                } else if (style.grouped && section !== arranged.first()) {
                    item(key = "gap:${section.id}", span = { GridItemSpan(maxLineSpan) }) { Spacer(Modifier.height(FmSpace.x2)) }
                }
                items(section.ids.mapNotNull { tiles[it] }, key = { "${section.id}:${it.key}" }) { tile ->
                    val barCell = barCells.firstOrNull { it.id == tile.id }
                    Box(
                        Modifier
                            .animateItem()
                            .onGloballyPositioned { homeBounds[tile.id] = section.id to it.boundsInRoot() }
                            .graphicsLayer { alpha = if (ShellDrag.isDragging(tile.id, bar = false)) 0.35f else 1f }
                            .shellDragSource(
                                tile.id, tile.icon, fromBar = false, enabled = needle.isEmpty(),
                                onStart = {
                                    tileMenu = null
                                    backgroundMenu = false
                                    lastTarget = null
                                    haptics.selection()
                                },
                                onMove = { at ->
                                    if (ShellDrag.overBar || !style.reorders) return@shellDragSource
                                    val hit = homeBounds.entries.firstOrNull { it.key != tile.id && it.value.second.contains(at) } ?: return@shellDragSource
                                    if (hit.key == lastTarget) return@shellDragSource
                                    lastTarget = hit.key
                                    if (ShellBarStore.moveInMenu(tile.id, hit.value.first, hit.key, placed)) {
                                        if (style.sort != ShellHomeSort.custom) ShellBarStore.editHome { sort = ShellHomeSort.custom }
                                        haptics.selection()
                                    }
                                },
                                onDrop = { at ->
                                    if (ShellDrag.overBar) {
                                        if (ShellBarStore.putInBar(tile.id, ShellDrag.barSlotAt(at), barCells)) haptics.success() else haptics.warning()
                                    }
                                },
                            ),
                    ) {
                    ShellHomeTile(
                        tile.icon,
                        tile.title,
                        tile.badge,
                        tile.selected,
                        tile.running,
                        tile.desktopOffline,
                        iconStyle = style.icons,
                        heat = if (style.highlightFrequent) ShellBarOrder.heat(tile.id, uses, most) else 0.0,
                        tileHeight = fitted,
                        updated = packagesVersion >= 0 && packages?.isUpdated(tile.id, tile.kind) == true,
                        onLongClick = {
                            haptics.palette()
                            if (barCell != null) {
                                tileMenu = tile.key
                            } else {
                                pressAt = Offset.Unspecified
                                backgroundMenu = true
                            }
                        },
                    ) {
                        ShellBarStore.use(tile.id)
                        packages?.markViewed(tile.id, tile.kind)
                        tile.onClick()
                    }
                    if (barCell != null) {
                        FmDropdownMenu(expanded = tileMenu == tile.key, onDismissRequest = { tileMenu = null }) {
                            ShellBarItemMenu(barCell, barCells, onSettings = { barSettings = true }, inBarMenu = false) { tileMenu = null }
                            FmDropdownMenuItem("Home settings…", onClick = {
                                tileMenu = null
                                settings = true
                            }, icon = "grid_view")
                        }
                    }
                    }
                }
            if (needle.isNotEmpty()) {
                item(key = "home-search-everywhere", span = { GridItemSpan(maxLineSpan) }) { ShellHomeSearchEverywhere(needle) }
            }
            }
        }
        val menuWidth = 260.dp
        val anchorX = if (pressAt.isSpecified) with(density) { pressAt.x.toDp() } else maxWidth / 2
        val anchorY = if (pressAt.isSpecified) with(density) { pressAt.y.toDp() } else maxHeight / 2
        val left = (anchorX - menuWidth / 2).coerceIn(FmSpace.x3, (maxWidth - menuWidth - FmSpace.x3).coerceAtLeast(FmSpace.x3))
        Box(Modifier.align(Alignment.TopStart).offset(x = left, y = anchorY)) {
            FmActionMenu(expanded = backgroundMenu, onDismissRequest = { backgroundMenu = false }) {
                if (packages != null && packages.canShowUpdates) {
                    FmActionMenuButton("Show all update dots", onClick = {
                        backgroundMenu = false
                        packages.markAllUnviewed()
                    })
                }
                if (packages != null && packages.updated.isNotEmpty()) {
                    FmActionMenuButton("Clear all update dots", onClick = {
                        backgroundMenu = false
                        packages.markAllViewed()
                    })
                }
                FmActionMenuButton("Home settings…", onClick = {
                    backgroundMenu = false
                    settings = true
                })
                FmActionMenuButton("Bottom bar settings…", onClick = {
                    backgroundMenu = false
                    barSettings = true
                })
            }
        }
    }
    if (settings) ShellHomeSettingsSheet(onClose = { settings = false }, onBar = {
        settings = false
        barSettings = true
    })
    if (barSettings) ShellBarSettingsSheet(onClose = { barSettings = false })
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun ShellHomeSettingsSheet(onClose: () -> Unit, onBar: () -> Unit) {
    FmModalBottomSheet(onDismissRequest = onClose, isScrollControlled = true, showDragHandle = true) {
        ShellHomeSettingsForm(onDone = onClose, onBar = onBar)
    }
}

@Composable
fun ShellHomeSettingsForm(onDone: (() -> Unit)? = null, onBar: () -> Unit) {
    ShellBarStore.load()
    val c = Fm.colors
    val style by ShellBarStore.home.collectAsState()
    val uses by ShellBarStore.uses.collectAsState()
    val recent by ShellBarStore.recent.collectAsState()
    val hideBarIcons by SettingsArea.homeHidesBarIcons.collectAsState()
    val haptics = FmHaptics.current
    val auth = Services.get<AuthService>()
    val guest = auth?.isGuest?.collectAsState()?.value == true
    val session = auth?.session?.collectAsState()?.value
    val surfaces by PackageGateFeature.surfaces.collectAsState()
    val groups = HubCatalog.pages(HubCatalog.groups(guest, session?.isAdmin == true, surfaces.hubItems, NavigationFeature::isRegistered))
    val currentRepo by CodeFeature.currentRepo.collectAsState()
    val repoCategory = currentRepo?.takeIf { surfaces.code }?.let { Triple("repo", it.name, ShellBadges.repoSections.size) }
    val categories = listOf(Triple("main", "Go to", 0)) + listOfNotNull(repoCategory) + groups.map { Triple(it.id, it.title, it.items.size) } +
        style.hidden.filter { id -> id != "main" && (id != "repo" || repoCategory == null) && groups.none { it.id == id } }.map { Triple(it, it.replaceFirstChar(Char::uppercase), 0) }
    fun edit(change: ShellHomeStyle.() -> Unit) {
        haptics.selection()
        ShellBarStore.editHome(change)
    }
    Column(Modifier.fillMaxWidth().verticalScroll(rememberScrollState()).padding(bottom = FmSpace.x6)) {
        Box(Modifier.fillMaxWidth().heightIn(min = 44.dp).padding(horizontal = FmSpace.x4), contentAlignment = Alignment.Center) {
            Text("Home screen", style = Fm.text.titleMedium)
            if (onDone != null) {
                Text("Done", Modifier.align(Alignment.CenterEnd).clickable(onClick = onDone).padding(FmSpace.x2), style = Fm.text.labelLarge.copy(color = c.accent))
            }
        }
        SettingsGroup("Look", footer = "Outline and Filled keep the icons plain, Accent draws them in your accent colour and Tiles sets each one on a rounded tile.") {
            Row(Modifier.fillMaxWidth().background(c.bgBase).padding(top = FmSpace.x4, bottom = FmSpace.x2)) {
                listOf(Triple("Agents", "auto_awesome", 3), Triple("Inbox", "inbox", 0), Triple("Repos", "library_books", 0), Triple("Notes", "description", 0)).forEachIndexed { index, sample ->
                    Box(Modifier.weight(1f)) {
                        ShellHomeTile(
                            sample.second,
                            sample.first,
                            sample.third,
                            false,
                            index == 0,
                            false,
                            iconStyle = style.icons,
                            heat = if (style.highlightFrequent && index == 1) 0.9 else 0.0,
                        ) {}
                    }
                }
            }
            Segmented(ShellHomeIconStyle.entries, style.icons, { it.title }) { edit { icons = it } }
            LabeledRow("Icons per row", "grid_view") {
                Segmented(ShellHomeStyle.ACROSS_CHOICES, style.across, { "$it" }, Modifier.width(160.dp)) { edit { across = it } }
            }
        }
        SettingsGroup(
            "Sort icons",
            footer = if (style.grouped) "Icons are sorted inside each category." else "Icons are sorted across all categories.",
        ) {
            ShellHomeSort.entries.forEach { sort ->
                PickRow(sort.title, sortIcon(sort), style.sort == sort) { edit { this.sort = sort } }
            }
        }
        SettingsGroup(
            "Categories",
            footer = "A hidden category leaves Home, but its icons stay in the bottom bar. Touch and hold a category title on Home to hide it from there.",
        ) {
            ToggleRow("Group by category", "view_agenda", style.grouped) { value -> edit { grouped = value } }
            ToggleRow("Show category titles", "title", style.showTitles) { value -> edit { showTitles = value } }
            LabeledRow("Category order", "swap_vert") {
                var open by remember { mutableStateOf(false) }
                Box {
                    Text(style.groupOrder.title, Modifier.clickable { open = true }.padding(FmSpace.x2), style = Fm.text.bodyLarge.copy(color = c.textSecondary))
                    FmDropdownMenu(expanded = open, onDismissRequest = { open = false }) {
                        ShellHomeGroupOrder.entries.forEach { order ->
                            FmDropdownMenuItem(order.title, onClick = {
                                open = false
                                edit { groupOrder = order }
                            }, icon = if (order == style.groupOrder) "check" else null)
                        }
                    }
                }
            }
            categories.forEach { (id, title, count) ->
                val shown = style.shows(id)
                Row(Modifier.fillMaxWidth().heightIn(min = 44.dp), verticalAlignment = Alignment.CenterVertically) {
                    FmIcon(homeGroupIcon(id), size = 20.dp, tint = if (shown) c.accent else c.textTertiary, outlined = true)
                    Spacer(Modifier.width(FmSpace.x3))
                    Text(title, Modifier.weight(1f), style = Fm.text.bodyLarge.copy(color = if (shown) c.textPrimary else c.textTertiary))
                    if (count > 0) Text("$count", Modifier.padding(end = FmSpace.x2), style = Fm.text.bodySmall.copy(color = c.textTertiary, fontFeatureSettings = "tnum"))
                    com.forkmesh.nativeapp.ui.kit.FmSwitch(checked = shown, onCheckedChange = { on -> edit { setShown(id, on) } })
                }
            }
        }
        SettingsGroup("Highlights", footer = "Frequently used icons get a soft glow that grows with use. Unread badges always show, even with counts off.") {
            ToggleRow("Highlight frequently used", "auto_awesome", style.highlightFrequent) { value -> edit { highlightFrequent = value } }
            ToggleRow("Show counts", "tag", style.showCounts) { value -> edit { showCounts = value } }
        }
        SettingsGroup(footer = "Icons already in the bottom bar leave Home so each one shows once. Bottom bar sets its shape, glass, icon style and size, labels and how the selected icon is marked.") {
            ToggleRow("Hide icons in the bottom bar", "visibility_off", hideBarIcons) { value ->
                haptics.selection()
                SettingsArea.setHomeHidesBarIcons(value)
            }
            ActionRow("Bottom bar", "tune", onClick = onBar)
        }
        SettingsGroup {
            ActionRow("Clear usage history", "history", enabled = uses.isNotEmpty() || recent.isNotEmpty()) {
                haptics.selection()
                ShellBarStore.clearUsage()
            }
            ActionRow("Reset Home settings", "restart_alt", enabled = !style.isDefault, destructive = true) {
                haptics.selection()
                ShellBarStore.resetHome()
            }
        }
    }
}

private fun sortIcon(sort: ShellHomeSort): String = when (sort) {
    ShellHomeSort.custom -> "pan_tool"
    ShellHomeSort.byName -> "sort_by_alpha"
    ShellHomeSort.nameDescending -> "sort_by_alpha"
    ShellHomeSort.mostUsed -> "bar_chart"
    ShellHomeSort.leastUsed -> "bar_chart"
    ShellHomeSort.recent -> "history"
    ShellHomeSort.attention -> "notifications_active"
    ShellHomeSort.count -> "tag"
}

@Composable
private fun PickRow(title: String, icon: String, selected: Boolean, onPick: () -> Unit) {
    val c = Fm.colors
    Row(
        Modifier.fillMaxWidth().heightIn(min = 44.dp).clickable(onClick = onPick),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        FmIcon(icon, size = 20.dp, tint = c.accent, outlined = true)
        Spacer(Modifier.width(FmSpace.x3))
        Text(title, Modifier.weight(1f), style = Fm.text.bodyLarge)
        if (selected) FmIcon("check", size = 20.dp, tint = c.accent)
    }
}

@Composable
internal fun rememberShellBarCandidates(
    base: List<ShellBarCell>,
    main: List<ShellMenuEntry>,
    repo: ShellRepoGroup?,
    hubBadges: Map<Route.Kind, ShellMenuBadge>,
    scope: CoroutineScope,
): List<ShellBarCell> {
    val auth = Services.get<AuthService>()
    val guest = auth?.isGuest?.collectAsState()?.value == true
    val session = auth?.session?.collectAsState()?.value
    val surfaces by PackageGateFeature.surfaces.collectAsState()
    val groups = HubCatalog.pages(HubCatalog.groups(guest, session?.isAdmin == true, surfaces.hubItems, NavigationFeature::isRegistered))
    val status = rememberFmStatus()
    val run = rememberHubItemRunner(scope, status)
    val activity = rememberShellHubActivity(hubBadges)
    val out = LinkedHashMap<String, ShellBarCell>()
    base.forEach { out[it.id] = it }
    (main + repo?.entries.orEmpty()).forEach { entry ->
        out.putIfAbsent(entry.id, ShellBarCell(entry.id, entry.title, entry.icon, entry.badge, entry.selected, entry.running, onClick = entry.onClick))
    }
    groups.forEach { group ->
        group.items.forEach { item ->
            val badge = item.route?.let { hubBadges[it.kind] }
            val kind = item.route?.kind
            val progress = kind?.let { activity.progress[it] }
            out.putIfAbsent(
                item.id,
                ShellBarCell(
                    item.id, item.title, item.icon, badge?.count ?: 0, false, badge?.running == true,
                    description = if (progress != null) "${item.title}, ${progress.spoken(System.currentTimeMillis())}" else item.title,
                    active = kind != null && kind in activity.active,
                    progress = progress,
                    onClick = { run(item) },
                ),
            )
        }
    }
    return out.values.toList()
}


@Composable
private fun ShellHomeSearchField(
    query: String,
    onQuery: (String) -> Unit,
    onSubmit: () -> Unit,
    onBlur: () -> Unit,
    onHomeSettings: () -> Unit,
    onBarSettings: () -> Unit,
) {
    val c = Fm.colors
    val focus = remember { androidx.compose.ui.focus.FocusRequester() }
    var focused by remember { mutableStateOf(false) }
    var menu by remember { mutableStateOf(false) }
    LaunchedEffect(Unit) { runCatching { focus.requestFocus() } }
    Column {
        Row(
            Modifier
                .padding(horizontal = FmSpace.x2, vertical = FmSpace.x1)
                .fillMaxWidth()
                .height(36.dp)
                .fmGlass(androidx.compose.foundation.shape.CircleShape, shadow = false)
                .padding(start = FmSpace.x3),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            FmIcon("search", size = 16.dp, tint = c.textTertiary)
            Spacer(Modifier.width(FmSpace.x2))
            Box(Modifier.weight(1f), contentAlignment = Alignment.CenterStart) {
                if (query.isEmpty()) Text("Search", style = Fm.text.bodySmall, color = c.textTertiary)
                androidx.compose.foundation.text.BasicTextField(
                    query,
                    FmSafeInput.text(onQuery),
                    Modifier
                        .fillMaxWidth()
                        .focusRequester(focus)
                        .onFocusChanged { state ->
                            if (focused && !state.isFocused) onBlur()
                            focused = state.isFocused
                        }
                        .semantics {
                            contentDescription = "Search"
                            testTag = "shell-home-search"
                        },
                    singleLine = true,
                    textStyle = Fm.text.bodySmall.copy(color = c.textPrimary),
                    cursorBrush = androidx.compose.ui.graphics.SolidColor(c.accent),
                    keyboardOptions = androidx.compose.foundation.text.KeyboardOptions(
                        autoCorrectEnabled = false,
                        imeAction = androidx.compose.ui.text.input.ImeAction.Search,
                    ),
                    keyboardActions = androidx.compose.foundation.text.KeyboardActions(onSearch = { onSubmit() }),
                )
            }
            Box {
                Box(
                    Modifier
                        .size(40.dp, 36.dp)
                        .clickable {
                            if (query.isEmpty()) menu = true else onQuery("")
                        }
                        .semantics { contentDescription = if (query.isEmpty()) "Icon settings" else "Clear search" },
                    contentAlignment = Alignment.Center,
                ) {
                    FmIcon(if (query.isEmpty()) "tune" else "cancel", size = 16.dp, tint = c.textTertiary)
                }
                FmDropdownMenu(expanded = menu, onDismissRequest = { menu = false }) {
                    FmDropdownMenuItem("Home settings…", onClick = {
                        menu = false
                        onHomeSettings()
                    }, icon = "grid_view")
                    FmDropdownMenuItem("Bottom bar settings…", onClick = {
                        menu = false
                        onBarSettings()
                    }, icon = "tune")
                }
            }
        }
    }
}

@Composable
private fun ShellHomeSearchEverywhere(query: String) {
    val c = Fm.colors
    Row(
        Modifier
            .padding(horizontal = FmSpace.x2, vertical = FmSpace.x1)
            .fillMaxWidth()
            .height(44.dp)
            .clip(RoundedCornerShape(FmRadius.md))
            .background(c.bgOverlay)
            .clickable {
                NavigationFeature.open(Route.Search(query.trim(), com.forkmesh.nativeapp.navigation.SearchScope.Everything))
            }
            .padding(horizontal = FmSpace.x3)
            .semantics { testTag = "shell-home-search-everywhere" },
        verticalAlignment = Alignment.CenterVertically,
    ) {
        FmIcon("search", size = 16.dp, tint = c.accent)
        Spacer(Modifier.width(FmSpace.x2))
        Text("Search everywhere for \u201C${query.trim()}\u201D", style = Fm.text.bodySmall, color = c.accent, maxLines = 1, overflow = androidx.compose.ui.text.style.TextOverflow.Ellipsis)
    }
}
