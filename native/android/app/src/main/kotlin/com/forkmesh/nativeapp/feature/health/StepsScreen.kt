package com.forkmesh.nativeapp.feature.health

import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
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
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.api.PrefStore
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.account.AccountOutlinedButton
import com.forkmesh.nativeapp.feature.tools.ToolsAppBar
import com.forkmesh.nativeapp.feature.tools.observe
import com.forkmesh.nativeapp.health.HealthApi
import com.forkmesh.nativeapp.health.HeartTapModel
import com.forkmesh.nativeapp.health.StepsModel
import com.forkmesh.nativeapp.platform.AppLifecycle
import com.forkmesh.nativeapp.ui.kit.FmAlertDialog
import com.forkmesh.nativeapp.ui.kit.FmCard
import com.forkmesh.nativeapp.ui.kit.FmCircularProgressIndicator
import com.forkmesh.nativeapp.ui.kit.FmConfirmDialog
import com.forkmesh.nativeapp.ui.kit.FmEmptyState
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmInlineStatusLine
import com.forkmesh.nativeapp.ui.kit.FmPullToRefreshBox
import com.forkmesh.nativeapp.ui.kit.FmSectionHeader
import com.forkmesh.nativeapp.ui.kit.FmSegmentTab
import com.forkmesh.nativeapp.ui.kit.FmSegmentTabBar
import com.forkmesh.nativeapp.ui.kit.FmSwitch
import com.forkmesh.nativeapp.ui.kit.FmTextButton
import com.forkmesh.nativeapp.ui.kit.FmTextField
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmSpace
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.time.LocalDate

class StepsScreen : Screen() {
    override val title: String get() = StepsModel.TITLE

    @Composable
    override fun Content(navigator: Navigator) {
        val api = Services.get<ApiService>()
        val prefs = Services.get<PrefStore>()
        Column(Modifier.fillMaxSize().background(Fm.colors.canvas)) {
            if (api == null || prefs == null) {
                ToolsAppBar(StepsModel.TITLE, navigator)
                FmEmptyState("directions_walk_outlined", StepsModel.UNAVAILABLE_TITLE)
            } else {
                StepsContent(navigator, api, prefs)
            }
        }
    }
}

