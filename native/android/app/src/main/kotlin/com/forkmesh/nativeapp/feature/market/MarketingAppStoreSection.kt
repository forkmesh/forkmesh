package com.forkmesh.nativeapp.feature.market

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.gestures.detectHorizontalDragGestures
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.StrokeJoin
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.format.relativeTime
import com.forkmesh.nativeapp.market.AppStoreTone
import com.forkmesh.nativeapp.market.MarketingAppStore
import com.forkmesh.nativeapp.market.MarketingAppStoreDay
import com.forkmesh.nativeapp.market.MarketingAppStoreShare
import com.forkmesh.nativeapp.market.MarketingModel
import com.forkmesh.nativeapp.market.MarketingText
import com.forkmesh.nativeapp.ui.kit.FmCard
import com.forkmesh.nativeapp.ui.kit.FmCircularProgressIndicator
import com.forkmesh.nativeapp.ui.kit.FmKeyValueRow
import com.forkmesh.nativeapp.ui.kit.FmListRow
import com.forkmesh.nativeapp.ui.kit.FmStatusBadge
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import kotlin.math.roundToInt

private data class AppStoreSeries(val label: String, val color: Color, val value: (MarketingAppStoreDay) -> Long)

@Composable
private fun appStoreSeries(): List<AppStoreSeries> {
    val dark = Fm.colors.dark
    return listOf(
        AppStoreSeries("Downloads", if (dark) Color(0xFF3987E5) else Color(0xFF2A78D6)) { it.downloads },
        AppStoreSeries("Redownloads", if (dark) Color(0xFFD95926) else Color(0xFFEB6834)) { it.redownloads },
        AppStoreSeries("Updates", if (dark) Color(0xFF199E70) else Color(0xFF1BAF7A)) { it.updates },
    )
}

@Composable
internal fun MarketingAppStoreSection(model: MarketingModel, runner: MarketingRunner) {
    LaunchedEffect(model.organization) { runner.run({ loadAppStore() }) }
    var range by remember { mutableIntStateOf(model.appStoreRange) }
    val store = model.appStore
    val context = LocalContext.current
    MarketingHeader("App Store")
    if (store == null) {
        if (model.isWorking("app-store") || runner.busy > 0) Box(Modifier.fillMaxWidth().padding(24.dp), contentAlignment = Alignment.Center) { FmCircularProgressIndicator() }
        return
    }
    if (store.isEmpty) {
        Text(MarketingAppStore.EMPTY_TEXT, Modifier.padding(horizontal = 16.dp, vertical = 8.dp), style = Fm.text.bodySmall, color = Fm.colors.textSecondary)
    } else {
        Row(Modifier.padding(horizontal = 16.dp, vertical = 4.dp), horizontalArrangement = Arrangement.spacedBy(6.dp)) {
            MarketingAppStore.RANGES.forEach { days ->
                CrmChip("${days}D", Fm.colors.accent, filled = range == days, onTap = {
                    range = days
                    model.appStoreRange = days
                })
            }
        }
        val days = remember(store, range) { store.chartDays(range) }
        FmCard(Modifier.fillMaxWidth(), padding = PaddingValues(12.dp), margin = PaddingValues(horizontal = 16.dp, vertical = 4.dp)) {
            AppStoreChart(days)
        }
        AppStoreTiles(store, range)
        val window = store.window(range)
        if (window != null) {
            AppStoreShares("Top countries", window.territories, window.counts.downloads, window.territoryCount) { MarketingAppStore.countryName(it) }
            AppStoreShares("Devices", window.devices, window.counts.downloads, window.devices.size) { it }
        }
    }
    if (store.reviews.isNotEmpty()) {
        MarketingHeader("Reviews", store.reviews.size)
        store.reviews.take(10).forEach { review ->
            FmListRow(
                review.title.ifEmpty { "Review" },
                subtitleContent = {
                    Column {
                        Text(
                            listOf(review.starsText, review.territory, shortDate(review.createdAt)).filter { it.isNotEmpty() }.joinToString(" · "),
                            style = Fm.text.labelSmall, color = Fm.colors.textSecondary,
                        )
                        if (review.body.isNotEmpty()) Text(review.body, style = Fm.text.bodySmall, color = Fm.colors.textSecondary, maxLines = 4)
                    }
                },
                showChevron = false,
            )
        }
    }
    if (store.versions.isNotEmpty()) {
        MarketingHeader("Versions", store.versions.size)
        store.versions.forEach { version ->
            FmListRow(
                version.title, subtitle = shortDate(version.createdAt).ifEmpty { null },
                trailing = { FmStatusBadge(version.status.label, toneColor(version.status.tone)) }, showChevron = false,
            )
        }
    }
    val lookup = store.lookup
    if (!lookup.isEmpty) {
        MarketingHeader("Store listing")
        if (lookup.version.isNotEmpty()) FmKeyValueRow("Version", lookup.version, Modifier.padding(horizontal = 16.dp))
        if (lookup.releasedAt > 0) FmKeyValueRow("Released", shortDate(lookup.releasedAt), Modifier.padding(horizontal = 16.dp))
        if (lookup.genre.isNotEmpty()) FmKeyValueRow("Genre", lookup.genre, Modifier.padding(horizontal = 16.dp))
        if (lookup.price.isNotEmpty()) FmKeyValueRow("Price", lookup.price, Modifier.padding(horizontal = 16.dp))
        if (lookup.url.isNotEmpty()) FmListRow("Open in the App Store", leading = { FmIcon("open_in_new") }, onTap = { crmOpen(context, lookup.url) })
    }
    val sent = store.sentText(relativeTime(store.publishedAt))
    if (sent.isNotEmpty()) {
        Text(sent, Modifier.padding(horizontal = 16.dp, vertical = 4.dp), style = Fm.text.labelSmall, color = if (store.fresh) Fm.colors.textTertiary else MarketingTint.orange)
    }
}

