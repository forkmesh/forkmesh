package com.forkmesh.nativeapp.feature.health

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
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
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.StrokeJoin
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.drawscope.clipPath
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.drawText
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.rememberTextMeasurer
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.api.PrefStore
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.account.AccountFilledButton
import com.forkmesh.nativeapp.feature.account.AccountOutlinedButton
import com.forkmesh.nativeapp.feature.tools.ToolsAppBar
import com.forkmesh.nativeapp.health.HealthApi
import com.forkmesh.nativeapp.health.WaterBar
import com.forkmesh.nativeapp.health.WaterEntryRow
import com.forkmesh.nativeapp.health.WaterModel
import com.forkmesh.nativeapp.health.WaterText
import com.forkmesh.nativeapp.ui.kit.FmCard
import com.forkmesh.nativeapp.ui.kit.FmConfirmDialog
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenu
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenuItem
import com.forkmesh.nativeapp.ui.kit.FmEmptyState
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmInlineStatusLine
import com.forkmesh.nativeapp.ui.kit.FmPullToRefreshBox
import com.forkmesh.nativeapp.ui.kit.FmSectionHeader
import com.forkmesh.nativeapp.ui.kit.FmSegmentTab
import com.forkmesh.nativeapp.ui.kit.FmSegmentTabBar
import com.forkmesh.nativeapp.ui.kit.FmSkeletonList
import com.forkmesh.nativeapp.ui.kit.FmTextButton
import com.forkmesh.nativeapp.ui.kit.FmTextField
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmSpace
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kotlin.math.max
import kotlin.math.min

class WaterScreen : Screen() {
    override val title: String get() = "Water"

    private val model: WaterModel? by lazy {
        Services.get<ApiService>()?.let { WaterModel(HealthApi(it).water(), Services.get<PrefStore>()) }
    }

    private var rev by mutableIntStateOf(0)

    @Composable
    override fun Content(navigator: Navigator) {
        val model = model
        val scope = rememberCoroutineScope()
        var refreshing by remember { mutableStateOf(false) }
        fun act(block: (WaterModel) -> Unit) {
            val current = model ?: return
            scope.launch {
                withContext(Dispatchers.IO) { block(current) }
                rev++
            }
        }
        LaunchedEffect(Unit) {
            val current = model ?: return@LaunchedEffect
            withContext(Dispatchers.IO) { current.load() }
            rev++
        }
        Column(Modifier.fillMaxSize().background(Fm.colors.canvas)) {
            ToolsAppBar(title, navigator) {
                FmIconButton(icon = "refresh", label = "Refresh", onPressed = { act { it.load() } })
            }
            if (model == null) {
                FmEmptyState(icon = "water_drop_outlined", title = WaterModel.UNAVAILABLE_TITLE)
            } else {
                WaterBody(model, rev, refreshing, { value -> refreshing = value }, ::act)
            }
        }
    }
}

@Composable
private fun WaterBody(
    model: WaterModel,
    rev: Int,
    refreshing: Boolean,
    setRefreshing: (Boolean) -> Unit,
    act: ((WaterModel) -> Unit) -> Unit,
) {
    val scope = rememberCoroutineScope()
    var adding by remember { mutableStateOf(false) }
    var editingGoal by remember { mutableStateOf(false) }
    var confirmUninstall by remember { mutableStateOf(false) }
    when {
        model.showsSpinner -> FmSkeletonList()
        model.showsError -> FmEmptyState(icon = "water_drop_outlined", title = WaterModel.UNAVAILABLE_TITLE, message = model.error)
        model.showsInstall -> WaterInstall(model, rev, act)
        model.overview != null -> FmPullToRefreshBox(
            isRefreshing = refreshing,
            onRefresh = {
                setRefreshing(true)
                scope.launch {
                    withContext(Dispatchers.IO) { model.load() }
                    setRefreshing(false)
                    act { }
                }
            },
            modifier = Modifier.fillMaxSize(),
        ) {
            WaterDashboard(model, rev, act, { adding = true }, { editingGoal = true }, { confirmUninstall = true })
        }
    }
    if (adding) {
        AddDrinkDialog(model, onDismiss = { adding = false }) { text, drink ->
            adding = false
            act { it.addTyped(text, drink) }
        }
    }
    if (editingGoal) {
        GoalDialog(model, onDismiss = { editingGoal = false }) { goal, units ->
            editingGoal = false
            act { it.saveGoal(goal, units) }
        }
    }
    if (confirmUninstall) {
        FmConfirmDialog(
            title = "Uninstall Water Intake?",
            body = "Your drinks log is deleted from the web dashboard, the desktop app, and this app.",
            confirmLabel = "Uninstall",
            destructive = true,
            onDismissRequest = { confirmUninstall = false },
            onConfirm = {
                confirmUninstall = false
                act { if (it.uninstall()) removeHealthPackageFromAccountNow("water") }
            },
        )
    }
}

