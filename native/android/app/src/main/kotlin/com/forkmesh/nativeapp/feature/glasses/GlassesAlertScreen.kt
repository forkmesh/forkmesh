package com.forkmesh.nativeapp.feature.glasses

import android.content.Intent
import android.provider.Settings
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.feature.push.PingNotifier
import com.forkmesh.nativeapp.feature.tools.ToolsAppBar
import com.forkmesh.nativeapp.glasses.GlassesRange
import com.forkmesh.nativeapp.glasses.GlassesSighting
import com.forkmesh.nativeapp.glasses.NearbyDevice
import com.forkmesh.nativeapp.platform.AppLifecycle
import com.forkmesh.nativeapp.ui.kit.FmCard
import com.forkmesh.nativeapp.ui.kit.FmListRow
import com.forkmesh.nativeapp.ui.kit.FmSectionHeader
import com.forkmesh.nativeapp.ui.kit.FmSegmentTab
import com.forkmesh.nativeapp.ui.kit.FmSegmentTabBar
import com.forkmesh.nativeapp.ui.kit.FmTone
import com.forkmesh.nativeapp.ui.kit.FmToneButton
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import java.text.DateFormat
import java.util.Date

class GlassesAlertScreen : Screen() {
    override val title: String get() = "Glasses Alert"

    private var permissionsAsked by mutableIntStateOf(0)

    @Composable
    override fun Content(navigator: Navigator) {
        val context = LocalContext.current
        val state by GlassesWatchService.state.collectAsState()
        val foreground by AppLifecycle.foreground.collectAsState()
        var range by remember { mutableStateOf(GlassesWatchService.range(context)) }
        val scanAllowed = remember(foreground, permissionsAsked) { GlassesWatchService.scanPermitted(context) }
        val notificationsAllowed = remember(foreground, permissionsAsked) { PingNotifier.allowed(context) }
        val breaksSilence = remember(foreground) { GlassesAlarm.canBreakTotalSilence(context) }
        val ask = rememberLauncherForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) { _ ->
            permissionsAsked++
            if (GlassesWatchService.scanPermitted(context)) GlassesWatchService.start(context)
        }
        val ranges = GlassesRange.entries
        var picked by remember { mutableStateOf<NearbyDevice?>(null) }
        DisposableEffect(picked) {
            GlassesWatchService.track(picked)
            onDispose { GlassesWatchService.track(null) }
        }
        picked?.let { device ->
            NearbyDeviceSheet(
                device = state.others.firstOrNull { it.address == device.address } ?: device,
                tracked = state.tracked,
                watching = state.watching,
                onDismiss = { picked = null },
            )
        }
        Column(Modifier.fillMaxSize().background(Fm.colors.bgBase)) {
            ToolsAppBar("Glasses Alert", navigator)
            LazyColumn(Modifier.fillMaxSize()) {
                item {
                    StatusCard(
                        running = state.running,
                        watching = state.watching,
                        ringing = state.ringing,
                        count = state.nearby.size,
                        onToggle = {
                            if (state.running) {
                                GlassesWatchService.stop(context)
                            } else if (scanAllowed && notificationsAllowed) {
                                GlassesWatchService.start(context)
                            } else {
                                ask.launch(GlassesWatchService.runtimePermissions())
                            }
                        },
                        onSilence = { GlassesWatchService.silence(context) },
                        onTest = { GlassesWatchService.test(context) },
                    )
                }
                state.homing?.let { reading ->
                    item(key = "homing") { GlassesHomingCard(reading) }
                }
                state.problem?.let { problem ->
                    item {
                        FmListRow(
                            problem,
                            leading = { FmIcon("bluetooth_disabled", tint = Fm.colors.danger) },
                            showChevron = false,
                        )
                    }
                }
                if (!scanAllowed) {
                    item {
                        FmListRow(
                            "Allow Nearby devices",
                            subtitle = "Glasses Alert listens to Bluetooth adverts. It never connects to or pairs with anything.",
                            leading = { FmIcon("bluetooth_searching", tint = Fm.colors.warning) },
                            onTap = { ask.launch(GlassesWatchService.runtimePermissions()) },
                        )
                    }
                }
                if (!notificationsAllowed) {
                    item {
                        FmListRow(
                            "Allow notifications",
                            subtitle = "The alarm and the Stop button appear as a notification, even on the lock screen.",
                            leading = { FmIcon("notifications_active", tint = Fm.colors.warning) },
                            onTap = { ask.launch(GlassesWatchService.runtimePermissions()) },
                        )
                    }
                }
                item {
                    FmListRow(
                        if (breaksSilence) "Rings through Do Not Disturb" else "Ring through Do Not Disturb",
                        subtitle = if (breaksSilence) {
                            "On. When Do Not Disturb is set to total silence, the alarm switches it to alarms only while it rings, then puts it back."
                        } else {
                            "The alarm already rings on silent and vibrate. Allow Do Not Disturb access so it also rings through total silence."
                        },
                        leading = { FmIcon("do_not_disturb_on", tint = if (breaksSilence) Fm.colors.success else Fm.colors.warning) },
                        onTap = {
                            context.startActivity(Intent(Settings.ACTION_NOTIFICATION_POLICY_ACCESS_SETTINGS).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK))
                        },
                    )
                }
                item {
                    Column(Modifier.padding(horizontal = 16.dp, vertical = 6.dp)) {
                        FmSectionHeader("Alert range", icon = "radar")
                        Spacer(Modifier.height(4.dp))
                        FmSegmentTabBar(ranges.map { FmSegmentTab(it.label) }, ranges.indexOf(range), onChanged = {
                            range = ranges[it]
                            GlassesWatchService.setRange(context, range)
                        })
                    }
                }
                item {
                    Column(Modifier.padding(start = 16.dp, end = 16.dp, top = 12.dp)) {
                        FmSectionHeader("Nearby now", icon = "visibility", count = state.nearby.size)
                    }
                }
                if (state.nearby.isEmpty()) {
                    item {
                        Text(
                            if (state.watching) "No Meta glasses in range." else "Start watching to scan for Meta glasses.",
                            style = Fm.text.bodyMedium,
                            color = Fm.colors.textSecondary,
                            modifier = Modifier.padding(horizontal = 16.dp, vertical = 8.dp),
                        )
                    }
                } else {
                    items(state.nearby, key = { it.address }) { SightingRow(it) }
                }
                if (state.watching) {
                    item(key = "others") { NearbyDevicesSection(state.others, state.listening, onPick = { picked = it }) }
                }
            }
        }
    }
}

