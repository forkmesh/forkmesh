package com.forkmesh.nativeapp.feature.health

import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.core.tween
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.api.PrefStore
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.tools.ToolsAppBar
import com.forkmesh.nativeapp.health.HealthApi
import com.forkmesh.nativeapp.health.SugarBreakDay
import com.forkmesh.nativeapp.health.SugarBreakHistoryRow
import com.forkmesh.nativeapp.health.SugarBreakModel
import com.forkmesh.nativeapp.health.SugarContract
import com.forkmesh.nativeapp.health.SugarDayStatus
import com.forkmesh.nativeapp.health.contract
import com.forkmesh.nativeapp.platform.AppLifecycle
import com.forkmesh.nativeapp.ui.kit.FmCard
import com.forkmesh.nativeapp.ui.kit.FmConfirmDialog
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenu
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenuItem
import com.forkmesh.nativeapp.ui.kit.FmEmptyState
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmInlineStatusLine
import com.forkmesh.nativeapp.ui.kit.FmModalBottomSheet
import com.forkmesh.nativeapp.ui.kit.FmPullToRefreshBox
import com.forkmesh.nativeapp.ui.kit.FmSectionHeader
import com.forkmesh.nativeapp.ui.kit.FmSkeletonList
import com.forkmesh.nativeapp.ui.kit.FmTextButton
import com.forkmesh.nativeapp.ui.kit.fmSafeAreaInsets
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmHaptics
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmRadius
import com.forkmesh.nativeapp.ui.theme.FmSpace
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

class SugarBreakScreen : Screen() {
    override val title: String get() = SugarBreakModel.TITLE