@Composable
private fun WaterInstall(model: WaterModel, rev: Int, act: ((WaterModel) -> Unit) -> Unit) {
    Column(
        Modifier.fillMaxSize().padding(FmSpace.x5).verticalScroll(rememberScrollState()),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.Center,
    ) {
        Text(WaterModel.INSTALL_TITLE, style = Fm.text.titleMedium, textAlign = TextAlign.Center)
        Spacer(Modifier.height(FmSpace.x3))
        Text(
            WaterModel.INSTALL_MESSAGE,
            style = Fm.text.bodyMedium.copy(color = Fm.colors.textSecondary),
            textAlign = TextAlign.Center,
        )
        Spacer(Modifier.height(FmSpace.x4))
        AccountFilledButton(WaterModel.INSTALL_BUTTON, "water_drop_outlined", onClick = { act { it.install() } })
        WaterNotice(model, rev)
    }
}

@Composable
private fun WaterNotice(model: WaterModel, rev: Int) {
    var dismissed by remember(rev) { mutableStateOf(false) }
    val notice = model.notice
    if (notice != null && !dismissed) {
        FmInlineStatusLine(
            message = notice.message,
            isError = notice.isError,
            modifier = Modifier.padding(top = FmSpace.x2),
            onDismiss = {
                model.notice = null
                dismissed = true
            },
        )
    }
}

@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun WaterDashboard(
    model: WaterModel,
    rev: Int,
    act: ((WaterModel) -> Unit) -> Unit,
    onOther: () -> Unit,
    onGoal: () -> Unit,
    onUninstall: () -> Unit,
) {
    val c = Fm.colors
    Column(
        Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(FmSpace.x4),
        verticalArrangement = Arrangement.spacedBy(FmSpace.x3),
    ) {
        FmCard {
            Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(FmSpace.x4)) {
                WaterGlass(model.ratio, if (model.goalMet) c.success else c.accent, c.border, Modifier.size(width = 84.dp, height = 116.dp))
                Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                    Text(model.totalText, style = Fm.text.titleLarge.copy(fontSize = 28.sp, fontWeight = FontWeight.SemiBold))
                    Text(model.goalLine, style = Fm.text.bodyMedium.copy(color = c.textSecondary))
                }
            }
        }
        FlowRow(horizontalArrangement = Arrangement.spacedBy(FmSpace.x2), verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
            for (preset in model.presets) {
                AccountFilledButton(
                    "+ ${preset.label}", "water_drop_outlined",
                    onClick = { act { it.add(preset.milliliters) } },
                    enabled = !model.busy,
                )
            }
            AccountOutlinedButton("Other drink", onClick = onOther, icon = "add")
        }
        WaterNotice(model, rev)
        FmSectionHeader(title = "Today")
        val rows = model.rows
        if (rows.isEmpty()) {
            Text(
                WaterModel.EMPTY_LOG,
                modifier = Modifier.fillMaxWidth().padding(FmSpace.x3),
                style = Fm.text.bodyMedium.copy(color = c.textSecondary),
                textAlign = TextAlign.Center,
            )
        }
        for (row in rows) WaterEntry(row) { act { it.delete(row.id) } }
        FmSectionHeader(title = "Last 30 days")
        WaterBars(model.bars, c.accent, c.success, c.textSecondary, Modifier.fillMaxWidth().height(140.dp))
        Text(model.summaryLine, style = Fm.text.bodySmall.copy(color = c.textSecondary))
        Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
            AccountOutlinedButton("Goal", onClick = onGoal, icon = "flag_outlined")
            Spacer(Modifier.weight(1f))
            FmTextButton(onClick = onUninstall, contentColor = c.danger) { Text("Uninstall") }
        }
    }
}

@Composable
private fun WaterEntry(row: WaterEntryRow, onDelete: () -> Unit) {
    val c = Fm.colors
    Row(
        Modifier.fillMaxWidth().padding(vertical = FmSpace.x1),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(FmSpace.x3),
    ) {
        Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(2.dp)) {
            Text(row.title, style = Fm.text.bodyLarge)
            if (row.subtitle.isNotEmpty()) Text(row.subtitle, style = Fm.text.bodySmall.copy(color = c.textSecondary))
        }
        Text(row.amount, style = Fm.text.bodyMedium)
        FmIconButton(icon = "delete_outline", label = "Delete", onPressed = onDelete)
    }
}