@Composable
private fun toneColor(tone: AppStoreTone): Color = when (tone) {
    AppStoreTone.GOOD -> MarketingTint.green
    AppStoreTone.ACTION -> MarketingTint.orange
    AppStoreTone.PENDING -> Fm.colors.accent
    AppStoreTone.NEUTRAL -> Fm.colors.textTertiary
}

@Composable
private fun AppStoreTiles(store: MarketingAppStore, range: Int) {
    val window = store.window(range)
    val lookup = store.lookup
    val tiles = listOf(
        Triple("Downloads · ${range}D", MarketingText.count(window?.counts?.downloads ?: 0), window?.changeText.orEmpty().let { if (it.isEmpty()) "" else "$it vs previous ${range}D" }),
        Triple("Redownloads", MarketingText.count(window?.counts?.redownloads ?: 0), ""),
        Triple("Updates", MarketingText.count(window?.counts?.updates ?: 0), ""),
        Triple("Rating", lookup.ratingText, ""),
        Triple("Ratings", MarketingText.count(lookup.ratings), ""),
        Triple("All-time downloads", MarketingText.count(store.total.downloads), ""),
        Triple("Best day", store.bestDay?.let { MarketingText.count(it.downloads) } ?: "None yet", store.bestDay?.let { MarketingAppStore.dayLabelWithYear(it.day) }.orEmpty()),
    )
    val change = window?.downloadsChange
    Column(Modifier.padding(horizontal = 16.dp, vertical = 4.dp)) {
        tiles.chunked(2).forEach { row ->
            Row(Modifier.fillMaxWidth().padding(vertical = 4.dp), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                row.forEach { (label, value, note) ->
                    FmCard(Modifier.weight(1f), padding = PaddingValues(12.dp)) {
                        Column {
                            Text(label, style = Fm.text.labelSmall, color = Fm.colors.textTertiary, maxLines = 1)
                            Text(value, style = Fm.text.titleMedium, color = Fm.colors.textPrimary, maxLines = 1)
                            if (note.isNotEmpty()) {
                                val tint = when {
                                    label.startsWith("Downloads") && change != null && change >= 0.5 -> MarketingTint.green
                                    label.startsWith("Downloads") && change != null && change <= -0.5 -> MarketingTint.orange
                                    else -> Fm.colors.textTertiary
                                }
                                Text(note, style = Fm.text.labelSmall, color = tint, maxLines = 1)
                            }
                        }
                    }
                }
                if (row.size == 1) Spacer(Modifier.weight(1f))
            }
        }
    }
}

@Composable
private fun AppStoreShares(title: String, rows: List<MarketingAppStoreShare>, total: Long, count: Int, name: (String) -> String) {
    if (rows.isEmpty()) return
    MarketingHeader(title, count)
    rows.forEach { row ->
        val share = if (total > 0) " · " + MarketingText.percent(row.downloads * 100.0 / total) else ""
        FmListRow(name(row.label), subtitle = MarketingText.count(row.downloads) + " downloads" + share, showChevron = false, minHeight = 48.dp)
    }
}