    @Composable
    override fun Content(navigator: Navigator) {
        val api = remember { Services.get<ApiService>() }
        if (api == null) {
            Column(Modifier.fillMaxSize().background(Fm.colors.canvas)) {
                ToolsAppBar(title, navigator)
                FmEmptyState("no_food_outlined", SugarBreakModel.UNAVAILABLE_TITLE)
            }
        } else {
            SugarBreakContent(api, title, navigator)
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun SugarBreakContent(api: ApiService, title: String, navigator: Navigator) {
    val model = remember { SugarBreakModel(HealthApi(api).sugarBreak(), Services.get<PrefStore>()) }
    val ticker = remember { ModelTicker() }
    DisposableEffect(model) {
        model.onChange = ticker::bump
        onDispose { model.onChange = null }
    }
    val sugar = ticker.track(model)
    val scope = rememberCoroutineScope()
    val foreground by AppLifecycle.foreground.collectAsState()
    var confirmEnd by remember { mutableStateOf(false) }
    var confirmUninstall by remember { mutableStateOf(false) }
    var deleting by remember { mutableStateOf<String?>(null) }
    var contract by remember { mutableStateOf<SugarContract?>(null) }

    fun run(block: () -> Unit) {
        scope.launch(Dispatchers.IO) { block() }
    }

    LaunchedEffect(foreground) {
        if (foreground) withContext(Dispatchers.IO) { sugar.load() }
    }

    Column(Modifier.fillMaxSize().background(Fm.colors.canvas)) {
        ToolsAppBar(title, navigator, actions = {
            FmIconButton("refresh", "Refresh", onPressed = { run { sugar.load() } })
        })
        Box(Modifier.weight(1f).fillMaxWidth()) {
            when {
                sugar.showsSpinner -> FmSkeletonList()
                sugar.showsError -> FmEmptyState("no_food_outlined", SugarBreakModel.UNAVAILABLE_TITLE, message = sugar.error)
                sugar.showsInstall -> InstallPanel(sugar, ::run)
                sugar.overview != null -> Dashboard(
                    sugar,
                    run = ::run,
                    onContract = { contract = sugar.contract() },
                    onEnd = { confirmEnd = true },
                    onDelete = { deleting = it },
                    onUninstall = { confirmUninstall = true },
                )
            }
        }
    }

    val sheet = contract
    if (sheet != null) {
        FmModalBottomSheet(onDismissRequest = { contract = null }, isScrollControlled = true) {
            SugarContractSheet(sheet)
        }
    }
    if (confirmEnd) {
        FmConfirmDialog(
            title = "End this sugar break early?",
            body = "Your days so far stay in your history. You can start a new break any time.",
            confirmLabel = "End break",
            destructive = true,
            onDismissRequest = { confirmEnd = false },
            onConfirm = {
                confirmEnd = false
                run { sugar.endEarly() }
            },
        )
    }
    val deletingId = deleting
    if (deletingId != null) {
        FmConfirmDialog(
            title = "Delete this sugar break?",
            body = "The break and every +1 in it are deleted from your account.",
            confirmLabel = "Delete",
            destructive = true,
            onDismissRequest = { deleting = null },
            onConfirm = {
                deleting = null
                run { sugar.deleteBreak(deletingId) }
            },
        )
    }
    if (confirmUninstall) {
        FmConfirmDialog(
            title = "Uninstall Sugar Break?",
            body = "Your breaks and every +1 are deleted from the web dashboard, the desktop app, and this app.",
            confirmLabel = "Uninstall",
            destructive = true,
            onDismissRequest = { confirmUninstall = false },
            onConfirm = {
                confirmUninstall = false
                run { if (sugar.uninstall()) removeHealthPackageFromAccountNow("sugar-break") }
            },
        )
    }
}

@Composable
private fun Notice(model: SugarBreakModel) {
    val notice = model.notice ?: return
    FmInlineStatusLine(notice.message, notice.isError, onDismiss = model::dismissNotice)
}

@Composable
private fun InstallPanel(model: SugarBreakModel, run: (() -> Unit) -> Unit) {
    Column(
        Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(FmSpace.x5),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.Center,
    ) {
        Text(SugarBreakModel.INSTALL_TITLE, textAlign = TextAlign.Center, style = Fm.text.titleMedium)
        Spacer(Modifier.height(FmSpace.x3))
        Text(
            SugarBreakModel.INSTALL_MESSAGE,
            textAlign = TextAlign.Center,
            style = Fm.text.bodyMedium.copy(color = Fm.colors.textSecondary),
        )
        Spacer(Modifier.height(FmSpace.x4))
        SfSolidButton(SugarBreakModel.INSTALL_BUTTON, Fm.colors.textPrimary, onClick = { run { model.install() } })
        Spacer(Modifier.height(FmSpace.x3))
        Notice(model)
    }
}

@Composable
private fun Dashboard(
    model: SugarBreakModel,
    run: (() -> Unit) -> Unit,
    onContract: () -> Unit,
    onEnd: () -> Unit,
    onDelete: (String) -> Unit,
    onUninstall: () -> Unit,
) {
    FmPullToRefreshBox(
        isRefreshing = model.loading && model.overview != null,
        onRefresh = { run { model.load() } },
        modifier = Modifier.fillMaxSize(),
    ) {
        Column(
            Modifier
                .fillMaxSize()
                .verticalScroll(rememberScrollState())
                .windowInsetsPadding(fmSafeAreaInsets(top = false))
                .padding(FmSpace.x4),
            verticalArrangement = Arrangement.spacedBy(FmSpace.x3),
        ) {
            if (model.isActive) {
                ProgressCard(model)
                SkipButton { run { model.skip() } }
                TodayCard(model, run)
                StatsCard(model)
            } else {
                if (model.currentBreak != null) ResultCard(model)
                StartCard(model, run)
            }
            Notice(model)
            SfOutlinedButton("Send a no-sugar contract", onClick = onContract, icon = "ios_share", modifier = Modifier.fillMaxWidth())
            val days = model.days
            if (days.isNotEmpty()) {
                FmSectionHeader("Days")
                DayGrid(days)
                DayLegend()
            }
            val history = model.history
            if (history.isNotEmpty()) {
                FmSectionHeader("Past breaks")
                History(history, onDelete)
            }
            Row(horizontalArrangement = Arrangement.spacedBy(FmSpace.x2), verticalAlignment = Alignment.CenterVertically) {
                if (model.isActive) {
                    SfOutlinedButton("End break early", onClick = if (model.busy) null else onEnd, icon = "stop_circle_outlined")
                }
                Spacer(Modifier.weight(1f))
                FmTextButton(onClick = onUninstall, contentColor = Fm.colors.danger) { Text("Uninstall") }
            }
        }
    }
}

@Composable
private fun ProgressCard(model: SugarBreakModel) {
    val c = Fm.colors
    val ratio by animateFloatAsState(model.progressRatio.toFloat(), tween(300), label = "sugarRing")
    FmCard(Modifier.fillMaxWidth()) {
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(FmSpace.x4)) {
            Box(
                Modifier.size(112.dp).semantics { contentDescription = model.progressLine },
                contentAlignment = Alignment.Center,
            ) {
                Canvas(Modifier.fillMaxSize()) {
                    val stroke = 10.dp.toPx()
                    val inset = stroke / 2
                    val arc = Size(size.width - stroke, size.height - stroke)
                    drawArc(c.border, 0f, 360f, false, Offset(inset, inset), arc, style = Stroke(stroke))
                    drawArc(c.success, -90f, 360f * ratio, false, Offset(inset, inset), arc, style = Stroke(stroke, cap = StrokeCap.Round))
                }
                Column(horizontalAlignment = Alignment.CenterHorizontally) {
                    Text(
                        model.sugarFreeDays.toString(),
                        style = Fm.text.titleLarge.copy(fontSize = 28.sp, fontWeight = FontWeight.W600, color = c.textPrimary),
                    )
                    Text("of ${model.lengthDays} days", style = Fm.text.bodySmall.copy(fontSize = 11.sp))
                }
            }
            Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                Text(model.headline, style = Fm.text.titleLarge)
                Text(model.progressLine, style = Fm.text.bodyMedium)
                Text(model.remainingLine, style = Fm.text.bodySmall)
            }
        }
    }
}

@Composable
private fun SkipButton(onSkip: () -> Unit) {
    val c = Fm.colors
    val haptics = FmHaptics.current
    val ink = onSolid(c.success)
    Column(
        Modifier
            .fillMaxWidth()
            .heightIn(min = 132.dp)
            .clip(RoundedCornerShape(FmRadius.lg))
            .background(c.success)
            .clickable(role = Role.Button, onClickLabel = "Plus one, I skipped sugar") {
                haptics.commit()
                onSkip()
            }
            .padding(FmSpace.x4),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.Center,
    ) {
        Text("+1", style = Fm.text.displaySmall.copy(fontSize = 44.sp, fontWeight = FontWeight.W700, color = ink))
        Spacer(Modifier.height(FmSpace.x1))
        Text("I skipped sugar", style = Fm.text.titleMedium.copy(color = ink, fontWeight = FontWeight.W600))
    }
}

@Composable
private fun TodayCard(model: SugarBreakModel, run: (() -> Unit) -> Unit) {
    val c = Fm.colors
    FmCard(Modifier.fillMaxWidth()) {
        Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text("Today", style = Fm.text.titleMedium, modifier = Modifier.weight(1f))
                Text("+${model.todaySkips}", style = Fm.text.titleMedium.copy(color = c.success))
            }
            Text(model.todayLine, style = Fm.text.bodyMedium.copy(color = c.textSecondary))
            SfOutlinedButton(
                "I had sugar",
                onClick = if (model.busy) null else ({ run { model.slip() } }),
                icon = "add",
                color = c.danger,
            )
            for (row in model.events) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Box(Modifier.size(8.dp).background(if (row.slip) c.danger else c.success, CircleShape))
                    Spacer(Modifier.width(FmSpace.x3))
                    Text(row.title, style = Fm.text.bodyMedium, modifier = Modifier.weight(1f))
                    Text(row.time, style = Fm.text.bodySmall)
                    FmIconButton("undo", "Undo", onPressed = { run { model.undo(row.id) } }, iconSize = 20.dp)
                }
            }
        }
    }
}