@Composable
private fun AddDrinkDialog(model: WaterModel, onDismiss: () -> Unit, onLog: (String, String) -> Unit) {
    var amount by remember { mutableStateOf("") }
    var drink by remember { mutableStateOf("water") }
    var menu by remember { mutableStateOf(false) }
    FmConfirmDialog(
        title = "Log a drink",
        confirmLabel = "Log",
        onDismissRequest = onDismiss,
        onConfirm = { onLog(amount, drink) },
    ) {
        Spacer(Modifier.height(FmSpace.x2))
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
            Box(Modifier.weight(1f)) {
                FmTextField(
                    value = amount,
                    onValueChange = { amount = it },
                    hint = model.amountHint,
                    autofocus = true,
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Decimal),
                )
            }
            Text(model.unit, style = Fm.text.bodyMedium.copy(color = Fm.colors.textSecondary))
        }
        Spacer(Modifier.height(FmSpace.x2))
        Box {
            Row(
                Modifier.clickable { menu = true }.padding(vertical = FmSpace.x2),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(FmSpace.x2),
            ) {
                Text("Drink", style = Fm.text.bodyMedium.copy(color = Fm.colors.textSecondary))
                Text(WaterText.drinkLabel(drink), style = Fm.text.bodyLarge.copy(color = Fm.colors.accent))
                FmIcon("arrow_drop_down", size = 20.dp, tint = Fm.colors.accent)
            }
            FmDropdownMenu(expanded = menu, onDismissRequest = { menu = false }) {
                for (option in WaterModel.DRINKS) {
                    FmDropdownMenuItem(text = WaterText.drinkLabel(option), onClick = {
                        drink = option
                        menu = false
                    })
                }
            }
        }
    }
}

private val goalUnitValues = listOf("auto", "metric", "imperial", "cups")
private val goalUnitTabs = listOf(FmSegmentTab("Automatic"), FmSegmentTab("ml"), FmSegmentTab("fl oz"), FmSegmentTab("cups"))

@Composable
private fun GoalDialog(model: WaterModel, onDismiss: () -> Unit, onSave: (Int, String) -> Unit) {
    var goal by remember { mutableStateOf(model.goalMl.toString()) }
    var units by remember { mutableStateOf(model.unitsPreference) }
    FmConfirmDialog(
        title = "Daily water goal (ml)",
        confirmLabel = "Save",
        onDismissRequest = onDismiss,
        onConfirm = { onSave(goal.trim().toIntOrNull() ?: 0, units) },
    ) {
        Spacer(Modifier.height(FmSpace.x2))
        FmTextField(
            value = goal,
            onValueChange = { goal = it.filter { ch -> ch.isDigit() } },
            hint = "2000",
            autofocus = true,
            keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
        )
        Spacer(Modifier.height(FmSpace.x2))
        FmSegmentTabBar(
            tabs = goalUnitTabs,
            index = max(0, goalUnitValues.indexOf(units)),
            onChanged = { units = goalUnitValues[it] },
        )
    }
}

@Composable
private fun WaterGlass(ratio: Double, fill: Color, outline: Color, modifier: Modifier = Modifier) {
    Canvas(modifier) {
        val inset = 4.dp.toPx()
        val slant = 12.dp.toPx()
        val glass = Path().apply {
            moveTo(inset, inset)
            lineTo(size.width - inset, inset)
            lineTo(size.width - slant, size.height - inset)
            lineTo(slant, size.height - inset)
            close()
        }
        val level = inset + (size.height - 2 * inset) * (1 - min(1.0, max(0.0, ratio))).toFloat()
        clipPath(glass) {
            drawRect(fill.copy(alpha = 0.85f), topLeft = Offset(0f, level), size = Size(size.width, size.height - level))
        }
        drawPath(glass, outline, style = Stroke(width = 3.dp.toPx(), join = StrokeJoin.Round))
    }
}

@Composable
private fun WaterBars(bars: List<WaterBar>, accent: Color, met: Color, ink: Color, modifier: Modifier = Modifier) {
    val measurer = rememberTextMeasurer()
    val label = TextStyle(fontSize = 9.sp, color = ink)
    Canvas(modifier) {
        if (bars.isEmpty()) return@Canvas
        val plotHeight = size.height - 16.dp.toPx()
        val slot = size.width / bars.size
        val width = max(2.dp.toPx(), min(12.dp.toPx(), slot - 3.dp.toPx()))
        bars.forEachIndexed { index, bar ->
            val height = if (bar.ratio > 0) max(2.dp.toPx(), plotHeight * min(1.0, bar.ratio).toFloat()) else 0f
            val x = index * slot + (slot - width) / 2
            drawRoundRect(
                if (bar.metGoal) met else accent,
                topLeft = Offset(x, plotHeight - height),
                size = Size(width, height),
                cornerRadius = CornerRadius(2.dp.toPx()),
            )
            if (bar.label.isNotEmpty()) {
                val measured = measurer.measure(bar.label, label)
                val left = min(max(0f, x + width / 2 - measured.size.width / 2f), size.width - measured.size.width)
                drawText(measured, topLeft = Offset(left, size.height - measured.size.height))
            }
        }
    }
}