@Composable
private fun StatusCard(running: Boolean, watching: Boolean, ringing: Boolean, count: Int, onToggle: () -> Unit, onSilence: () -> Unit, onTest: () -> Unit) {
    FmCard(
        Modifier.padding(horizontal = 16.dp, vertical = 8.dp),
        color = Fm.colors.bgRaised.copy(alpha = 0.62f),
        padding = PaddingValues(12.dp),
    ) {
        Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
            Row(Modifier.weight(1f), verticalAlignment = Alignment.CenterVertically) {
                FmIcon(
                    if (ringing) "notifications_active" else if (watching) "bluetooth_searching" else "bluetooth",
                    tint = if (ringing) Fm.colors.danger else if (watching) Fm.colors.success else Fm.colors.textTertiary,
                )
                Spacer(Modifier.width(12.dp))
                Column {
                    Text(
                        when {
                            ringing -> "Smart glasses nearby"
                            watching -> "Watching for smart glasses"
                            running -> "Waiting for Bluetooth"
                            else -> "Not watching"
                        },
                        style = Fm.text.titleMedium,
                    )
                    Text(
                        when {
                            watching && count > 0 -> "$count nearby right now"
                            watching -> "Scanning nearby"
                            running -> "Waiting for Bluetooth"
                            else -> "Ready to scan"
                        },
                        style = Fm.text.bodySmall,
                        color = Fm.colors.textSecondary,
                    )
                }
            }
            Column(horizontalAlignment = Alignment.End, verticalArrangement = Arrangement.spacedBy(6.dp)) {
                FmToneButton(
                    if (running) "Stop" else "Start",
                    onPressed = onToggle,
                    icon = if (running) "stop" else "play_arrow",
                    tone = if (running) FmTone.Neutral else FmTone.Accent,
                    filled = !running,
                    tooltip = if (running) "Stop watching" else "Start watching",
                )
                if (ringing) {
                    FmToneButton("Stop alarm", onPressed = onSilence, icon = "volume_off", tone = FmTone.Danger, filled = true)
                } else {
                    FmToneButton("Test", onPressed = onTest, icon = "vibration", tooltip = "Test alarm")
                }
            }
        }
    }
}

@Composable
private fun SightingRow(sighting: GlassesSighting) {
    val seen = remember(sighting.seenAtMs) { DateFormat.getTimeInstance(DateFormat.SHORT).format(Date(sighting.seenAtMs)) }
    FmListRow(
        sighting.label,
        subtitle = listOfNotNull(sighting.distance, "${sighting.rssi} dBm", sighting.maker?.label, "seen $seen").joinToString(" · "),
        leading = { FmIcon("visibility", tint = if (sighting.rssi >= -70) Fm.colors.danger else Fm.colors.warning) },
        showChevron = false,
    )
}
