package com.forkmesh.nativeapp.feature.tools

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.gestures.detectHorizontalDragGestures
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.navigationBars
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.windowInsetsBottomHeight
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.pager.HorizontalPager
import androidx.compose.foundation.pager.rememberPagerState
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.selection.selectable
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.FilterChip
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.PrimaryTabRow
import androidx.compose.material3.Tab
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.tools.AppLogEntry
import com.forkmesh.nativeapp.tools.AppTelemetry
import com.forkmesh.nativeapp.tools.LogCategory
import com.forkmesh.nativeapp.tools.MetricCardModel
import com.forkmesh.nativeapp.tools.TelemetryChart
import com.forkmesh.nativeapp.tools.TelemetryMetric
import com.forkmesh.nativeapp.tools.TelemetryRange
import com.forkmesh.nativeapp.tools.TelemetryScreenText
import com.forkmesh.nativeapp.tools.clockStamp
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmInlineStatus
import com.forkmesh.nativeapp.ui.kit.FmPagerSounds
import com.forkmesh.nativeapp.ui.kit.FmStatus
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmRadius
import com.forkmesh.nativeapp.ui.theme.FmType
import kotlinx.coroutines.launch

internal class TelemetryScreen(
    private val telemetry: AppTelemetry,
    focusMetric: TelemetryMetric? = null,
    logCategory: LogCategory? = null,
    private val now: () -> Long = { System.currentTimeMillis() },
) : Screen() {
    private var range by mutableStateOf(TelemetryRange.M5)
    private var focus by mutableStateOf(focusMetric)
    private var categories by mutableStateOf(if (logCategory != null) setOf(logCategory) else emptySet())
    private var pausedAt by mutableStateOf<Long?>(null)
    private var page by mutableIntStateOf(if (logCategory != null) 1 else 0)
    private val copyStatus = FmStatus()

    override val title: String get() = TelemetryScreenText.TITLE

    private fun visibleEntries(): List<AppLogEntry> =
        TelemetryScreenText.visibleEntries(telemetry.log, categories, range, now(), pausedAt)

    @OptIn(ExperimentalMaterial3Api::class)
    @Composable
    override fun Content(navigator: Navigator) {
        val tick = telemetry.observe()
        val c = Fm.colors
        val context = LocalContext.current
        val scope = rememberCoroutineScope()
        val pager = rememberPagerState(initialPage = page) { 2 }
        FmPagerSounds(pager)
        LaunchedEffect(pager.currentPage) { page = pager.currentPage }

        Column(
            Modifier
                .fillMaxSize()
                .background(c.bgBase),
        ) {
            ToolsAppBar(TelemetryScreenText.TITLE, navigator) {
                FmIconButton(
                    "copy_all_outlined",
                    TelemetryScreenText.COPY_TOOLTIP,
                    onPressed = {
                        val entries = visibleEntries()
                        copyToClipboard(context, TelemetryScreenText.copyText(entries))
                        copyStatus.show(TelemetryScreenText.copiedLines(entries.size))
                    },
                    color = c.textPrimary,
                )
            }
            PrimaryTabRow(selectedTabIndex = pager.currentPage, containerColor = c.bgRaised, contentColor = c.textPrimary) {
                listOf("Stats", "Logs").forEachIndexed { index, label ->
                    Tab(
                        selected = pager.currentPage == index,
                        onClick = { scope.launch { pager.animateScrollToPage(index) } },
                        text = { Text(label) },
                    )
                }
            }
            RangeBar(range) { range = it }
            FmInlineStatus(copyStatus, Modifier.padding(horizontal = 16.dp))
            HorizontalPager(
                state = pager,
                modifier = Modifier
                    .weight(1f)
                    .fillMaxWidth(),
            ) { index ->
                if (index == 0) Stats(tick) else Logs(tick)
            }
        }
    }

    @Composable
    private fun Stats(tick: Int) {
        val c = Fm.colors
        BoxWithConstraints(Modifier.fillMaxSize()) {
            val gap = 12.dp
            val width = maxWidth - 32.dp
            val columns = TelemetryScreenText.statsColumns(width.value)
            val side = (width - gap * (columns - 1)) / columns
            Column(
                Modifier
                    .fillMaxSize()
                    .verticalScroll(rememberScrollState())
                    .padding(16.dp),
            ) {
                Text(
                    TelemetryScreenText.statsHeader(telemetry.uptimeMs),
                    style = Fm.text.bodySmall.copy(color = c.textTertiary),
                )
                Spacer(Modifier.height(gap))
                val focused = focus
                if (focused != null) {
                    MetricCard(
                        metric = focused,
                        large = true,
                        selected = false,
                        onTap = { focus = null },
                        modifier = Modifier
                            .fillMaxWidth()
                            .height(240.dp),
                        tick = tick,
                    )
                    Spacer(Modifier.height(gap))
                }
                TelemetryMetric.entries.chunked(columns).forEachIndexed { row, metrics ->
                    if (row > 0) Spacer(Modifier.height(gap))
                    Row(horizontalArrangement = Arrangement.spacedBy(gap)) {
                        for (metric in metrics) {
                            MetricCard(
                                metric = metric,
                                large = false,
                                selected = metric == focused,
                                onTap = { focus = metric },
                                modifier = Modifier.size(side),
                                tick = tick,
                            )
                        }
                    }
                }
                Spacer(Modifier.windowInsetsBottomHeight(WindowInsets.navigationBars))
            }
        }
    }

    @Composable
    private fun MetricCard(
        metric: TelemetryMetric,
        large: Boolean,
        selected: Boolean,
        onTap: () -> Unit,
        modifier: Modifier,
        tick: Int,
    ) {
        val c = Fm.colors
        val text = Fm.text
        var scrub by remember(metric) { mutableStateOf<Int?>(null) }
        val card = MetricCardModel.of(telemetry, metric, range)
        val current by rememberUpdatedState(card)
        val shape = RoundedCornerShape(FmRadius.md)
        Column(
            modifier
                .clip(shape)
                .background(c.bgRaised)
                .border(1.dp, if (selected) c.accent else c.border, shape)
                .clickable(onClick = onTap)
                .padding(12.dp),
        ) {
            Text(
                metric.title,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
                style = text.labelMedium.copy(color = c.textSecondary),
            )
            Row {
                Text(
                    card.valueText(scrub),
                    modifier = Modifier
                        .weight(1f, fill = false)
                        .alignByBaseline(),
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                    style = (if (large) text.headlineSmall else text.titleLarge).copy(color = c.textPrimary),
                )
                Spacer(Modifier.width(4.dp))
                Text(
                    card.whenText(scrub),
                    modifier = Modifier.alignByBaseline(),
                    style = text.bodySmall.copy(color = c.textTertiary),
                )
            }
            if (large) {
                Text(
                    metric.description,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                    style = text.bodySmall.copy(color = c.textTertiary),
                )
            }
            Spacer(Modifier.height(8.dp))
            val line = c.accent
            val fill = c.accentSubtle
            val grid = c.border
            val surface = c.bgRaised
            Canvas(
                Modifier
                    .fillMaxWidth()
                    .weight(1f)
                    .pointerInput(Unit) {
                        fun scrubTo(x: Float) {
                            val model = current
                            scrub = TelemetryChart.indexAt(x.toDouble(), size.width.toDouble(), model.points.size, model.slots)
                        }
                        detectHorizontalDragGestures(
                            onDragStart = { scrubTo(it.x) },
                            onDragEnd = { scrub = null },
                            onDragCancel = { scrub = null },
                        ) { change, _ -> scrubTo(change.position.x) }
                    },
            ) {
                drawTelemetryChart(
                    card.points,
                    card.slots,
                    metric.floor,
                    line,
                    fill,
                    strokeWidth = 2.dp.toPx(),
                    grid = grid,
                    scrubIndex = card.scrubIndex(scrub),
                    surface = surface,
                )
            }
            Spacer(Modifier.height(8.dp))
            Text(
                card.footer,
                maxLines = 2,
                overflow = TextOverflow.Ellipsis,
                style = text.bodySmall.copy(color = c.textTertiary),
            )
        }
    }

    @Composable
    private fun Logs(tick: Int) {
        val c = Fm.colors
        val log = telemetry.log
        val nowMs = now()
        val counts = TelemetryScreenText.filterCounts(log, range, nowMs, pausedAt)
        val entries = TelemetryScreenText.visibleEntries(log, categories, range, nowMs, pausedAt)
        val caption = TelemetryScreenText.logsCaption(log, categories, entries.size, range, telemetry.monitor?.stallLogPath)
        Column(Modifier.fillMaxSize()) {
            Row(
                Modifier
                    .fillMaxWidth()
                    .height(48.dp)
                    .horizontalScroll(rememberScrollState())
                    .padding(horizontal = 16.dp, vertical = 4.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                FilterChip(
                    selected = categories.isEmpty(),
                    onClick = { categories = emptySet() },
                    label = { Text(TelemetryScreenText.allChipLabel(counts)) },
                )
                for (category in LogCategory.entries) {
                    Spacer(Modifier.width(8.dp))
                    FilterChip(
                        selected = category in categories,
                        onClick = { categories = if (category in categories) categories - category else categories + category },
                        label = { Text(TelemetryScreenText.chipLabel(category, counts)) },
                        leadingIcon = {
                            FmIcon(
                                category.iconName(),
                                size = 16.dp,
                                tint = logCategoryColor(category, counts[category.ordinal]),
                            )
                        },
                    )
                }
            }
            Row(
                Modifier
                    .fillMaxWidth()
                    .padding(start = 16.dp, end = 8.dp, bottom = 4.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Text(caption, modifier = Modifier.weight(1f), style = Fm.text.bodySmall.copy(color = c.textTertiary))
                FmIconButton(
                    if (pausedAt == null) "pause_rounded" else "play_arrow_rounded",
                    if (pausedAt == null) "Pause" else "Resume",
                    onPressed = { pausedAt = if (pausedAt == null) now() else null },
                )
            }
            HorizontalDivider(thickness = 1.dp, color = c.border)
            if (entries.isEmpty()) {
                Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                    Text(
                        TelemetryScreenText.emptyLogs(range),
                        style = Fm.text.bodyMedium.copy(color = c.textTertiary),
                    )
                }
            } else {
                LazyColumn(Modifier.fillMaxSize(), contentPadding = PaddingValues(bottom = 16.dp)) {
                    items(entries) { entry -> LogRow(entry) }
                    item { Spacer(Modifier.windowInsetsBottomHeight(WindowInsets.navigationBars)) }
                }
            }
        }
    }

    @OptIn(ExperimentalFoundationApi::class)
    @Composable
    private fun LogRow(entry: AppLogEntry) {
        val c = Fm.colors
        val context = LocalContext.current
        val mono = Fm.mono(size = FmType.meta, height = FmType.leadingSnug)
        val ink = logCategoryColor(entry.category, 1)
        Row(
            Modifier
                .fillMaxWidth()
                .combinedClickable(
                    onClick = {},
                    onLongClick = {
                        copyToClipboard(context, entry.toString())
                        copyStatus.show(TelemetryScreenText.COPIED_LINE)
                    },
                )
                .padding(horizontal = 16.dp, vertical = 6.dp),
            verticalAlignment = Alignment.Top,
        ) {
            Text(clockStamp(entry.timestampMs), style = mono.copy(color = c.textTertiary))
            Spacer(Modifier.width(8.dp))
            FmIcon(
                entry.category.iconName(),
                modifier = Modifier.padding(top = 1.dp),
                size = 14.dp,
                tint = if (ink == c.textPrimary) c.textSecondary else ink,
                contentDescription = entry.category.label,
            )
            Spacer(Modifier.width(8.dp))
            Text(
                entry.message,
                modifier = Modifier.weight(1f),
                maxLines = 8,
                overflow = TextOverflow.Ellipsis,
                style = mono.copy(color = c.textPrimary),
            )
        }
    }
}

@Composable
private fun RangeBar(range: TelemetryRange, onChanged: (TelemetryRange) -> Unit) {
    val c = Fm.colors
    Row(
        Modifier
            .padding(start = 16.dp, top = 8.dp, end = 16.dp, bottom = 4.dp)
            .fillMaxWidth()
            .height(36.dp)
            .background(c.bgOverlay, RoundedCornerShape(FmRadius.md))
            .padding(2.dp),
    ) {
        for (r in TelemetryRange.entries) {
            val selected = r == range
            val shape = RoundedCornerShape(FmRadius.sm + 2.dp)
            Box(
                Modifier
                    .weight(1f)
                    .fillMaxHeight()
                    .clip(shape)
                    .background(if (selected) c.bgRaised else Color.Transparent)
                    .selectable(selected = selected, role = Role.Button, onClick = { onChanged(r) }),
                contentAlignment = Alignment.Center,
            ) {
                Text(
                    r.label,
                    style = Fm.text.labelMedium.copy(color = if (selected) c.textPrimary else c.textTertiary),
                )
            }
        }
    }
}