@Composable
private fun StatsCard(model: SugarBreakModel) {
    FmCard(Modifier.fillMaxWidth()) {
        Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x3)) {
            for (rowStats in model.stats.chunked(3)) {
                Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(FmSpace.x3)) {
                    for (stat in rowStats) {
                        Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(FmSpace.x1)) {
                            Text(
                                stat.label.uppercase(),
                                style = Fm.text.bodySmall.copy(fontSize = 11.sp, fontWeight = FontWeight.W600),
                            )
                            Text(stat.value, style = Fm.text.bodyMedium)
                        }
                    }
                    repeat(3 - rowStats.size) { Spacer(Modifier.weight(1f)) }
                }
            }
        }
    }
}

@Composable
private fun ResultCard(model: SugarBreakModel) {
    FmCard(Modifier.fillMaxWidth()) {
        Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
            Text(model.headline, style = Fm.text.titleMedium)
            Text(model.remainingLine, style = Fm.text.bodySmall)
            Text(model.resultLine, style = Fm.text.bodyMedium)
        }
    }
}

@Composable
private fun StartCard(model: SugarBreakModel, run: (() -> Unit) -> Unit) {
    val c = Fm.colors
    var open by remember { mutableStateOf(false) }
    FmCard(Modifier.fillMaxWidth()) {
        Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x3)) {
            Text(SugarBreakModel.START_TITLE, style = Fm.text.titleMedium)
            Text(SugarBreakModel.START_MESSAGE, style = Fm.text.bodyMedium.copy(color = c.textSecondary))
            Box {
                Row(
                    Modifier
                        .fillMaxWidth()
                        .heightIn(min = 44.dp)
                        .clickable(role = Role.DropdownList) { open = true },
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Text("Length", style = Fm.text.bodyLarge, modifier = Modifier.weight(1f))
                    val selected = model.lengths.firstOrNull { it.days == model.selectedLength }
                    Text(selected?.label ?: "", style = Fm.text.bodyLarge.copy(color = c.accent))
                    Spacer(Modifier.width(FmSpace.x1))
                    FmIcon("expand_more", size = 20.dp, tint = c.accent)
                }
                FmDropdownMenu(expanded = open, onDismissRequest = { open = false }) {
                    for (length in model.lengths) {
                        FmDropdownMenuItem(length.label, onClick = {
                            open = false
                            model.selectLength(length.days)
                        })
                    }
                }
            }
            SfSolidButton(
                "Start sugar break",
                c.success,
                onClick = if (model.busy) null else ({ run { model.startBreak() } }),
                icon = "play_arrow",
                minHeight = 52.dp,
                modifier = Modifier.fillMaxWidth(),
            )
        }
    }
}

