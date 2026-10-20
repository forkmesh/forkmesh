package com.forkmesh.nativeapp.feature.health

import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.sp
import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.api.PrefStore
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.account.AccountOutlinedButton
import com.forkmesh.nativeapp.feature.tools.ToolsAppBar
import com.forkmesh.nativeapp.feature.tools.observe
import com.forkmesh.nativeapp.health.HealthApi
import com.forkmesh.nativeapp.health.SleepModel
import com.forkmesh.nativeapp.health.SleepSchedule
import com.forkmesh.nativeapp.health.SleepText
import com.forkmesh.nativeapp.health.StepsText
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
import com.forkmesh.nativeapp.ui.kit.FmSwitch
import com.forkmesh.nativeapp.ui.kit.FmTextButton
import com.forkmesh.nativeapp.ui.kit.FmTextField
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmSpace
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.time.LocalDate
import kotlin.math.roundToInt

class SleepScreen : Screen() {
    override val title: String get() = SleepModel.TITLE

    @Composable
    override fun Content(navigator: Navigator) {
        val api = Services.get<ApiService>()
        val prefs = Services.get<PrefStore>()
        Column(Modifier.fillMaxSize().background(Fm.colors.canvas)) {
            if (api == null || prefs == null) {
                ToolsAppBar(SleepModel.TITLE, navigator)
                FmEmptyState("dark_mode_outlined", SleepModel.UNAVAILABLE_TITLE)
            } else {
                SleepContent(navigator, api, prefs)
            }
        }
    }
}

@Composable
private fun SleepContent(navigator: Navigator, api: ApiService, prefs: PrefStore) {
    val model = remember { SleepModel(HealthApi(api).sleep(), SleepSensorBridge(), prefs) }
    model.observe()
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
            io { model.enableTracking() }
        } else {
            permissionNote = "ForkMesh was not allowed to count your steps. Allow Physical activity for ForkMesh in your phone's app settings."
        }
    }

    LaunchedEffect(model, foreground) {
        if (foreground) withContext(Dispatchers.IO) { model.load() }
    }
    LaunchedEffect(model.trackingEnabled) { HealthFeature.sleepEnabledChanged() }

    ToolsAppBar(SleepModel.TITLE, navigator) {
        FmIconButton("refresh", "Refresh", onPressed = { io { model.load() } }, color = Fm.colors.textPrimary)
    }
    when {
        model.showsSpinner -> Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) { FmCircularProgressIndicator() }
        model.showsError -> FmEmptyState("dark_mode_outlined", SleepModel.UNAVAILABLE_TITLE, message = model.error)
        model.showsInstall -> HealthInstallPanel(
            SleepModel.INSTALL_TITLE, SleepModel.INSTALL_MESSAGE, SleepModel.INSTALL_BUTTON, model.notice, model::dismissNotice,
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
                    withContext(Dispatchers.IO) { model.syncNow() }
                    refreshing = false
                }
            },
            modifier = Modifier.fillMaxSize(),
        ) {
            Column(
                Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(FmSpace.x4),
                verticalArrangement = Arrangement.spacedBy(FmSpace.x3),
            ) {
                LastNightCard(model)
                TrackingCard(model, permissionNote, onDismissNote = { permissionNote = null }, onToggle = { on ->
                    if (!on) {
                        model.disableTracking()
                    } else if (StepsSensors.needsPermission) {
                        permission.launch(StepsSensors.permissionName)
                    } else {
                        io { model.enableTracking() }
                    }
                }, onSync = { io { model.syncNow() } })
                HealthNoticeLine(model.notice, model::dismissNotice)
                HealthStatsCard(model.stats.map { it.label to it.value })
                FmSectionHeader("Last 30 nights")
                HealthBarsChart(model.bars.map { HealthChartBar(it.label, it.ratio, it.minutes > 0, it.metGoal) }, model.goalRatio)
                Row(horizontalArrangement = Arrangement.spacedBy(FmSpace.x2), verticalAlignment = Alignment.CenterVertically) {
                    AccountOutlinedButton("Add night", { adding = true }, icon = "add")
                    AccountOutlinedButton("Goal", { editingGoal = true }, icon = "flag_outlined")
                    Spacer(Modifier.weight(1f))
                    FmTextButton(onClick = { confirmUninstall = true }, contentColor = Fm.colors.danger) { Text("Uninstall") }
                }
            }
        }
    }
    if (adding) {
        AddNightDialog(
            model,
            onDismiss = { adding = false },
            onRemove = { night -> adding = false; io { model.removeAddedNight(night) } },
            onSave = { night, bed, wake, awake ->
                adding = false
                val span = model.span(night, bed, wake)
                if (span != null) io { model.addNight(night, span.first, span.second, awake) }
            },
        )
    }
    if (editingGoal) {
        SleepGoalDialog(
            model,
            onDismiss = { editingGoal = false },
            onSave = { minutes, bedtime, wakeTime -> editingGoal = false; io { model.saveGoal(minutes, bedtime, wakeTime) } },
        )
    }
    if (confirmUninstall) {
        FmConfirmDialog(
            title = "Uninstall Sleep?",
            body = "Your nights are deleted from the web dashboard, the desktop app, and this app.",
            confirmLabel = "Uninstall",
            destructive = true,
            onDismissRequest = { confirmUninstall = false },
            onConfirm = {
                confirmUninstall = false
                scope.launch {
                    val removed = withContext(Dispatchers.IO) { model.uninstall() }
                    if (removed) removeHealthPackageFromAccount("sleep")
                }
            },
        )
    }
}

