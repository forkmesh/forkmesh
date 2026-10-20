package com.forkmesh.nativeapp.shell

import androidx.compose.runtime.LaunchedEffect
import androidx.compose.ui.layout.boundsInRoot
import androidx.compose.ui.layout.onGloballyPositioned
import androidx.compose.foundation.ExperimentalFoundationApi
import com.forkmesh.nativeapp.ui.theme.FmType
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.interaction.collectIsPressedAsState
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Slider
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import com.forkmesh.nativeapp.ui.kit.LocalFmMessenger
import kotlinx.coroutines.launch
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.draw.shadow
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Shape
import androidx.compose.ui.graphics.compositeOver
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.layout.onSizeChanged
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.role
import androidx.compose.ui.semantics.selected
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.forkmesh.nativeapp.api.PrefStore
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.navigation.DesktopOfflineDot
import com.forkmesh.nativeapp.feature.navigation.ShellHomeCountMark
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenu
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenuItem
import com.forkmesh.nativeapp.ui.kit.FmModalBottomSheet
import com.forkmesh.nativeapp.ui.kit.FmSwitch
import com.forkmesh.nativeapp.ui.kit.fmGlass
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmHaptics
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmSpace
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlin.math.max
import kotlin.math.min
import kotlin.math.roundToInt

object ShellBarStore {
    private var prefs: ShellBarPrefs? = null
    private val flow = MutableStateFlow(ShellBarStyle())
    private val homeFlow = MutableStateFlow(ShellHomeStyle())
    private val usesFlow = MutableStateFlow<Map<String, Int>>(emptyMap())
    private val recentFlow = MutableStateFlow<List<String>>(emptyList())
    val style: StateFlow<ShellBarStyle> get() = flow
    val home: StateFlow<ShellHomeStyle> get() = homeFlow
    val uses: StateFlow<Map<String, Int>> get() = usesFlow
    val recent: StateFlow<List<String>> get() = recentFlow
    private val layoutFlow = MutableStateFlow(ShellBarLayout())
    val layout: StateFlow<ShellBarLayout> get() = layoutFlow
    private val menuFlow = MutableStateFlow(ShellMenuLayout())
    val menu: StateFlow<ShellMenuLayout> get() = menuFlow
    var measured by mutableStateOf(0.dp)
    val keep: List<String> get() = listOf(ShellBarPrefs.HOME_ID)

    private fun prefs(): ShellBarPrefs? {
        prefs?.let { return it }
        val store = runCatching { Services.get<PrefStore>() }.getOrNull() ?: return null
        return ShellBarPrefs(store).also {
            prefs = it
            publish(it)
        }
    }

    private fun publish(store: ShellBarPrefs) {
        flow.value = store.barStyle
        homeFlow.value = store.home
        usesFlow.value = store.uses
        recentFlow.value = store.recent
        layoutFlow.value = store.layout
        menuFlow.value = store.menu
    }

    internal fun arrangedSections(sections: List<ShellHomeSection>): List<ShellHomeSection> {
        val store = prefs() ?: return sections
        return store.arranged(sections, { it.id }, { it.entries }, { it.id }).map { (section, entries) -> ShellHomeSection(section.id, section.title, entries) }
    }

    internal fun moveInMenu(id: String, to: String, near: String?, current: List<ShellHomeSection>): Boolean {
        val store = prefs() ?: return false
        val moved = store.moveInMenu(id, to, near, current.map { section -> section.id to section.entries.map { it.id } })
        if (moved) publish(store)
        return moved
    }

    fun resetMenu() {
        val store = prefs() ?: return
        store.resetMenu()
        publish(store)
    }

    internal fun slots(cells: List<ShellBarCell>): List<ShellBarSlot<ShellBarCell>> =
        prefs()?.slots(cells) { it.id } ?: cells.take(flow.value.capacity).map { ShellBarSlot.Single(it) }

    internal fun inBar(id: String, cells: List<ShellBarCell>): Boolean = prefs()?.inBar(id, cells) { it.id } ?: false

    internal fun barIds(cells: List<ShellBarCell>): Set<String> = prefs()?.barIds(cells) { it.id } ?: emptySet()

    internal fun barIsFull(cells: List<ShellBarCell>): Boolean = prefs()?.barIsFull(cells) { it.id } ?: true

    internal fun removeFromBar(ids: List<String>, cells: List<ShellBarCell>) {
        val store = prefs() ?: return
        store.removeFromBar(ids, cells) { it.id }
        publish(store)
    }

    internal fun putInBar(id: String, at: String?, cells: List<ShellBarCell>): Boolean {
        val store = prefs() ?: return false
        val placed = store.putInBar(id, at, cells) { it.id }
        publish(store)
        return placed
    }

    internal fun canStep(key: String, step: ShellBarStep, cells: List<ShellBarCell>): Boolean = prefs()?.canStep(key, step, cells) { it.id } ?: false

    internal fun step(key: String, step: ShellBarStep, cells: List<ShellBarCell>) {
        val store = prefs() ?: return
        store.step(key, step, cells) { it.id }
        publish(store)
    }

    fun arrange(edit: ShellBarLayout.() -> Unit) {
        val store = prefs() ?: return
        store.arrange(edit)
        publish(store)
    }

    fun resetLayout() {
        arrange {
            hidden = emptyList()
            folders = emptyList()
            order = emptyList()
            bar = null
        }
    }

    fun use(id: String) {
        val store = prefs() ?: return
        store.use(id)
        publish(store)
    }

    fun clearUsage() {
        val store = prefs() ?: return
        store.clear()
        publish(store)
    }

    fun editHome(change: ShellHomeStyle.() -> Unit) {
        val store = prefs()
        if (store == null) {
            homeFlow.value = homeFlow.value.copy().apply(change)
            return
        }
        store.styleHome(change)
        publish(store)
    }

    fun resetHome() {
        editHome {
            val base = ShellHomeStyle()
            sort = base.sort
            grouped = base.grouped
            showTitles = base.showTitles
            groupOrder = base.groupOrder
            hidden = base.hidden
            icons = base.icons
            across = base.across
            showCounts = base.showCounts
            highlightFrequent = base.highlightFrequent
            hideBarIcons = base.hideBarIcons
        }
    }

