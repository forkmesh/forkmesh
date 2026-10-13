package com.forkmesh.nativeapp.feature.planning

import com.forkmesh.nativeapp.platform.AppLifecycle
import com.forkmesh.nativeapp.feature.push.PingNotifier
import com.forkmesh.nativeapp.feature.push.PushFeature
import android.content.Intent
import android.net.Uri
import android.os.Build
import android.provider.Settings
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.feature.tools.ToolsAppBar
import com.forkmesh.nativeapp.planning.PlanningTimer
import com.forkmesh.nativeapp.planning.PlanningTimerException
import com.forkmesh.nativeapp.planning.PlanningTimerKind
import com.forkmesh.nativeapp.ui.kit.FmCard
import com.forkmesh.nativeapp.ui.kit.FmConfirmDialog
import com.forkmesh.nativeapp.ui.kit.FmEmptyState
import com.forkmesh.nativeapp.ui.kit.FmExtendedFloatingActionButton
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmListRow
import com.forkmesh.nativeapp.ui.kit.FmSegmentTab
import com.forkmesh.nativeapp.ui.kit.FmSegmentTabBar
import com.forkmesh.nativeapp.ui.kit.FmTextField
import com.forkmesh.nativeapp.ui.kit.FmTone
import com.forkmesh.nativeapp.ui.kit.FmToneButton
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import kotlinx.coroutines.delay
import java.time.LocalDate
import java.time.LocalTime
import java.time.ZoneId
import java.time.format.DateTimeFormatter

class TimersScreen : Screen() {
    override val title: String get() = "Timers"

    private var now by mutableLongStateOf(System.currentTimeMillis())
    private var adding by mutableStateOf(false)

    @Composable
    override fun Content(navigator: Navigator) {
        val context = LocalContext.current
        val store = PlanningFeature.timerStore(context)
        val timers by PlanningFeature.timers.collectAsState()
        LaunchedEffect(Unit) {
            while (true) {
                now = System.currentTimeMillis()
                if (timers.any { it.isDue(now) }) PlanningFeature.settleTimers(context)
                delay(if (timers.any { it.isRunning }) 250 else 1_000)
            }
        }
        Box(Modifier.fillMaxSize().background(Fm.colors.bgBase)) {
            Column(Modifier.fillMaxSize()) {
                ToolsAppBar("Timers", navigator) {
                    if (timers.any { it.isFinished }) {
                        FmIconButton("clear_all", "Clear finished", onPressed = { store.removeFinished() }, color = Fm.colors.textPrimary)
                    }
                }
                val foreground by AppLifecycle.foreground.collectAsState()
                val notificationsAllowed = remember(foreground, timers) { PingNotifier.allowed(context) }
                if (!notificationsAllowed && timers.any { it.fireAtMs != null }) {
                    FmListRow(
                        "Allow notifications",
                        subtitle = "Countdowns and alarms ring through a notification.",
                        leading = { FmIcon("notifications_active", tint = Fm.colors.warning) },
                        onTap = { PushFeature.requestNotificationPermission(context) },
                    )
                }
                if (!TimerAlarms.canBeExact(context) && timers.any { it.fireAtMs != null }) {
                    FmListRow(
                        "Allow exact alarms",
                        subtitle = "Without it, Android may ring countdowns and alarms a few minutes late.",
                        leading = { FmIcon("alarm_on", tint = Fm.colors.warning) },
                        onTap = {
                            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                                context.startActivity(
                                    Intent(Settings.ACTION_REQUEST_SCHEDULE_EXACT_ALARM, Uri.parse("package:" + context.packageName))
                                        .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK),
                                )
                            }
                        },
                    )
                }
                if (timers.isEmpty()) {
                    FmEmptyState("timer", "No timers", message = "Start a timer, a countdown or an alarm. They keep running when the app is closed.")
                } else {
                    LazyColumn(Modifier.fillMaxSize()) {
                        items(timers, key = { it.id }) { timer -> TimerRow(timer, now, store) }
                        item { Spacer(Modifier.height(88.dp)) }
                    }
                }
            }
            FmExtendedFloatingActionButton("New", onClick = { adding = true }, modifier = Modifier.align(Alignment.BottomEnd).padding(16.dp), icon = "add")
        }
        if (adding) NewTimerDialog(onDismiss = { adding = false }) { kind, label, value ->
            try {
                when (kind) {
                    PlanningTimerKind.STOPWATCH -> store.startStopwatch(label)
                    PlanningTimerKind.COUNTDOWN -> store.startCountdown(label, parseDuration(value) ?: 0)
                    PlanningTimerKind.ALARM -> store.setAlarm(label, parseAlarm(value) ?: 0)
                }
                if (kind != PlanningTimerKind.STOPWATCH) PushFeature.requestNotificationPermission(context)
                adding = false
                null
            } catch (thrown: PlanningTimerException) {
                thrown.message
            }
        }
    }
}