@Composable
private fun StepsContent(navigator: Navigator, api: ApiService, prefs: PrefStore) {
    val model = remember { StepsModel(HealthApi(api).steps(), StepsSensorBridge(), prefs) }
    val heart = remember { HeartTapModel() }
    model.observe()
    heart.observe()
    val scope = rememberCoroutineScope()
    val foreground by AppLifecycle.foreground.collectAsState()
    var adding by remember { mutableStateOf(false) }
    var editingGoal by remember { mutableStateOf(false) }
    var confirmUninstall by remember { mutableStateOf(false) }
    var refreshing by remember { mutableStateOf(false) }
    var pendingInstall by remember { mutableStateOf(false) }
    var permissionNote by remember { mutableStateOf<String?>(null) }

    fun io(block: () -> Unit) {
        scope.launch(Dispatchers.IO) { block() }
    }

    val permission = rememberLauncherForActivityResult(ActivityResultContracts.RequestPermission()) { granted ->
        val install = pendingInstall
        pendingInstall = false
        if (install) {
            io { model.install() }
        } else if (granted) {
            io { model.enableHealth() }
        } else {
            permissionNote = "ForkMesh was not allowed to count your steps. Allow Physical activity for ForkMesh in your phone's app settings."
        }
    }

    LaunchedEffect(model, foreground) {
        if (foreground) withContext(Dispatchers.IO) { model.load() }
    }
    LaunchedEffect(model) {
        while (true) {
            delay(1000)
            model.tick()
        }
    }
    DisposableEffect(model, foreground, model.healthEnabled) {
        if (foreground) model.startLive() else model.stopLive()
        onDispose { }
    }
    LaunchedEffect(model.healthEnabled) { HealthFeature.stepsEnabledChanged() }
    DisposableEffect(model) {
        onDispose { AppLifecycle.scope.launch(Dispatchers.IO) { model.leave() } }
    }

    ToolsAppBar(StepsModel.TITLE, navigator) {
        FmIconButton("refresh", "Refresh", onPressed = { io { model.load() } }, color = Fm.colors.textPrimary)
    }
    when {
        model.showsSpinner -> Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) { FmCircularProgressIndicator() }
        model.showsError -> FmEmptyState("directions_walk_outlined", StepsModel.UNAVAILABLE_TITLE, message = model.error)
        model.showsInstall -> HealthInstallPanel(
            StepsModel.INSTALL_TITLE, StepsModel.INSTALL_MESSAGE, StepsModel.INSTALL_BUTTON, model.notice, model::dismissNotice,
        ) {
            if (StepsSensors.needsPermission && StepsSensors.stepSensorAvailable) {
                pendingInstall = true
                permission.launch(StepsSensors.permissionName)
            } else {
                io { model.install() }
            }
        }
        model.overview != null -> FmPullToRefreshBox(
            isRefreshing = refreshing,
            onRefresh = {
                scope.launch {
                    refreshing = true
                    withContext(Dispatchers.IO) { model.syncNow(7) }
                    refreshing = false
                }
            },
            modifier = Modifier.fillMaxSize(),
        ) {
            Column(
                Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(FmSpace.x4),
                verticalArrangement = Arrangement.spacedBy(FmSpace.x3),
            ) {
                TodayCard(model)
                WalkCard(model)
                CountingCard(model, permissionNote, onDismissNote = { permissionNote = null }, onToggle = { on ->
                    if (!on) {
                        model.disableHealth()
                    } else if (StepsSensors.needsPermission) {
                        permission.launch(StepsSensors.permissionName)
                    } else {
                        io { model.enableHealth() }
                    }
                }, onSync = { io { model.syncNow(30) } })
                HealthNoticeLine(model.notice, model::dismissNotice)
                HeartCard(heart)
                HealthStatsCard(model.stats.map { it.label to it.value })
                FmSectionHeader("Last 30 days")
                HealthBarsChart(model.bars.map { HealthChartBar(it.label, it.ratio, it.steps > 0, it.metGoal) }, model.goalRatio)
                Row(horizontalArrangement = Arrangement.spacedBy(FmSpace.x2), verticalAlignment = Alignment.CenterVertically) {
                    AccountOutlinedButton("Add steps", { adding = true }, icon = "add")
                    AccountOutlinedButton("Goal", { editingGoal = true }, icon = "flag_outlined")
                    Spacer(Modifier.weight(1f))
                    FmTextButton(onClick = { confirmUninstall = true }, contentColor = Fm.colors.danger) { Text("Uninstall") }
                }
            }
        }
    }
    if (adding) {
        AddStepsDialog(
            model,
            onDismiss = { adding = false },
            onRemove = { day -> adding = false; io { model.removeAddedSteps(day) } },
            onSave = { day, steps, distance -> adding = false; io { model.addSteps(day, steps, distance) } },
        )
    }
    if (editingGoal) {
        GoalDialog(
            model,
            onDismiss = { editingGoal = false },
            onSave = { goal, units -> editingGoal = false; io { model.saveGoal(goal, units) } },
        )
    }
    if (confirmUninstall) {
        FmConfirmDialog(
            title = "Uninstall Pedometer?",
            body = "Your step history is deleted from the web dashboard, the desktop app, and this app.",
            confirmLabel = "Uninstall",
            destructive = true,
            onDismissRequest = { confirmUninstall = false },
            onConfirm = {
                confirmUninstall = false
                scope.launch {
                    val removed = withContext(Dispatchers.IO) { model.uninstall() }
                    if (removed) removeHealthPackageFromAccount("steps")
                }
            },
        )
    }
}

@Composable
private fun TodayCard(model: StepsModel) {
    val c = Fm.colors
    FmCard {
        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(FmSpace.x4), verticalAlignment = Alignment.CenterVertically) {
            HealthRing(model.ringRatio, model.goalMet) {
                Column(horizontalAlignment = Alignment.CenterHorizontally) {
                    Text(model.stepsText, style = TextStyle(fontSize = 24.sp, fontWeight = FontWeight.SemiBold), color = c.textPrimary)
                    Text(model.goalLine, style = TextStyle(fontSize = 11.sp), color = c.textSecondary)
                }
            }
            Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                Text("Today", style = Fm.text.titleMedium, color = c.textPrimary)
                Text(model.detailLine, style = Fm.text.bodyMedium, color = c.textSecondary)
            }
        }
    }
}

@Composable
private fun WalkCard(model: StepsModel) {
    val c = Fm.colors
    FmCard {
        Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
            StepsWalkScene(
                model.cadence, model.running, if (model.goalMet) c.success else c.accent, c.textTertiary, c.border,
                Modifier.fillMaxWidth().height(96.dp),
            )
            Text(model.paceLine, style = Fm.text.bodyMedium, color = c.textSecondary)
        }
    }
}