    fun load() {
        prefs()
    }

    val homeIcons: ShellHomeIconStyle get() = prefs()?.home?.icons ?: ShellHomeIconStyle.outline

    fun edit(change: ShellBarStyle.() -> Unit) {
        val store = prefs()
        if (store == null) {
            flow.value = flow.value.copy().apply(change).clamped
            return
        }
        store.styleBar(change)
        publish(store)
    }

    fun reset() {
        edit {
            val base = ShellBarStyle()
            shape = base.shape
            fill = base.fill
            icons = base.icons
            size = base.size
            labels = base.labels
            selection = base.selection
            width = base.width
            corner = base.corner
            spacing = base.spacing
            limit = base.limit
            showCounts = base.showCounts
            showActivity = base.showActivity
            highlightFrequent = base.highlightFrequent
            marks = base.marks
            rows = base.rows
        }
    }
}

internal class ShellBarCell(
    val id: String,
    val title: String,
    val icon: String,
    val badge: Int,
    val selected: Boolean,
    val running: Boolean = false,
    val desktopOffline: Boolean = false,
    val active: Boolean = false,
    val progress: com.forkmesh.nativeapp.shell.ShellBarProgress? = null,
    val description: String = title,
    val modifier: Modifier = Modifier,
    val onClick: () -> Unit = {},
)

private object ShellBarBase {
    const val CELL_WIDTH = 64.0
    const val CELL_HEIGHT = 52.0
    const val ICON = 24.0
    val innerRadius = 10.dp
}

private fun barShape(style: ShellBarStyle, side: Dp, rows: Int): Shape = when (style.shape) {
    ShellBarShape.capsule -> if (rows > 1) RoundedCornerShape(style.barRadius(side.value.toDouble()).dp) else RoundedCornerShape(50)
    ShellBarShape.panel -> RoundedCornerShape(style.barRadius(side.value.toDouble()).dp)
    ShellBarShape.squares, ShellBarShape.circles, ShellBarShape.icons -> RoundedCornerShape(18.dp)
}

private fun cellShape(style: ShellBarStyle, side: Dp, first: Boolean, last: Boolean, top: Boolean, bottom: Boolean): Shape = when (style.shape) {
    ShellBarShape.capsule, ShellBarShape.panel -> {
        val outer = max(0.0, style.barRadius(side.value.toDouble()) - ShellBarStyle.PADDING).dp
        val inner = minOf(ShellBarBase.innerRadius, outer)
        fun corner(edge: Boolean, row: Boolean) = if (edge && row) outer else inner
        RoundedCornerShape(
            topStart = corner(first, top),
            topEnd = corner(last, top),
            bottomEnd = corner(last, bottom),
            bottomStart = corner(first, bottom),
        )
    }
    ShellBarShape.circles -> CircleShape
    ShellBarShape.squares -> RoundedCornerShape(style.cellRadius(side.value.toDouble()).dp)
    ShellBarShape.icons -> RoundedCornerShape(min(14.0, side.value * 0.3).dp)
}

@Composable
private fun Modifier.shellBarBackdrop(shape: Shape, fill: ShellBarFill, lift: Dp = 12.dp): Modifier {
    val c = Fm.colors
    return when (fill) {
        ShellBarFill.solid -> {
            val shadow = Color.Black.copy(alpha = if (c.dark) 0.35f else 0.12f)
            this
                .shadow(lift, shape, clip = false, ambientColor = shadow, spotColor = shadow)
                .clip(shape)
                .background(if (c.dark) c.bgElevated else c.bgRaised)
                .border(0.5.dp, c.border.copy(alpha = 0.8f), shape)
        }
        ShellBarFill.glass -> fmGlass(shape)
        ShellBarFill.clear -> fmGlass(shape, tint = c.bgRaised.copy(alpha = if (c.dark) 0.22f else 0.3f))
        ShellBarFill.tinted -> fmGlass(shape, tint = c.accent.copy(alpha = 0.35f).compositeOver(c.bgRaised.copy(alpha = if (c.dark) 0.55f else 0.66f)))
    }
}

@Composable
private fun Modifier.shellBarSlotChrome(shape: Shape, style: ShellBarStyle, selected: Boolean): Modifier {
    val c = Fm.colors
    var result = this
    if (style.shape.separate && style.shape.backed) result = result.shellBarBackdrop(shape, style.fill, lift = 5.dp)
    if (!selected) return result
    result = when (style.selection) {
        ShellBarSelection.fill -> result.clip(shape).background(c.accent.copy(alpha = if (style.shape.separate) 0.2f else 0.14f))
        ShellBarSelection.glow -> result.drawBehind {
            val side = min(size.width, size.height)
            drawCircle(
                Brush.radialGradient(listOf(c.accent.copy(alpha = 0.42f), c.accent.copy(alpha = 0f)), center = center, radius = side * 0.6f),
                radius = side * 0.6f,
                center = center,
            )
        }
        ShellBarSelection.outline -> result.border(1.5.dp, c.accent, shape)
        ShellBarSelection.dot -> result.drawBehind {
            drawCircle(c.accent, radius = 2.5.dp.toPx(), center = Offset(size.width / 2, size.height - 2.dp.toPx() - 2.5.dp.toPx()))
        }
        ShellBarSelection.underline -> result.drawBehind {
            val w = 18.dp.toPx()
            val h = 3.dp.toPx()
            drawRoundRect(
                c.accent,
                topLeft = Offset((size.width - w) / 2, size.height - 1.5.dp.toPx() - h),
                size = androidx.compose.ui.geometry.Size(w, h),
                cornerRadius = androidx.compose.ui.geometry.CornerRadius(h / 2, h / 2),
            )
        }
        ShellBarSelection.none -> result
    }
    return result
}