@Composable
private fun LastNightCard(model: SleepModel) {
    val c = Fm.colors
    FmCard {
        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(FmSpace.x4), verticalAlignment = Alignment.CenterVertically) {
            HealthRing(model.ringRatio, model.goalMet) {
                Column(Modifier.padding(horizontal = FmSpace.x3), horizontalAlignment = Alignment.CenterHorizontally) {
                    Text(
                        model.durationText, maxLines = 1, overflow = TextOverflow.Ellipsis,
                        style = TextStyle(fontSize = 20.sp, fontWeight = FontWeight.SemiBold), color = c.textPrimary,
                    )
                    Text(model.goalLine, style = TextStyle(fontSize = 11.sp), color = c.textSecondary)
                }
            }
            Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                Text("Last night", style = Fm.text.titleMedium, color = c.textPrimary)
                Text(model.detailLine, style = Fm.text.bodyMedium, color = c.textSecondary)
            }
        }
    }
}

@Composable
private fun TrackingCard(
    model: SleepModel,
    permissionNote: String?,
    onDismissNote: () -> Unit,
    onToggle: (Boolean) -> Unit,
    onSync: () -> Unit,
) {
    val c = Fm.colors
    FmCard {
        Column(Modifier.fillMaxWidth(), verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text("Track sleep with my phone", Modifier.weight(1f), style = Fm.text.bodyLarge, color = c.textPrimary)
                FmSwitch(model.trackingEnabled, onToggle, enabled = model.trackingAvailable)
            }
            Text(model.sourceLine, style = Fm.text.bodySmall, color = c.textSecondary)
            FmInlineStatusLine(permissionNote, true, onDismiss = onDismissNote)
            if (model.trackingEnabled) {
                FmTextButton(onClick = onSync, enabled = !model.syncing) { Text(if (model.syncing) "Syncing…" else "Sync now") }
            }
            Text(
                "Your phone's step sensor tells ForkMesh when the phone lay still and when you used the app. The longest still, unused stretch near your bedtime counts as sleep. Nothing leaves the phone except one entry per night sent to your ForkMesh account.",
                style = Fm.text.bodySmall, color = c.textTertiary,
            )
        }
    }
}