@Composable
private fun DayGrid(days: List<SugarBreakDay>) {
    val c = Fm.colors
    Column(
        Modifier.semantics { contentDescription = "Days of this sugar break" },
        verticalArrangement = Arrangement.spacedBy(6.dp),
    ) {
        for (week in days.chunked(7)) {
            Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                for (day in week) {
                    val fill = when (day.status) {
                        SugarDayStatus.SugarFree -> c.success
                        SugarDayStatus.Slip -> c.danger
                        SugarDayStatus.Open -> c.border.copy(alpha = 0.6f)
                        SugarDayStatus.Upcoming -> Color.Transparent
                    }
                    val ink = when (day.status) {
                        SugarDayStatus.SugarFree, SugarDayStatus.Slip -> onSolid(c.success)
                        SugarDayStatus.Open -> c.textPrimary
                        SugarDayStatus.Upcoming -> c.textTertiary
                    }
                    val edge = when {
                        day.isToday -> c.accent
                        day.status == SugarDayStatus.Upcoming -> c.border
                        else -> Color.Transparent
                    }
                    val shape = RoundedCornerShape(8.dp)
                    Box(
                        Modifier
                            .weight(1f)
                            .heightIn(min = 40.dp)
                            .background(fill, shape)
                            .border(if (day.isToday) 2.dp else 1.dp, edge, shape)
                            .semantics { contentDescription = day.caption },
                        contentAlignment = Alignment.Center,
                    ) {
                        Text(
                            day.number.toString(),
                            style = Fm.text.bodySmall.copy(
                                fontSize = 13.sp,
                                fontWeight = if (day.isToday) FontWeight.W700 else FontWeight.W500,
                                color = ink,
                            ),
                        )
                    }
                }
                repeat(7 - week.size) { Spacer(Modifier.weight(1f)) }
            }
        }
    }
}

@Composable
private fun DayLegend() {
    val c = Fm.colors
    Row(horizontalArrangement = Arrangement.spacedBy(FmSpace.x3), verticalAlignment = Alignment.CenterVertically) {
        for ((color, label) in listOf(c.success to "Sugar-free", c.danger to "Had sugar", c.border.copy(alpha = 0.6f) to "Nothing logged")) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Box(Modifier.size(10.dp).background(color, RoundedCornerShape(3.dp)))
                Spacer(Modifier.width(FmSpace.x1))
                Text(label, style = Fm.text.bodySmall)
            }
        }
    }
}

@Composable
private fun History(rows: List<SugarBreakHistoryRow>, onDelete: (String) -> Unit) {
    Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
        for (row in rows) {
            Row(Modifier.padding(vertical = FmSpace.x1), verticalAlignment = Alignment.CenterVertically) {
                Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(2.dp)) {
                    Row(horizontalArrangement = Arrangement.spacedBy(FmSpace.x2), verticalAlignment = Alignment.CenterVertically) {
                        Text(row.title, style = Fm.text.bodyLarge)
                        Text(row.status, style = Fm.text.labelSmall.copy(color = Fm.colors.textSecondary))
                    }
                    Text(row.detail, style = Fm.text.bodySmall)
                }
                FmIconButton("delete_outline", "Delete", onPressed = { onDelete(row.id) })
            }
        }
    }
}