@Composable
private fun CountingCard(
    model: StepsModel,
    permissionNote: String?,
    onDismissNote: () -> Unit,
    onToggle: (Boolean) -> Unit,
    onSync: () -> Unit,
) {
    val c = Fm.colors
    FmCard {
        Column(Modifier.fillMaxWidth(), verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text("Count steps with the step sensor", Modifier.weight(1f), style = Fm.text.bodyLarge, color = c.textPrimary)
                FmSwitch(model.healthEnabled, onToggle, enabled = model.healthAvailable)
            }
            Text(model.sourceLine, style = Fm.text.bodySmall, color = c.textSecondary)
            FmInlineStatusLine(permissionNote, true, onDismiss = onDismissNote)
            if (model.healthEnabled) {
                FmTextButton(onClick = onSync, enabled = !model.syncing) { Text(if (model.syncing) "Syncing…" else "Sync now") }
            }
            Text(
                "Steps come from your phone's built-in step counter, so only steps taken while counting is on are included. Distance is estimated from your stride. Nothing leaves the phone except one total per day sent to your ForkMesh account.",
                style = Fm.text.bodySmall, color = c.textTertiary,
            )
        }
    }
}

@Composable
private fun HeartCard(heart: HeartTapModel) {
    val c = Fm.colors
    FmCard {
        Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x3)) {
            Row(horizontalArrangement = Arrangement.spacedBy(FmSpace.x4), verticalAlignment = Alignment.CenterVertically) {
                Box(
                    Modifier.size(88.dp).clip(CircleShape).background(c.danger).clickable(onClickLabel = HeartTapModel.TAP_LABEL) { heart.tap() },
                    contentAlignment = Alignment.Center,
                ) { FmIcon("favorite", size = 34.dp, tint = Color.White) }
                Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                    Text(HeartTapModel.TITLE, style = Fm.text.titleMedium, color = c.textPrimary)
                    Text(heart.line, style = Fm.text.bodyMedium, color = c.textSecondary)
                }
            }
            if (heart.canReset) {
                Row { FmTextButton(onClick = { heart.reset() }) { Text("Reset") } }
            }
        }
    }
}

@Composable
private fun AddStepsDialog(
    model: StepsModel,
    onDismiss: () -> Unit,
    onRemove: (String) -> Unit,
    onSave: (String, Int, Double) -> Unit,
) {
    var day by remember { mutableStateOf(LocalDate.now()) }
    var steps by remember { mutableStateOf("") }
    var distance by remember { mutableStateOf("") }
    var picking by remember { mutableStateOf(false) }
    FmAlertDialog(
        onDismissRequest = onDismiss,
        title = "Add steps",
        actions = {
            FmTextButton(onClick = { onRemove(day.toString()) }) { Text("Remove added") }
            Spacer(Modifier.weight(1f))
            FmTextButton(onClick = onDismiss) { Text("Cancel") }
            FmTextButton(onClick = {
                onSave(day.toString(), steps.trim().toDoubleOrNull()?.toInt() ?: 0, distance.replace(',', '.').trim().toDoubleOrNull() ?: 0.0)
            }) { Text("Save") }
        },
    ) {
        Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
            Text(
                "For a walk your phone missed. These count on top of the step sensor's total for the day.",
                style = Fm.text.bodyMedium, color = Fm.colors.textSecondary,
            )
            AccountOutlinedButton(model.dayLabel(day.toString()), { picking = true }, icon = "calendar_today_outlined")
            FmTextField(steps, { steps = it }, hint = "Steps", autofocus = true, keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number))
            FmTextField(
                distance, { distance = it }, hint = "Distance (${model.distanceUnit}, optional)",
                keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Decimal),
            )
        }
    }
    if (picking) {
        HealthDayPickerDialog(day, onDismiss = { picking = false }) {
            day = it
            picking = false
        }
    }
}

@Composable
private fun GoalDialog(model: StepsModel, onDismiss: () -> Unit, onSave: (Int, String) -> Unit) {
    var goal by remember { mutableStateOf(model.goal.toString()) }
    val units = listOf("auto", "metric", "imperial")
    var unit by remember { mutableStateOf(units.indexOf(model.unitsPreference).coerceAtLeast(0)) }
    FmAlertDialog(
        onDismissRequest = onDismiss,
        title = "Daily step goal",
        actions = {
            Spacer(Modifier.weight(1f))
            FmTextButton(onClick = onDismiss) { Text("Cancel") }
            FmTextButton(onClick = { onSave(goal.trim().toDoubleOrNull()?.toInt() ?: 0, units[unit]) }) { Text("Save") }
        },
    ) {
        Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
            FmTextField(goal, { goal = it }, hint = "10000", autofocus = true, keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number))
            Text("Distance in", style = Fm.text.bodySmall, color = Fm.colors.textSecondary)
            FmSegmentTabBar(listOf(FmSegmentTab("Automatic"), FmSegmentTab("Kilometres"), FmSegmentTab("Miles")), unit, { unit = it })
        }
    }
}