@Composable
private fun AddNightDialog(
    model: SleepModel,
    onDismiss: () -> Unit,
    onRemove: (String) -> Unit,
    onSave: (String, Int, Int, Int) -> Unit,
) {
    var day by remember { mutableStateOf(LocalDate.now()) }
    var bed by remember { mutableStateOf(SleepSchedule.minutes(model.bedtime) ?: SleepSchedule.DEFAULT_BEDTIME) }
    var wake by remember { mutableStateOf(SleepSchedule.minutes(model.wakeTime) ?: SleepSchedule.DEFAULT_WAKE) }
    var awake by remember { mutableStateOf("") }
    var picking by remember { mutableStateOf<String?>(null) }
    FmAlertDialog(
        onDismissRequest = onDismiss,
        title = "Add a night",
        actions = {
            FmTextButton(onClick = { onRemove(day.toString()) }) { Text("Remove added") }
            Spacer(Modifier.weight(1f))
            FmTextButton(onClick = onDismiss) { Text("Cancel") }
            FmTextButton(onClick = {
                onSave(day.toString(), bed, wake, awake.trim().toDoubleOrNull()?.toInt() ?: 0)
            }) { Text("Save") }
        },
    ) {
        Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
            Text(
                "For a night your phone was not by your bed. It replaces what the phone recorded for that night.",
                style = Fm.text.bodyMedium, color = Fm.colors.textSecondary,
            )
            AccountOutlinedButton("Woke up on ${model.nightLabel(day.toString())}", { picking = "day" }, Modifier.fillMaxWidth(), icon = "calendar_today_outlined")
            AccountOutlinedButton("Went to bed ${SleepText.clock(bed)}", { picking = "bed" }, Modifier.fillMaxWidth(), icon = "dark_mode_outlined")
            AccountOutlinedButton("Woke up ${SleepText.clock(wake)}", { picking = "wake" }, Modifier.fillMaxWidth(), icon = "calendar_today_outlined")
            FmTextField(
                awake, { awake = it }, hint = "Minutes awake in bed (optional)",
                keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
            )
        }
    }
    when (picking) {
        "day" -> HealthDayPickerDialog(day, onDismiss = { picking = null }) {
            day = it
            picking = null
        }
        "bed" -> HealthTimePickerDialog(bed, onDismiss = { picking = null }) {
            bed = it
            picking = null
        }
        "wake" -> HealthTimePickerDialog(wake, onDismiss = { picking = null }) {
            wake = it
            picking = null
        }
    }
}

@Composable
private fun SleepGoalDialog(model: SleepModel, onDismiss: () -> Unit, onSave: (Int, String, String) -> Unit) {
    var hours by remember { mutableStateOf(StepsText.fixed(model.goal / 60.0, 2)) }
    var bed by remember { mutableStateOf(SleepSchedule.minutes(model.bedtime) ?: SleepSchedule.DEFAULT_BEDTIME) }
    var wake by remember { mutableStateOf(SleepSchedule.minutes(model.wakeTime) ?: SleepSchedule.DEFAULT_WAKE) }
    var picking by remember { mutableStateOf<String?>(null) }
    FmAlertDialog(
        onDismissRequest = onDismiss,
        title = "Sleep goal and schedule",
        actions = {
            Spacer(Modifier.weight(1f))
            FmTextButton(onClick = onDismiss) { Text("Cancel") }
            FmTextButton(onClick = {
                val value = hours.replace(',', '.').trim().toDoubleOrNull() ?: 0.0
                onSave(
                    (value * 60).roundToInt(),
                    SleepText.clockText(bed / 60, bed % 60),
                    SleepText.clockText(wake / 60, wake % 60),
                )
            }) { Text("Save") }
        },
    ) {
        Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
            Text("Your phone looks for its longest still stretch around this schedule.", style = Fm.text.bodyMedium, color = Fm.colors.textSecondary)
            FmTextField(
                hours, { hours = it }, hint = "Hours of sleep, e.g. 8", autofocus = true,
                keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Decimal),
            )
            AccountOutlinedButton("Bedtime ${SleepText.clock(bed)}", { picking = "bed" }, Modifier.fillMaxWidth(), icon = "dark_mode_outlined")
            AccountOutlinedButton("Wake time ${SleepText.clock(wake)}", { picking = "wake" }, Modifier.fillMaxWidth(), icon = "calendar_today_outlined")
        }
    }
    when (picking) {
        "bed" -> HealthTimePickerDialog(bed, onDismiss = { picking = null }) {
            bed = it
            picking = null
        }
        "wake" -> HealthTimePickerDialog(wake, onDismiss = { picking = null }) {
            wake = it
            picking = null
        }
    }
}