@Composable
internal fun ShellBarRows(
    slots: List<ShellBarSlot<ShellBarCell>>,
    style: ShellBarStyle,
    available: Dp,
    interactive: Boolean,
    cells: List<ShellBarCell> = emptyList(),
    onSettings: () -> Unit = {},
) {
    val metrics = style.metrics(available.value.toDouble(), slots.size, ShellBarBase.CELL_WIDTH, ShellBarBase.CELL_HEIGHT)
    val side = metrics.height.dp
    val slot = metrics.slot.dp
    val lines = style.rowSplit(slots)
    val shape = barShape(style, side, metrics.rows)
    val backed = if (style.shape.separate) Modifier else Modifier.shellBarBackdrop(shape, style.fill)
    val sized = if (style.width == ShellBarWidth.fit) Modifier.width(metrics.width.dp) else Modifier.fillMaxWidth()
    Box(Modifier.fillMaxWidth(), contentAlignment = Alignment.Center) {
        Column(
            sized
                .then(backed)
                .then(if (metrics.scrolls) Modifier.horizontalScroll(rememberScrollState()) else Modifier)
                .padding(metrics.padding.dp),
            verticalArrangement = Arrangement.spacedBy(metrics.rowGap.dp),
        ) {
            lines.forEachIndexed { row, line ->
                Row(horizontalArrangement = Arrangement.spacedBy(metrics.gap.dp)) {
                    line.forEachIndexed { index, entry ->
                        val cellShape = cellShape(
                            style,
                            side,
                            first = index == 0,
                            last = index == line.lastIndex && line.size == metrics.columns,
                            top = row == 0,
                            bottom = row == lines.lastIndex,
                        )
                        when (entry) {
                            is ShellBarSlot.Single -> ShellBarCellView(entry.item, style, slot, side, cellShape, interactive, cells, onSettings)
                            is ShellBarSlot.Folder -> ShellBarFolderView(entry.folder, entry.members, style, slot, side, cellShape, interactive, cells, onSettings)
                        }
                    }
                }
            }
        }
    }
}