@Composable
private fun TimerRow(timer: PlanningTimer, now: Long, store: com.forkmesh.nativeapp.planning.PlanningTimerStore) {
    FmCard(Modifier.padding(horizontal = 16.dp, vertical = 6.dp)) {
        Column(Modifier.padding(12.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                FmIcon(timer.kind.icon, tint = if (timer.isFinished) Fm.colors.textTertiary else Fm.colors.accent)
                Spacer(Modifier.padding(4.dp))
                Column(Modifier.weight(1f)) {
                    Text(timer.label, style = Fm.text.bodyMedium)
                    Text(
                        when {
                            timer.kind == PlanningTimerKind.ALARM -> "Rings at " + formatAt(timer.alarmAtMs, ZoneId.systemDefault(), DateTimeFormatter.ofPattern("EEE HH:mm"))
                            timer.isPaused -> "Paused"
                            timer.isFinished -> "Finished"
                            else -> timer.kind.label
                        },
                        style = Fm.text.bodySmall,
                        color = Fm.colors.textTertiary,
                    )
                }
                Text(timer.display(now), style = Fm.text.headlineSmall)
            }
            Row(Modifier.padding(top = 8.dp), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                if (timer.isRunning && timer.kind != PlanningTimerKind.ALARM) FmToneButton("Pause", onPressed = { store.pause(timer.id) })
                if (timer.isPaused) FmToneButton("Resume", onPressed = { store.resume(timer.id) }, tone = FmTone.Accent)
                if (timer.kind != PlanningTimerKind.ALARM && (timer.isFinished || timer.isPaused)) FmToneButton("Restart", onPressed = { store.restart(timer.id) })
                if (!timer.isFinished) FmToneButton(if (timer.kind == PlanningTimerKind.ALARM) "Cancel" else "Stop", onPressed = { store.stop(timer.id) })
                FmToneButton("Remove", onPressed = { store.remove(timer.id) }, tone = FmTone.Danger)
            }
        }
    }
}

internal fun parseDuration(text: String): Int? {
    val parts = text.trim().split(':').map { it.trim().toIntOrNull() ?: return null }
    return when (parts.size) {
        1 -> parts[0] * 60
        2 -> parts[0] * 60 + parts[1]
        3 -> parts[0] * 3600 + parts[1] * 60 + parts[2]
        else -> null
    }
}

internal fun parseAlarm(text: String, zone: ZoneId = ZoneId.systemDefault(), now: Long = System.currentTimeMillis()): Long? {
    val time = runCatching { LocalTime.parse(text.trim(), DateTimeFormatter.ofPattern("H:mm")) }.getOrNull() ?: return null
    val today = LocalDate.now(zone).atTime(time).atZone(zone).toInstant().toEpochMilli()
    return if (today > now) today else LocalDate.now(zone).plusDays(1).atTime(time).atZone(zone).toInstant().toEpochMilli()
}

@Composable
private fun NewTimerDialog(onDismiss: () -> Unit, onCreate: (PlanningTimerKind, String, String) -> String?) {
    var kind by remember { mutableStateOf(PlanningTimerKind.COUNTDOWN) }
    var label by remember { mutableStateOf("") }
    var value by remember { mutableStateOf("5:00") }
    var problem by remember { mutableStateOf<String?>(null) }
    val kinds = PlanningTimerKind.entries
    FmConfirmDialog(
        title = "New ${kind.label.lowercase()}",
        confirmLabel = "Start",
        onDismissRequest = onDismiss,
        onConfirm = { problem = onCreate(kind, label, value) },
        fields = {
            FmSegmentTabBar(kinds.map { FmSegmentTab(it.label) }, kinds.indexOf(kind), onChanged = {
                kind = kinds[it]
                value = when (kind) {
                    PlanningTimerKind.COUNTDOWN -> "5:00"
                    PlanningTimerKind.ALARM -> "7:00"
                    PlanningTimerKind.STOPWATCH -> ""
                }
            })
            FmTextField(label, { label = it }, hint = "Label")
            when (kind) {
                PlanningTimerKind.COUNTDOWN -> FmTextField(value, { value = it }, hint = "Minutes, or MM:SS or H:MM:SS")
                PlanningTimerKind.ALARM -> FmTextField(value, { value = it }, hint = "Time (HH:MM)")
                PlanningTimerKind.STOPWATCH -> Unit
            }
            problem?.let { Text(it, color = Fm.colors.danger, style = Fm.text.bodySmall) }
        },
    )
}