@Composable
private fun AppStoreChart(days: List<MarketingAppStoreDay>) {
    val series = appStoreSeries()
    var selected by remember(days) { mutableStateOf<Int?>(null) }
    val grid = Fm.colors.border
    val cross = Fm.colors.textTertiary
    val surface = Fm.colors.bgRaised
    val top = remember(days) { MarketingAppStore.axisMax(days.maxOfOrNull { day -> maxOf(day.downloads, day.redownloads, day.updates) } ?: 0) }
    Column(verticalArrangement = Arrangement.spacedBy(6.dp)) {
        Row(horizontalArrangement = Arrangement.spacedBy(12.dp), verticalAlignment = Alignment.CenterVertically) {
            series.forEach { item ->
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Box(Modifier.size(width = 12.dp, height = 3.dp).background(item.color, RoundedCornerShape(2.dp)))
                    Spacer(Modifier.width(4.dp))
                    Text(item.label, style = Fm.text.labelSmall, color = Fm.colors.textSecondary)
                }
            }
        }
        val picked = selected?.let { days.getOrNull(it) }
        Text(
            if (picked != null) {
                MarketingAppStore.dayLabelWithYear(picked.day) + ": " + MarketingText.plural(picked.downloads.toInt(), "download", "downloads") + " · " +
                    MarketingText.plural(picked.redownloads.toInt(), "redownload", "redownloads") + " · " +
                    MarketingText.plural(picked.updates.toInt(), "update", "updates")
            } else {
                "Tap or drag across the chart to read a day."
            },
            style = Fm.text.labelSmall, color = if (picked != null) Fm.colors.textPrimary else Fm.colors.textTertiary, maxLines = 1,
        )
        Row {
            Box(Modifier.weight(1f)) {
                Canvas(
                    Modifier.fillMaxWidth().height(160.dp)
                        .pointerInput(days) {
                            detectTapGestures { offset -> selected = chartIndex(offset.x, size.width.toFloat(), days.size) }
                        }
                        .pointerInput(days) {
                            detectHorizontalDragGestures { change, _ -> selected = chartIndex(change.position.x, size.width.toFloat(), days.size) }
                        },
                ) {
                    val stroke = 1.dp.toPx()
                    for (step in 0..2) {
                        val y = size.height * step / 2f
                        drawLine(grid, Offset(0f, y), Offset(size.width, y), strokeWidth = stroke)
                    }
                    if (days.isEmpty()) return@Canvas
                    val step = if (days.size > 1) size.width / (days.size - 1) else 0f
                    fun x(index: Int): Float = if (days.size > 1) index * step else size.width / 2
                    fun y(value: Long): Float = size.height - (value.toFloat() / top) * size.height
                    series.asReversed().forEach { item ->
                        val path = Path()
                        days.forEachIndexed { index, day ->
                            if (index == 0) path.moveTo(x(index), y(item.value(day))) else path.lineTo(x(index), y(item.value(day)))
                        }
                        if (item === series.first()) {
                            val area = Path().apply {
                                addPath(path)
                                lineTo(x(days.lastIndex), size.height)
                                lineTo(x(0), size.height)
                                close()
                            }
                            drawPath(area, item.color.copy(alpha = 0.12f))
                        }
                        drawPath(path, item.color, style = Stroke(width = 2.dp.toPx(), cap = StrokeCap.Round, join = StrokeJoin.Round))
                    }
                    selected?.takeIf { it in days.indices }?.let { index ->
                        drawLine(cross, Offset(x(index), 0f), Offset(x(index), size.height), strokeWidth = stroke)
                        series.forEach { item ->
                            val center = Offset(x(index), y(item.value(days[index])))
                            drawCircle(surface, 6.dp.toPx(), center)
                            drawCircle(item.color, 4.dp.toPx(), center)
                        }
                    }
                }
            }
            Column(Modifier.height(160.dp).padding(start = 6.dp), verticalArrangement = Arrangement.SpaceBetween) {
                Text(MarketingText.count(top), style = Fm.text.labelSmall, color = Fm.colors.textTertiary)
                Text(MarketingText.count(top / 2), style = Fm.text.labelSmall, color = Fm.colors.textTertiary)
                Text("0", style = Fm.text.labelSmall, color = Fm.colors.textTertiary)
            }
        }
        if (days.isNotEmpty()) {
            Row(Modifier.fillMaxWidth()) {
                Text(MarketingAppStore.dayLabel(days.first().day), Modifier.weight(1f), style = Fm.text.labelSmall, color = Fm.colors.textTertiary)
                Text(MarketingAppStore.dayLabel(days.last().day), style = Fm.text.labelSmall, color = Fm.colors.textTertiary)
            }
        }
    }
}

private fun chartIndex(x: Float, width: Float, count: Int): Int? {
    if (count <= 0 || width <= 0f) return null
    if (count == 1) return 0
    return (x / width * (count - 1)).roundToInt().coerceIn(0, count - 1)
}