@OptIn(ExperimentalFoundationApi::class)
@Composable
private fun ShellBarCellView(
    cell: ShellBarCell,
    style: ShellBarStyle,
    slot: Dp,
    side: Dp,
    shape: Shape,
    interactive: Boolean,
    cells: List<ShellBarCell>,
    onSettings: () -> Unit,
    onRan: () -> Unit = {},
) {
    val c = Fm.colors
    val haptics = FmHaptics.current
    val interaction = remember { MutableInteractionSource() }
    val pressed by interaction.collectIsPressedAsState()
    var menu by remember { mutableStateOf(false) }
    val glyph = (ShellBarBase.ICON * style.size.scale).roundToInt().dp
    val tiled = style.icons == ShellHomeIconStyle.tiles
    val tile = (glyph.value * 1.5f).roundToInt().dp
    val tint = if (cell.selected || cell.running || style.icons == ShellHomeIconStyle.accent) c.accent else c.textPrimary
    val homeMarks = style.marks == ShellBarMarks.home
    Box(
        Modifier
            .size(slot, side)
            .graphicsLayer {
                val k = if (pressed) 0.94f else 1f
                scaleX = k
                scaleY = k
                alpha = if (pressed) 0.7f else 1f
            }
            .shellBarSlotChrome(shape, style, cell.selected)
            .then(if (interactive) Modifier.shellBarDropSlot(cell.id) else Modifier)
            .graphicsLayer { alpha = if (ShellDrag.isDragging(cell.id, bar = true)) 0.35f else 1f }
            .shellDragSource(
                cell.id, cell.icon, fromBar = true, enabled = interactive && cell.id !in ShellBarStore.keep,
                onStart = {
                    menu = false
                    haptics.selection()
                },
                onDrop = { at ->
                    if (ShellDrag.overBar) {
                        val target = ShellDrag.barSlotAt(at)
                        if (target != null && target != cell.id) {
                            if (ShellBarStore.putInBar(cell.id, target, cells)) haptics.success() else haptics.warning()
                        }
                    } else {
                        ShellBarStore.removeFromBar(listOf(cell.id), cells)
                        haptics.success()
                    }
                },
            )
            .then(
                if (interactive) {
                    Modifier.combinedClickable(
                        interactionSource = interaction,
                        indication = null,
                        onLongClick = {
                            haptics.palette()
                            menu = true
                        },
                        onClick = {
                            haptics.selection()
                            ShellBarStore.use(cell.id)
                            onRan()
                            cell.onClick()
                        },
                    )
                } else {
                    Modifier
                },
            )
            .semantics(mergeDescendants = true) {
                role = Role.Tab
                selected = cell.selected
                contentDescription = cell.description
            },
        contentAlignment = Alignment.Center,
    ) {
        Column(horizontalAlignment = Alignment.CenterHorizontally, verticalArrangement = Arrangement.Center) {
            Box(contentAlignment = Alignment.Center, modifier = if (tiled) Modifier.size(tile) else Modifier.size(glyph + 4.dp)) {
                val ringShown = cell.progress != null && style.showActivity
                if (cell.active && style.showActivity && !ringShown) ShellActivityHaloView((if (tiled) tile else glyph) + 10.dp)
                if (tiled) ShellBarTile(tile, cell.selected)
                if (ringShown) ShellBarProgressRing(cell.progress!!, if (tiled) tile - 5.dp else glyph + 10.dp, if (tiled) 1.75f else 2f)
                val shrink by androidx.compose.animation.core.animateFloatAsState(if (ringShown) 0.78f else 1f, label = "shellRingGlyph")
                Box(cell.modifier.graphicsLayer { scaleX = shrink; scaleY = shrink }) {
                    FmIcon(
                        cell.icon,
                        size = if (tiled) (glyph.value * 0.82f).dp else glyph,
                        tint = tint,
                        outlined = style.icons != ShellHomeIconStyle.filled && !cell.selected,
                    )
                }
                if (cell.badge > 0) {
                    val text = if (style.showCounts || homeMarks) (if (cell.badge > 9999) "${cell.badge / 1000}k" else "%,d".format(cell.badge)) else if (cell.badge > 999) "999+" else "${cell.badge}"
                    if (homeMarks) {
                        ShellHomeCountMark(
                            text,
                            unread = !cell.running,
                            modifier = Modifier.align(Alignment.TopEnd).offset(x = 14.dp, y = (-8).dp),
                        )
                    } else {
                        Text(
                            text,
                            Modifier.align(Alignment.TopEnd).offset(x = 12.dp, y = (-6).dp),
                            style = TextStyle(fontFamily = FmType.ui, fontSize = 11.sp, lineHeight = 13.sp, fontWeight = FontWeight.SemiBold, color = c.count, fontFeatureSettings = "tnum"),
                        )
                    }
                }
                if (cell.desktopOffline) {
                    DesktopOfflineDot(Modifier.align(Alignment.BottomStart).offset(x = (-4).dp, y = 4.dp))
                }
            }
            if (style.labels.shows(cell.selected)) {
                Spacer(Modifier.height(3.dp))
                Text(
                    cell.title,
                    style = TextStyle(
                        fontFamily = FmType.ui,
                        fontSize = (10 * style.size.scale).roundToInt().sp,
                        lineHeight = ((10 * style.size.scale).roundToInt() + 2).sp,
                        fontWeight = if (cell.selected) FontWeight.SemiBold else FontWeight.Medium,
                        color = if (cell.selected) c.accent else c.textSecondary,
                    ),
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
            }
        }
        if (interactive) {
            FmDropdownMenu(expanded = menu, onDismissRequest = { menu = false }) {
                ShellBarItemMenu(cell, cells, onSettings) { menu = false }
            }
        }
    }
}

@Composable
private fun ShellBarTile(side: Dp, selected: Boolean) {
    val c = Fm.colors
    val shape = RoundedCornerShape(side * 0.3f)
    val raised = if (c.dark) c.bgElevated else c.bgRaised
    val shadow = Color.Black.copy(alpha = if (c.dark) 0.45f else 0.1f)
    Box(
        Modifier
            .size(side)
            .shadow(2.5.dp, shape, clip = false, ambientColor = shadow, spotColor = shadow)
            .clip(shape)
            .background(Brush.verticalGradient(listOf(raised, c.bgOverlay)))
            .then(if (selected) Modifier.background(c.accent.copy(alpha = 0.16f)) else Modifier)
            .border(0.75.dp, Brush.verticalGradient(listOf(Color.White.copy(alpha = if (c.dark) 0.16f else 0.9f), Color.White.copy(alpha = if (c.dark) 0.03f else 0.2f))), shape),
    )
}

@Composable
internal fun ShellStyledBar(cells: List<ShellBarCell>) {
    ShellBarStore.load()
    val style by ShellBarStore.style.collectAsState()
    var settings by remember { mutableStateOf(false) }
    val density = LocalDensity.current
    BoxWithConstraints(
        Modifier
            .onSizeChanged { ShellBarStore.measured = with(density) { it.height.toDp() } }
            .onGloballyPositioned { ShellDrag.barArea = it.boundsInRoot() }
            .padding(start = ShellBar.margin, end = ShellBar.margin, top = FmSpace.x1, bottom = ShellBar.bottomInset())
            .fillMaxWidth(),
    ) {
        val layout by ShellBarStore.layout.collectAsState()
        val slots = remember(cells, layout, style) { ShellBarStore.slots(cells) }
        LaunchedEffect(slots) {
            val ids = slots.map { ShellBarLayout.slotId(it) { cell -> cell.id } }.toSet()
            ShellDrag.barSlots.keys.retainAll(ids)
        }
        ShellBarRows(slots, style, maxWidth, interactive = true, cells = cells, onSettings = { settings = true })
    }
    if (settings) ShellBarSettingsSheet(onClose = { settings = false })
}

private val previewCells = listOf(
    ShellBarCell("preview-home", "Home", "grid_view", 0, false),
    ShellBarCell("preview-agents", "Agents", "auto_awesome", 0, true),
    ShellBarCell("preview-inbox", "Inbox", "inbox", 4, false),
    ShellBarCell("preview-chat", "Chat", "forum", 0, false),
    ShellBarCell("preview-repos", "Repos", "library_books", 0, false),
)

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun ShellBarSettingsSheet(onClose: () -> Unit) {
    FmModalBottomSheet(onDismissRequest = onClose, isScrollControlled = true, showDragHandle = true) {
        ShellBarSettingsForm(onDone = onClose)
    }
}

@Composable
fun ShellBarSettingsForm(onDone: (() -> Unit)? = null) {
    ShellBarStore.load()
    val c = Fm.colors
    val style by ShellBarStore.style.collectAsState()
    val haptics = FmHaptics.current
    fun edit(change: ShellBarStyle.() -> Unit) {
        haptics.selection()
        ShellBarStore.edit(change)
    }
    Column(Modifier.fillMaxWidth().verticalScroll(rememberScrollState()).padding(bottom = FmSpace.x6)) {
        Box(Modifier.fillMaxWidth().heightIn(min = 44.dp).padding(horizontal = FmSpace.x4), contentAlignment = Alignment.Center) {
            Text("Bottom bar", style = Fm.text.titleMedium)
            if (onDone != null) {
                Text(
                    "Done",
                    Modifier.align(Alignment.CenterEnd).clickable(onClick = onDone).padding(FmSpace.x2),
                    style = Fm.text.labelLarge.copy(color = c.accent),
                )
            }
        }
        SettingsGroup(footer = "Changes show here and on the bar at once. Touch and hold any icon in the bar to open these settings.") {
            BoxWithConstraints(
                Modifier
                    .fillMaxWidth()
                    .background(
                        Brush.linearGradient(
                            listOf(c.accent.copy(alpha = if (c.dark) 0.45f else 0.3f), c.bgBase, c.accent.copy(alpha = if (c.dark) 0.2f else 0.12f)),
                        ),
                    )
                    .padding(vertical = FmSpace.x4, horizontal = FmSpace.x2),
            ) {
                ShellBarRows(previewCells.take(max(1, min(style.capacity, previewCells.size))).map { ShellBarSlot.Single(it) }, style, maxWidth, interactive = false)
            }
        }
        SettingsGroup(
            "Shape",
            footer = shapeFooter(style.shape) + " Double height stacks the icons in two rows.",
        ) {
            Choices(ShellBarShape.entries, style.shape, { it.title }, ::shapeIcon) { edit { shape = it } }
            Choices(ShellBarFill.entries, style.fill, { it.title }, ::fillIcon, enabled = style.shape.backed) { edit { fill = it } }
            SliderRow("Corners", "rounded_corner", "Square", "Round", style.corner, enabled = style.shape.cornered) { value -> ShellBarStore.edit { corner = value } }
            SliderRow("Spacing", "swap_horiz", "Tight", "Wide", style.spacing, enabled = !(style.shape.separate && style.width == ShellBarWidth.full)) { value -> ShellBarStore.edit { spacing = value } }
            Segmented(ShellBarWidth.entries, style.width, { it.title }) { edit { width = it } }
            LabeledRow("Height", "view_day") {
                Segmented(ShellBarRows.entries, style.rows, { it.title }, Modifier.width(200.dp)) { edit { rows = it } }
            }
        }
        SettingsGroup(
            "Icons",
            footer = "The same styles as the Home screen: Outline and Filled keep the icons plain, Accent draws them in your accent colour and Tiles sets each one on a rounded tile.",
        ) {
            Segmented(ShellHomeIconStyle.entries, style.icons, { it.title }) { edit { icons = it } }
            LabeledRow("Size", "format_size") {
                Segmented(ShellBarSize.entries, style.size, { it.title }, Modifier.width(200.dp)) { edit { size = it } }
            }
            LabeledRow("Labels", "text_fields") {
                Segmented(ShellBarLabels.entries, style.labels, { it.title }, Modifier.width(220.dp)) { edit { labels = it } }
            }
            val home = ShellBarStore.homeIcons
            ActionRow("Match the Home icons (${home.title})", "grid_view", enabled = !(style.icons == home && style.marks == ShellBarMarks.home)) {
                edit {
                    icons = home
                    marks = ShellBarMarks.home
                }
            }
        }
        SettingsGroup("Selected icon", footer = "The open page's icon is always drawn in the accent colour; this adds a mark around or under it.") {
            Choices(ShellBarSelection.entries, style.selection, { it.title }, ::selectionIcon) { edit { selection = it } }
        }
        SettingsGroup(
            "What the bar shows",
            footer = "A double height bar holds twice as many icons, in two rows. More icons than fit scroll sideways. Unread badges always show. Like Home draws them as on the Home screen: solid blue count pills on the icon's corner.",
        ) {
            StepperRow(if (style.rows == ShellBarRows.double) "Icons in each row" else "Icons in the bar", "apps", style.limit, ShellBarStyle.LIMIT_CHOICES.first(), ShellBarStyle.LIMIT_CHOICES.last()) { value -> edit { limit = value } }
            ToggleRow("Show counts", "tag", style.showCounts) { value -> edit { showCounts = value } }
            ToggleRow("Show activity", "monitor_heart", style.showActivity) { value -> edit { showActivity = value } }
            LabeledRow("Badges", "fiber_manual_record") {
                Segmented(ShellBarMarks.entries, style.marks, { it.title }, Modifier.width(220.dp)) { edit { marks = it } }
            }
            ToggleRow("Highlight frequently used", "auto_awesome", style.highlightFrequent) { value -> edit { highlightFrequent = value } }
        }
        SettingsGroup {
            ActionRow("Reset bar look", "restart_alt", enabled = !style.isDefault, destructive = true) {
                haptics.selection()
                ShellBarStore.reset()
            }
        }
    }
}

private fun shapeFooter(shape: ShellBarShape): String = when (shape) {
    ShellBarShape.capsule, ShellBarShape.panel -> "One piece of glass holds every icon. Fit the icons shrinks it to its icons, centred."
    ShellBarShape.squares, ShellBarShape.circles -> "Each icon sits on its own button. At full width they spread across the bar; fit them to set the spacing yourself."
    ShellBarShape.icons -> "Just the icons and labels, with a soft shadow so they read over any background."
}

private fun shapeIcon(shape: ShellBarShape): String = when (shape) {
    ShellBarShape.capsule -> "crop_7_5"
    ShellBarShape.panel -> "rectangle"
    ShellBarShape.squares -> "crop_square"
    ShellBarShape.circles -> "circle"
    ShellBarShape.icons -> "apps"
}

private fun fillIcon(fill: ShellBarFill): String = when (fill) {
    ShellBarFill.glass -> "blur_on"
    ShellBarFill.clear -> "opacity"
    ShellBarFill.tinted -> "palette"
    ShellBarFill.solid -> "square"
}

private fun selectionIcon(selection: ShellBarSelection): String = when (selection) {
    ShellBarSelection.fill -> "format_color_fill"
    ShellBarSelection.outline -> "crop_free"
    ShellBarSelection.glow -> "flare"
    ShellBarSelection.dot -> "fiber_manual_record"
    ShellBarSelection.underline -> "format_underlined"
    ShellBarSelection.none -> "block"
}

@Composable
internal fun SettingsGroup(header: String? = null, footer: String? = null, content: @Composable ColumnScope.() -> Unit) {
    val c = Fm.colors
    Column(Modifier.fillMaxWidth().padding(horizontal = FmSpace.x4, vertical = FmSpace.x2)) {
        if (header != null) {
            Text(header.uppercase(), Modifier.padding(start = FmSpace.x4, bottom = 6.dp), style = Fm.text.bodySmall.copy(fontSize = 13.sp, color = c.textSecondary))
        }
        Column(
            Modifier
                .fillMaxWidth()
                .clip(RoundedCornerShape(12.dp))
                .background(if (c.dark) c.bgElevated else c.bgRaised)
                .padding(horizontal = FmSpace.x4, vertical = FmSpace.x1),
            content = content,
        )
        if (footer != null) {
            Text(footer, Modifier.padding(start = FmSpace.x4, end = FmSpace.x4, top = 6.dp), style = Fm.text.bodySmall.copy(fontSize = 13.sp, color = c.textSecondary))
        }
    }
}

@Composable
internal fun <T> Choices(choices: List<T>, selected: T, title: (T) -> String, icon: (T) -> String, enabled: Boolean = true, onPick: (T) -> Unit) {
    val c = Fm.colors
    Row(Modifier.fillMaxWidth().padding(vertical = FmSpace.x1).alpha(if (enabled) 1f else 0.45f), horizontalArrangement = Arrangement.spacedBy(FmSpace.x1)) {
        choices.forEach { choice ->
            val on = choice == selected
            val shape = RoundedCornerShape(10.dp)
            Column(
                Modifier
                    .weight(1f)
                    .clip(shape)
                    .background(if (on) c.accent.copy(alpha = 0.14f) else c.textPrimary.copy(alpha = 0.05f))
                    .border(1.25.dp, if (on) c.accent else Color.Transparent, shape)
                    .clickable(enabled = enabled) { onPick(choice) }
                    .padding(vertical = FmSpace.x2)
                    .semantics(mergeDescendants = true) {
                        role = Role.Button
                        this.selected = on
                    },
                horizontalAlignment = Alignment.CenterHorizontally,
                verticalArrangement = Arrangement.spacedBy(4.dp),
            ) {
                FmIcon(icon(choice), size = 18.dp, tint = if (on) c.accent else c.textSecondary, outlined = !on)
                Text(
                    title(choice),
                    style = TextStyle(fontFamily = FmType.ui, fontSize = 11.sp, lineHeight = 13.sp, fontWeight = if (on) FontWeight.SemiBold else FontWeight.Medium, color = if (on) c.accent else c.textSecondary),
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                    textAlign = TextAlign.Center,
                )
            }
        }
    }
}

@Composable
internal fun <T> Segmented(choices: List<T>, selected: T, title: (T) -> String, modifier: Modifier = Modifier.fillMaxWidth(), onPick: (T) -> Unit) {
    val c = Fm.colors
    Row(
        modifier
            .padding(vertical = 6.dp)
            .height(32.dp)
            .clip(RoundedCornerShape(9.dp))
            .background(c.textPrimary.copy(alpha = if (c.dark) 0.12f else 0.06f))
            .padding(2.dp),
    ) {
        choices.forEach { choice ->
            val on = choice == selected
            Box(
                Modifier
                    .weight(1f)
                    .fillMaxHeight()
                    .then(if (on) Modifier.shadow(1.dp, RoundedCornerShape(7.dp)).clip(RoundedCornerShape(7.dp)).background(if (c.dark) Color(0xFF636366) else Color.White) else Modifier)
                    .clickable { onPick(choice) }
                    .semantics(mergeDescendants = true) {
                        role = Role.Tab
                        this.selected = on
                    },
                contentAlignment = Alignment.Center,
            ) {
                Text(title(choice), style = TextStyle(fontFamily = FmType.ui, fontSize = 13.sp, fontWeight = if (on) FontWeight.SemiBold else FontWeight.Medium, color = c.textPrimary), maxLines = 1, overflow = TextOverflow.Ellipsis)
            }
        }
    }
}

@Composable
internal fun LabeledRow(title: String, icon: String, content: @Composable () -> Unit) {
    val c = Fm.colors
    Row(Modifier.fillMaxWidth().heightIn(min = 44.dp), verticalAlignment = Alignment.CenterVertically) {
        FmIcon(icon, size = 20.dp, tint = c.accent, outlined = true)
        Spacer(Modifier.width(FmSpace.x3))
        Text(title, Modifier.weight(1f), style = Fm.text.bodyLarge)
        content()
    }
}

@Composable
internal fun ToggleRow(title: String, icon: String, checked: Boolean, onChange: (Boolean) -> Unit) {
    LabeledRow(title, icon) { FmSwitch(checked = checked, onCheckedChange = onChange) }
}

@Composable
internal fun StepperRow(title: String, icon: String, value: Int, low: Int, high: Int, onChange: (Int) -> Unit) {
    val c = Fm.colors
    LabeledRow(title, icon) {
        Text("$value", Modifier.padding(end = FmSpace.x2), style = Fm.text.bodyLarge.copy(color = c.textSecondary, fontFeatureSettings = "tnum"))
        Row(
            Modifier
                .height(32.dp)
                .clip(RoundedCornerShape(9.dp))
                .background(c.textPrimary.copy(alpha = if (c.dark) 0.12f else 0.06f)),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Box(Modifier.width(46.dp).fillMaxWidth().clickable(enabled = value > low) { onChange(value - 1) }, contentAlignment = Alignment.Center) {
                FmIcon("remove", size = 18.dp, tint = if (value > low) c.textPrimary else c.textDisabled)
            }
            Box(Modifier.width(0.5.dp).height(18.dp).background(c.textPrimary.copy(alpha = 0.2f)))
            Box(Modifier.width(46.dp).fillMaxWidth().clickable(enabled = value < high) { onChange(value + 1) }, contentAlignment = Alignment.Center) {
                FmIcon("add", size = 18.dp, tint = if (value < high) c.textPrimary else c.textDisabled)
            }
        }
    }
}

@Composable
internal fun SliderRow(title: String, icon: String, low: String, high: String, value: Double, enabled: Boolean, onChange: (Double) -> Unit) {
    val c = Fm.colors
    val haptics = FmHaptics.current
    Column(Modifier.fillMaxWidth().padding(vertical = 4.dp).alpha(if (enabled) 1f else 0.45f)) {
        Row(verticalAlignment = Alignment.CenterVertically, modifier = Modifier.heightIn(min = 32.dp)) {
            FmIcon(icon, size = 20.dp, tint = c.accent, outlined = true)
            Spacer(Modifier.width(FmSpace.x3))
            Text(title, style = Fm.text.bodyLarge)
        }
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(low, style = Fm.text.bodySmall.copy(color = c.textTertiary))
            FmIosSlider(
                value = value.toFloat(),
                onValueChange = { onChange(it.toDouble()) },
                onValueChangeFinished = { haptics.selection() },
                enabled = enabled,
                modifier = Modifier.weight(1f).padding(horizontal = FmSpace.x2).semantics { contentDescription = title },
            )
            Text(high, style = Fm.text.bodySmall.copy(color = c.textTertiary))
        }
    }
}

@Composable
internal fun ActionRow(title: String, icon: String, enabled: Boolean = true, destructive: Boolean = false, onClick: () -> Unit) {
    val c = Fm.colors
    val tint = if (destructive) c.danger else c.accent
    Row(
        Modifier
            .fillMaxWidth()
            .heightIn(min = 44.dp)
            .alpha(if (enabled) 1f else 0.45f)
            .clickable(enabled = enabled, onClick = onClick)
            .semantics(mergeDescendants = true) { role = Role.Button },
        verticalAlignment = Alignment.CenterVertically,
    ) {
        FmIcon(icon, size = 20.dp, tint = tint, outlined = true)
        Spacer(Modifier.width(FmSpace.x3))
        Text(title, style = Fm.text.bodyLarge.copy(color = tint))
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun FmIosSlider(
    value: Float,
    onValueChange: (Float) -> Unit,
    modifier: Modifier = Modifier,
    enabled: Boolean = true,
    onValueChangeFinished: (() -> Unit)? = null,
) {
    val c = Fm.colors
    val interaction = remember { MutableInteractionSource() }
    Slider(
        value = value,
        onValueChange = onValueChange,
        modifier = modifier,
        enabled = enabled,
        onValueChangeFinished = onValueChangeFinished,
        interactionSource = interaction,
        thumb = {
            Box(
                Modifier
                    .size(28.dp)
                    .shadow(3.dp, CircleShape, ambientColor = Color.Black.copy(alpha = 0.2f), spotColor = Color.Black.copy(alpha = 0.2f))
                    .background(Color.White, CircleShape),
            )
        },
        track = { state ->
            val fraction = ((state.value - state.valueRange.start) / (state.valueRange.endInclusive - state.valueRange.start)).coerceIn(0f, 1f)
            Box(
                Modifier
                    .fillMaxWidth()
                    .height(4.dp)
                    .clip(CircleShape)
                    .background(c.textPrimary.copy(alpha = if (c.dark) 0.24f else 0.12f)),
            ) {
                Box(Modifier.fillMaxWidth(fraction).fillMaxHeight().background(c.accent))
            }
        },
    )
}

@Composable
internal fun ShellBarItemMenu(cell: ShellBarCell, cells: List<ShellBarCell>, onSettings: () -> Unit, inBarMenu: Boolean = true, close: () -> Unit) {
    val c = Fm.colors
    val layout by ShellBarStore.layout.collectAsState()
    val messenger = LocalFmMessenger.current
    val scope = rememberCoroutineScope()
    val haptics = FmHaptics.current
    var page by remember { mutableStateOf(ShellBarMenuPage.Main) }
    val keep = cell.id in ShellBarStore.keep
    val docked = ShellBarStore.inBar(cell.id, cells)
    val current = layout.folderOf(cell.id)
    when (page) {
        ShellBarMenuPage.Folders -> {
            layout.folders.forEach { folder ->
                FmDropdownMenuItem(folder.name, onClick = {
                    close()
                    ShellBarStore.arrange { move(cell.id, folder.id) }
                }, icon = if (folder.id == current?.id) "check" else "folder", enabled = folder.id != current?.id)
            }
            FmDropdownMenuItem("New folder…", onClick = {
                close()
                scope.launch {
                    val name = ShellBarLayout.cleanName(messenger.showPromptDialog("New folder", hint = "Folder name", confirmLabel = "Create") ?: return@launch)
                    if (name.isNotEmpty()) ShellBarStore.arrange { createFolder(name, cell.id) }
                }
            }, icon = "create_new_folder")
        }
        ShellBarMenuPage.Replace -> {
            ShellBarStore.slots(cells).filter { slot ->
                when (slot) {
                    is ShellBarSlot.Single -> slot.item.id !in ShellBarStore.keep
                    is ShellBarSlot.Folder -> slot.members.none { it.id in ShellBarStore.keep }
                }
            }.forEach { slot ->
                val title = when (slot) {
                    is ShellBarSlot.Single -> slot.item.title
                    is ShellBarSlot.Folder -> slot.folder.name
                }
                val icon = when (slot) {
                    is ShellBarSlot.Single -> slot.item.icon
                    is ShellBarSlot.Folder -> "folder"
                }
                FmDropdownMenuItem(title, onClick = {
                    close()
                    if (ShellBarStore.putInBar(cell.id, ShellBarLayout.slotId(slot) { it.id }, cells)) haptics.success() else haptics.warning()
                }, icon = icon)
            }
        }
        ShellBarMenuPage.Main -> {
            if (!keep) {
                when {
                    docked -> FmDropdownMenuItem("Remove from bar", onClick = {
                        close()
                        ShellBarStore.removeFromBar(listOf(cell.id), cells)
                        haptics.success()
                    }, icon = "remove_circle_outline", textColor = c.danger)
                    !ShellBarStore.barIsFull(cells) -> FmDropdownMenuItem("Add to bar", onClick = {
                        close()
                        if (ShellBarStore.putInBar(cell.id, null, cells)) haptics.success() else haptics.warning()
                    }, icon = "add_circle_outline")
                    else -> FmDropdownMenuItem("Add to bar in place of…", onClick = { page = ShellBarMenuPage.Replace }, icon = "swap_horiz")
                }
            }
            if (inBarMenu && docked) ShellBarStepItems(cell.id, cells, close)
            FmDropdownMenuItem("Move to folder…", onClick = { page = ShellBarMenuPage.Folders }, icon = "folder")
            if (current != null) {
                FmDropdownMenuItem("Take out of ${current.name}", onClick = {
                    close()
                    ShellBarStore.arrange { move(cell.id, null) }
                }, icon = "drive_file_move")
            }
            if (!layout.isEmpty) {
                FmDropdownMenuItem("Reset bar layout", onClick = {
                    close()
                    ShellBarStore.resetLayout()
                }, icon = "restart_alt", textColor = c.danger)
            }
            FmDropdownMenuItem("Bottom bar settings…", onClick = {
                close()
                onSettings()
            }, icon = "tune")
        }
    }
}

private enum class ShellBarMenuPage { Main, Folders, Replace }

@Composable
private fun ShellBarStepItems(key: String, cells: List<ShellBarCell>, close: () -> Unit) {
    listOf(
        Triple("Move left", "arrow_back", ShellBarStep.left),
        Triple("Move right", "arrow_forward", ShellBarStep.right),
        Triple("Move to the front", "first_page", ShellBarStep.first),
        Triple("Move to the end", "last_page", ShellBarStep.last),
    ).forEach { (title, icon, step) ->
        FmDropdownMenuItem(title, onClick = {
            close()
            ShellBarStore.step(key, step, cells)
        }, icon = icon, enabled = ShellBarStore.canStep(key, step, cells))
    }
}

@OptIn(ExperimentalFoundationApi::class)
@Composable
private fun ShellBarFolderView(
    folder: ShellBarFolder,
    members: List<ShellBarCell>,
    style: ShellBarStyle,
    slot: Dp,
    side: Dp,
    shape: Shape,
    interactive: Boolean,
    cells: List<ShellBarCell>,
    onSettings: () -> Unit,
) {
    val c = Fm.colors
    val haptics = FmHaptics.current
    val messenger = LocalFmMessenger.current
    val scope = rememberCoroutineScope()
    var open by remember { mutableStateOf(false) }
    var menu by remember { mutableStateOf(false) }
    val selected = members.any { it.selected }
    val badge = members.sumOf { it.badge }
    val glyph = (ShellBarBase.ICON * style.size.scale).roundToInt().dp
    val mini = maxOf(7.dp, glyph * 0.42f)
    Box(
        Modifier
            .size(slot, side)
            .shellBarSlotChrome(shape, style, selected || open)
            .then(if (interactive) Modifier.shellBarDropSlot(folder.slotId) else Modifier)
            .then(
                if (interactive) {
                    Modifier.combinedClickable(
                        onLongClick = {
                            haptics.palette()
                            menu = true
                        },
                        onClick = {
                            haptics.selection()
                            open = !open
                        },
                    )
                } else {
                    Modifier
                },
            )
            .semantics(mergeDescendants = true) {
                role = Role.Button
                contentDescription = "${folder.name} folder, " + members.joinToString(", ") { it.title }
            },
        contentAlignment = Alignment.Center,
    ) {
        Column(horizontalAlignment = Alignment.CenterHorizontally) {
            Box {
                Column(
                    Modifier
                        .size(glyph + 6.dp, glyph + 4.dp)
                        .clip(RoundedCornerShape(6.dp))
                        .background(if (selected) c.accent.copy(alpha = 0.18f) else c.textPrimary.copy(alpha = 0.08f)),
                    verticalArrangement = Arrangement.spacedBy(2.dp, Alignment.CenterVertically),
                    horizontalAlignment = Alignment.CenterHorizontally,
                ) {
                    members.take(4).chunked(2).forEach { pair ->
                        Row(horizontalArrangement = Arrangement.spacedBy(2.dp)) {
                            pair.forEach { member -> FmIcon(member.icon, size = mini + 2.dp, tint = if (member.selected) c.accent else c.textPrimary, outlined = !member.selected) }
                        }
                    }
                }
                if (badge > 0) {
                    ShellHomeCountMark(if (badge > 999) "999+" else "$badge", unread = true, modifier = Modifier.align(Alignment.TopEnd).offset(x = 14.dp, y = (-8).dp))
                }
            }
            if (style.labels.shows(selected)) {
                Spacer(Modifier.height(3.dp))
                Text(
                    folder.name,
                    style = TextStyle(
                        fontFamily = FmType.ui,
                        fontSize = (10 * style.size.scale).roundToInt().sp,
                        lineHeight = ((10 * style.size.scale).roundToInt() + 2).sp,
                        fontWeight = if (selected) FontWeight.SemiBold else FontWeight.Medium,
                        color = if (selected) c.accent else c.textSecondary,
                    ),
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
            }
        }
        if (interactive) {
            FmDropdownMenu(expanded = open, onDismissRequest = { open = false }) {
                Text(folder.name, Modifier.padding(start = FmSpace.x3, bottom = FmSpace.x1), style = Fm.text.labelSmall)
                val popoverStyle = style.copy(labels = ShellBarLabels.always, shape = ShellBarShape.capsule, selection = ShellBarSelection.fill)
                members.chunked(4).forEach { row ->
                    Row(Modifier.padding(horizontal = FmSpace.x2), horizontalArrangement = Arrangement.spacedBy(2.dp)) {
                        row.forEach { member ->
                            ShellBarCellView(member, popoverStyle, maxOf(slot, side), side, CircleShape, true, cells, onSettings, onRan = { open = false })
                        }
                    }
                }
            }
            FmDropdownMenu(expanded = menu, onDismissRequest = { menu = false }) {
                FmDropdownMenuItem("Open ${folder.name}", onClick = {
                    menu = false
                    open = true
                }, icon = "folder_open")
                ShellBarStepItems(folder.slotId, cells) { menu = false }
                FmDropdownMenuItem("Rename folder…", onClick = {
                    menu = false
                    scope.launch {
                        val name = ShellBarLayout.cleanName(messenger.showPromptDialog("Rename folder", hint = "Folder name", initialValue = folder.name, confirmLabel = "Rename") ?: return@launch)
                        if (name.isNotEmpty()) ShellBarStore.arrange { renameFolder(folder.id, name) }
                    }
                }, icon = "edit")
                FmDropdownMenuItem("New folder…", onClick = {
                    menu = false
                    scope.launch {
                        val name = ShellBarLayout.cleanName(messenger.showPromptDialog("New folder", hint = "Folder name", confirmLabel = "Create") ?: return@launch)
                        if (name.isNotEmpty()) ShellBarStore.arrange { createFolder(name, null) }
                    }
                }, icon = "create_new_folder")
                FmDropdownMenuItem("Remove folder from bar", onClick = {
                    menu = false
                    ShellBarStore.removeFromBar(folder.items, cells)
                }, icon = "remove_circle_outline")
                FmDropdownMenuItem("Remove folder (keeps its items)", onClick = {
                    menu = false
                    ShellBarStore.arrange { removeFolder(folder.id) }
                }, icon = "folder_delete", textColor = c.danger)
                FmDropdownMenuItem("Bottom bar settings…", onClick = {
                    menu = false
                    onSettings()
                }, icon = "tune")
            }
        }
    }
}

