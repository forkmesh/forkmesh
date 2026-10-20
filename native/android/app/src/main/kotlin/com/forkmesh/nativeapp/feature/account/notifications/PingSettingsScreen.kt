package com.forkmesh.nativeapp.feature.account.notifications

import android.text.format.DateFormat
import androidx.compose.animation.animateColorAsState
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.DatePicker
import androidx.compose.material3.DatePickerDialog
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.FilterChip
import androidx.compose.material3.FilterChipDefaults
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.SelectableDates
import androidx.compose.material3.Text
import androidx.compose.material3.TimePicker
import androidx.compose.material3.rememberDatePickerState
import androidx.compose.material3.rememberTimePickerState
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
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.account.MonitorAlertField
import com.forkmesh.nativeapp.account.PING_DAY_LABELS
import com.forkmesh.nativeapp.account.PING_PAUSE_NOTE
import com.forkmesh.nativeapp.account.PING_SETTINGS_INTRO
import com.forkmesh.nativeapp.account.PING_PAUSE_PRESETS
import com.forkmesh.nativeapp.account.PING_SETTINGS_TITLE
import com.forkmesh.nativeapp.account.PingSettingsChange
import com.forkmesh.nativeapp.account.customPauseUntil
import com.forkmesh.nativeapp.account.effectiveQuietTimezone
import com.forkmesh.nativeapp.account.emailSwitchChecked
import com.forkmesh.nativeapp.account.emailSwitchEnabled
import com.forkmesh.nativeapp.account.isGeneralChatDigestOn
import com.forkmesh.nativeapp.account.isKindEnabled
import com.forkmesh.nativeapp.account.isPaused
import com.forkmesh.nativeapp.account.isValidTimezone
import com.forkmesh.nativeapp.account.lastPauseDay
import com.forkmesh.nativeapp.account.minutesToClock
import com.forkmesh.nativeapp.account.pauseUntilFor
import com.forkmesh.nativeapp.account.pingDeadlineLabel
import com.forkmesh.nativeapp.account.pingGroupIndexOf
import com.forkmesh.nativeapp.account.pingSettingsErrorText
import com.forkmesh.nativeapp.account.quietDaysLabel
import com.forkmesh.nativeapp.account.quietHoursRangeLabel
import com.forkmesh.nativeapp.account.showsPingSettings
import com.forkmesh.nativeapp.account.toggleQuietDay
import com.forkmesh.nativeapp.account.withCodePushes
import com.forkmesh.nativeapp.account.withDelivery
import com.forkmesh.nativeapp.account.withEmailNotifications
import com.forkmesh.nativeapp.account.withKindEmail
import com.forkmesh.nativeapp.account.withKindEnabled
import com.forkmesh.nativeapp.account.withMonitorAlert
import com.forkmesh.nativeapp.account.withPauseUntil
import com.forkmesh.nativeapp.account.withQuietHours
import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.account.AccountOutlinedButton
import com.forkmesh.nativeapp.feature.account.AccountScreen
import com.forkmesh.nativeapp.feature.account.AuthService
import com.forkmesh.nativeapp.feature.account.HGap
import com.forkmesh.nativeapp.feature.account.VGap
import com.forkmesh.nativeapp.model.PingKind
import com.forkmesh.nativeapp.model.PingKindGroup
import com.forkmesh.nativeapp.model.PingQuietHours
import com.forkmesh.nativeapp.model.PingSettings
import com.forkmesh.nativeapp.model.StatusMonitorAlert
import com.forkmesh.nativeapp.ui.kit.FmAlertDialog
import com.forkmesh.nativeapp.ui.kit.FmCard
import com.forkmesh.nativeapp.ui.kit.FmCheckbox
import com.forkmesh.nativeapp.ui.kit.FmEmptyState
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmInlineStatus
import com.forkmesh.nativeapp.ui.kit.FmListRow
import com.forkmesh.nativeapp.ui.kit.FmPanelHeader
import com.forkmesh.nativeapp.ui.kit.FmSectionHeader
import com.forkmesh.nativeapp.ui.kit.FmSkeletonList
import com.forkmesh.nativeapp.ui.kit.FmSoundOnShow
import com.forkmesh.nativeapp.ui.kit.FmStatus
import com.forkmesh.nativeapp.ui.kit.FmSwitch
import com.forkmesh.nativeapp.ui.kit.FmTextButton
import com.forkmesh.nativeapp.ui.kit.LocalFmMessenger
import com.forkmesh.nativeapp.ui.kit.fmSafeAreaInsets
import com.forkmesh.nativeapp.ui.kit.fmTapSounds
import com.forkmesh.nativeapp.ui.kit.rememberFmStatus
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmRadius
import com.forkmesh.nativeapp.ui.theme.FmSpace
import com.forkmesh.nativeapp.ui.theme.FmType
import java.time.Instant
import java.time.LocalDate
import java.time.ZoneId
import java.time.ZoneOffset
import java.util.TimeZone
import kotlin.coroutines.cancellation.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext

class PingSettingsScreen(private val initialKind: String? = null) : AccountScreen() {
    private var settings by mutableStateOf<PingSettings?>(null)
    private var loading by mutableStateOf(false)
    private var failure by mutableStateOf<Throwable?>(null)
    private var started = false
    private var scrolled = false

    private var generation = 0
    private val saves = Mutex()
    private val saveStatus = FmStatus()

    override val title: String get() = PING_SETTINGS_TITLE

    private fun node(): String = Services.get<AuthService>()?.core?.session?.nodeName.orEmpty()

    private fun reload(api: ApiService) {
        val current = ++generation
        loading = true
        failure = null
        scope.launch {
            try {
                val loaded = withContext(Dispatchers.IO) { api.pingSettings(node()) }
                if (current == generation) settings = loaded
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (error: Exception) {
                if (current == generation) failure = error
            } finally {
                if (current == generation) loading = false
            }
        }
    }

    private fun apply(api: ApiService, change: PingSettingsChange) {
        val before = settings ?: return
        val current = ++generation
        saveStatus.clear()
        settings = change.next
        scope.launch {
            saves.withLock {
                try {
                    val saved = withContext(Dispatchers.IO) { api.savePingSettings(node(), change.patch) }
                    if (current == generation) settings = saved
                    com.forkmesh.nativeapp.feature.push.PingAlerts.invalidateSettings(
                        Services.get<com.forkmesh.nativeapp.account.projects.ProjectScope>()?.projectId.orEmpty(),
                    )
                } catch (cancelled: CancellationException) {
                    throw cancelled
                } catch (error: Exception) {
                    saveStatus.error(pingSettingsErrorText(error))
                    if (current == generation) {
                        settings = before
                    } else {
                        reload(api)
                    }
                }
            }
        }
    }

    @OptIn(ExperimentalLayoutApi::class)
    @Composable
    override fun Content(navigator: Navigator) {
        val api = Services.require<ApiService>()
        val auth = Services.require<AuthService>()
        val session by auth.session.collectAsState()
        val guest by auth.isGuest.collectAsState()
        val c = Fm.colors
        val signedIn = showsPingSettings(session, guest)
        LaunchedEffect(signedIn) {
            if (!started && signedIn) {
                started = true
                reload(api)
            }
        }
        val change: (PingSettingsChange) -> Unit = { apply(api, it) }

        Column(
            Modifier
                .fillMaxSize()
                .background(c.bgBase)
                .windowInsetsPadding(fmSafeAreaInsets()),
        ) {
            Box(
                Modifier
                    .fillMaxWidth()
                    .background(c.bgRaised),
            ) {
                FmPanelHeader(
                    title = PING_SETTINGS_TITLE,
                    subtitle = "Saved as you change them",
                    leading = {
                        FmIconButton(icon = "arrow_back", label = "Back", onPressed = { navigator.pop() }, color = c.textPrimary)
                    },
                )
            }
            FmInlineStatus(saveStatus, Modifier.padding(horizontal = FmSpace.x4, vertical = FmSpace.x1))
            val current = settings
            Box(
                Modifier
                    .weight(1f)
                    .fillMaxWidth(),
            ) {
                when {
                    !signedIn -> FmEmptyState(
                        icon = "notifications_off_outlined",
                        title = "Sign in to manage pings",
                        message = "Ping settings belong to your ForkMesh account.",
                    )
                    current == null && failure != null -> Column(
                        Modifier.fillMaxSize(),
                        verticalArrangement = Arrangement.Center,
                        horizontalAlignment = Alignment.CenterHorizontally,
                    ) {
                        FmEmptyState(
                            icon = "cloud_off_outlined",
                            title = "Could not load ping settings",
                            modifier = Modifier.weight(1f, fill = false),
                            message = pingSettingsErrorText(failure),
                        )
                        FmTextButton(onClick = { reload(api) }) {
                            FmIcon("refresh", size = 18.dp)
                            HGap(8.dp)
                            Text("Retry")
                        }
                    }
                    current == null -> FmSkeletonList(rows = 6, label = "Loading ping settings")
                    else -> PingSettingsList(current, initialKind, change)
                }
            }
        }
    }

    @OptIn(ExperimentalLayoutApi::class)
    @Composable
    private fun PingSettingsList(settings: PingSettings, initialKind: String?, change: (PingSettingsChange) -> Unit) {
        val listState = rememberLazyListState()
        val now = System.currentTimeMillis()
        val zone = remember { ZoneId.systemDefault() }
        val groups = settings.pingKindCatalog
        val monitors = settings.statusMonitorAlerts.orEmpty()
        val headItems = 5
        LaunchedEffect(initialKind, groups.size) {
            val index = pingGroupIndexOf(settings, initialKind)
            if (!scrolled && index >= 0) {
                scrolled = true
                listState.animateScrollToItem(headItems + index)
            }
        }
        LazyColumn(
            Modifier.fillMaxSize(),
            state = listState,
            contentPadding = PaddingValues(FmSpace.x4),
        ) {
            item {
                Column(Modifier.fillMaxWidth()) {
                    Text(PING_SETTINGS_INTRO, style = noteStyle())
                    VGap(FmSpace.x5)
                }
            }
            item {
                Section("This phone", icon = "phone_android_outlined") {
                    FmCard(modifier = Modifier.fillMaxWidth(), radius = FmRadius.lg) {
                        com.forkmesh.nativeapp.feature.push.PushFeature.BackgroundCard()
                    }
                }
            }
            item {
                Section("Pause", icon = "pause_circle_outline") {
                    FmCard(modifier = Modifier.fillMaxWidth(), radius = FmRadius.lg) {
                        PauseCard(settings, now, zone, change)
                    }
                }
            }
            item {
                Section("Quiet hours", icon = "bedtime_outlined") {
                    FmCard(modifier = Modifier.fillMaxWidth(), radius = FmRadius.lg) {
                        QuietHoursCard(settings, now, change)
                    }
                }
            }
            item {
                Column(Modifier.fillMaxWidth()) {
                    FmSectionHeader(title = "Ping types", icon = "tune_rounded")
                    VGap(FmSpace.x2)
                    FmCard(modifier = Modifier.fillMaxWidth(), radius = FmRadius.lg) {
                        SwitchRow(
                            title = "Code pushes",
                            note = "A ping on this phone whenever code is pushed to a repository you own or follow.",
                            checked = settings.codePushes,
                            onCheckedChange = { change(withCodePushes(settings, it)) },
                            modifier = Modifier.testTag("ping-code-pushes"),
                        )
                    }
                    VGap(FmSpace.x3)
                }
            }
            itemsIndexed(groups, key = { index, group -> "group-$index-${group.key}" }) { index, group ->
                Column(Modifier.fillMaxWidth()) {
                    KindGroupCard(settings, group, initialKind, change)
                    VGap(if (index == groups.lastIndex) FmSpace.x5 else FmSpace.x3)
                }
            }
            item {
                Section("Email", icon = "mail_outlined", bottomGap = monitors.isNotEmpty()) {
                    FmCard(modifier = Modifier.fillMaxWidth(), radius = FmRadius.lg) {
                        EmailCard(settings, change)
                    }
                }
            }
            if (monitors.isNotEmpty()) {
                item {
                    Section("Administrator alerts", icon = "admin_panel_settings_outlined", bottomGap = false) {
                        FmCard(modifier = Modifier.fillMaxWidth(), radius = FmRadius.lg) {
                            MonitorAlertsCard(settings, monitors, change)
                        }
                    }
                }
            }
        }
    }
}

@Composable
private fun Section(
    title: String,
    icon: String? = null,
    bottomGap: Boolean = true,
    content: @Composable ColumnScope.() -> Unit,
) {
    Column(Modifier.fillMaxWidth()) {
        FmSectionHeader(title = title, icon = icon)
        VGap(FmSpace.x2)
        content()
        if (bottomGap) VGap(FmSpace.x5)
    }
}

@Composable
private fun noteStyle(): TextStyle =
    Fm.text.bodySmall.copy(color = Fm.colors.textSecondary, lineHeight = FmType.meta * FmType.leadingSnug)

@Composable
private fun emphasisStyle(): TextStyle = Fm.text.bodyMedium.copy(fontWeight = FmType.emphasis)

@Composable
private fun SwitchRow(
    title: String,
    checked: Boolean,
    onCheckedChange: (Boolean) -> Unit,
    modifier: Modifier = Modifier,
    note: String? = null,
    enabled: Boolean = true,
) {
    Row(modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
        Column(Modifier.weight(1f)) {
            Text(title, style = emphasisStyle())
            if (note != null) {
                VGap(FmSpace.x1)
                Text(note, style = noteStyle())
            }
        }
        HGap(FmSpace.x3)
        FmSwitch(
            checked = checked,
            onCheckedChange = onCheckedChange,
            enabled = enabled,
            modifier = Modifier.semantics { contentDescription = title },
        )
    }
}

@OptIn(ExperimentalLayoutApi::class, ExperimentalMaterial3Api::class)
@Composable
private fun PauseCard(settings: PingSettings, now: Long, zone: ZoneId, change: (PingSettingsChange) -> Unit) {
    val c = Fm.colors
    val context = LocalContext.current
    val pauseStatus = rememberFmStatus()
    val paused = isPaused(settings, now)
    var pickingDate by remember { mutableStateOf(false) }
    var pickedDay by remember { mutableStateOf<LocalDate?>(null) }
    Column(Modifier.fillMaxWidth()) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            FmIcon(
                if (paused) "notifications_paused_outlined" else "notifications_active_outlined",
                size = 20.dp,
                tint = if (paused) c.warning else c.success,
            )
            HGap(FmSpace.x2)
            Text(
                if (paused) {
                    "Paused until ${pingDeadlineLabel(settings.notificationPauseUntil, now, zone)}"
                } else {
                    "Pings are delivered as they happen"
                },
                modifier = Modifier
                    .weight(1f)
                    .testTag("ping-pause-status"),
                style = Fm.text.titleSmall,
            )
        }
        VGap(FmSpace.x1)
        Text(PING_PAUSE_NOTE, style = noteStyle())
        VGap(FmSpace.x3)
        FlowRow(
            horizontalArrangement = Arrangement.spacedBy(FmSpace.x2),
            verticalArrangement = Arrangement.spacedBy(FmSpace.x2),
        ) {
            PING_PAUSE_PRESETS.forEach { preset ->
                AccountOutlinedButton(
                    label = preset.label,
                    onClick = {
                        change(withPauseUntil(settings, pauseUntilFor(preset.durationMs, System.currentTimeMillis())))
                    },
                )
            }
            AccountOutlinedButton(
                label = "Custom\u2026",
                onClick = { pickingDate = true },
                modifier = Modifier.testTag("ping-pause-custom"),
            )
            if (paused) {
                AccountOutlinedButton(
                    label = "Resume",
                    icon = "play_arrow_rounded",
                    onClick = { change(withPauseUntil(settings, 0)) },
                    modifier = Modifier.testTag("ping-pause-resume"),
                )
            }
        }
        FmInlineStatus(pauseStatus, Modifier.padding(top = FmSpace.x2))
    }

    if (pickingDate) {
        val today = remember { LocalDate.now(zone) }
        val last = remember { lastPauseDay(System.currentTimeMillis(), zone) }
        val state = rememberDatePickerState(
            initialSelectedDateMillis = today.plusDays(1).atStartOfDay(ZoneOffset.UTC).toInstant().toEpochMilli(),
            selectableDates = remember(today, last) {
                object : SelectableDates {
                    override fun isSelectableDate(utcTimeMillis: Long): Boolean {
                        val day = Instant.ofEpochMilli(utcTimeMillis).atZone(ZoneOffset.UTC).toLocalDate()
                        return !day.isBefore(today) && !day.isAfter(last)
                    }

                    override fun isSelectableYear(year: Int): Boolean = year in today.year..last.year
                }
            },
        )
        FmSoundOnShow()
        DatePickerDialog(
            onDismissRequest = { pickingDate = false },
            modifier = Modifier.fmTapSounds(),
            confirmButton = {
                FmTextButton(onClick = {
                    pickedDay = state.selectedDateMillis?.let { Instant.ofEpochMilli(it).atZone(ZoneOffset.UTC).toLocalDate() }
                    pickingDate = false
                }) { Text("Next") }
            },
            dismissButton = { FmTextButton(onClick = { pickingDate = false }) { Text("Cancel") } },
        ) {
            DatePicker(state = state, title = { Text("Pause pings until", modifier = Modifier.padding(FmSpace.x4)) })
        }
    }

    val day = pickedDay
    if (day != null) {
        val start = remember(day) { Instant.ofEpochMilli(System.currentTimeMillis()).atZone(zone) }
        val state = rememberTimePickerState(
            initialHour = start.hour,
            initialMinute = start.minute,
            is24Hour = DateFormat.is24HourFormat(context),
        )
        FmAlertDialog(
            onDismissRequest = { pickedDay = null },
            title = "Pause pings until",
            containerColor = c.bgOverlay,
            shape = RoundedCornerShape(FmRadius.lg),
            actions = {
                FmTextButton(onClick = { pickedDay = null }) { Text("Cancel") }
                FmTextButton(onClick = {
                    pickedDay = null
                    val until = customPauseUntil(day, state.hour * 60 + state.minute, zone, System.currentTimeMillis())
                    if (until == null) {
                        pauseStatus.error("Pick a time in the future.")
                    } else {
                        pauseStatus.clear()
                        change(withPauseUntil(settings, until))
                    }
                }) { Text("Pause") }
            },
            content = { TimePicker(state = state) },
        )
    }
}

@OptIn(ExperimentalLayoutApi::class, ExperimentalMaterial3Api::class)
@Composable
private fun QuietHoursCard(settings: PingSettings, nowMs: Long, change: (PingSettingsChange) -> Unit) {
    val c = Fm.colors
    val context = LocalContext.current
    val deviceZone = remember { TimeZone.getDefault().id }
    val stored = settings.notificationQuietHours
    val quiet = stored.copy(timezone = effectiveQuietTimezone(stored.timezone, deviceZone))
    val save: (PingQuietHours) -> Unit = { change(withQuietHours(settings, it)) }
    var picking by remember { mutableStateOf<String?>(null) }
    val messenger = LocalFmMessenger.current
    val scope = rememberCoroutineScope()
    val zoneStatus = rememberFmStatus()

    Column(Modifier.fillMaxWidth()) {
        SwitchRow(
            title = "Quiet hours",
            note = if (quiet.enabled && settings.notificationQuietUntil > nowMs) {
                "Quiet until ${pingDeadlineLabel(settings.notificationQuietUntil, nowMs, ZoneId.systemDefault())}"
            } else if (quiet.enabled) {
                "${quietHoursRangeLabel(quiet)}, ${quietDaysLabel(quiet.days).lowercase()}"
            } else {
                "Mute banners, cards and email every day at the same time"
            },
            checked = quiet.enabled,
            onCheckedChange = { save(quiet.copy(enabled = it)) },
            modifier = Modifier.testTag("ping-quiet-hours-switch"),
        )
        VGap(FmSpace.x3)
        HorizontalDivider(thickness = 1.dp, color = c.border)
        FmListRow(
            title = "From",
            modifier = Modifier.testTag("ping-quiet-start"),
            leading = { FmIcon("bedtime_outlined", tint = c.textSecondary) },
            trailing = { Text(minutesToClock(quiet.start), style = emphasisStyle()) },
            showChevron = false,
            padding = PaddingValues(vertical = FmSpace.x2),
            onTap = { picking = "start" },
        )
        HorizontalDivider(thickness = 1.dp, color = c.border)
        FmListRow(
            title = "Until",
            modifier = Modifier.testTag("ping-quiet-end"),
            subtitle = if (quiet.end <= quiet.start) "The next day" else null,
            leading = { FmIcon("wb_sunny_outlined", tint = c.textSecondary) },
            trailing = { Text(minutesToClock(quiet.end), style = emphasisStyle()) },
            showChevron = false,
            padding = PaddingValues(vertical = FmSpace.x2),
            onTap = { picking = "end" },
        )
        HorizontalDivider(thickness = 1.dp, color = c.border)
        VGap(FmSpace.x3)
        FlowRow(
            horizontalArrangement = Arrangement.spacedBy(FmSpace.x2),
            verticalArrangement = Arrangement.spacedBy(FmSpace.x2),
        ) {
            PING_DAY_LABELS.forEachIndexed { day, label ->
                DayChip(label, selected = day in quiet.days, modifier = Modifier.testTag("ping-quiet-day-$day")) {
                    save(quiet.copy(days = toggleQuietDay(quiet.days, day)))
                }
            }
        }
        VGap(FmSpace.x2)
        Text(
            "Days a quiet window starts on. A window that ends before it starts runs past midnight.",
            style = noteStyle(),
        )
        VGap(FmSpace.x2)
        HorizontalDivider(thickness = 1.dp, color = c.border)
        FmListRow(
            title = "Time zone",
            modifier = Modifier.testTag("ping-quiet-timezone"),
            subtitle = quiet.timezone,
            leading = { FmIcon("public", tint = c.textSecondary) },
            padding = PaddingValues(vertical = FmSpace.x2),
            onTap = {
                scope.launch {
                    val zoneId = messenger.showPromptDialog(
                        title = "Quiet hours time zone",
                        hint = "Europe/Berlin",
                        initialValue = quiet.timezone,
                        confirmLabel = "Save",
                    ) ?: return@launch
                    if (zoneId.isEmpty() || zoneId == quiet.timezone) return@launch
                    if (!isValidTimezone(zoneId)) {
                        zoneStatus.error("\"$zoneId\" is not a time zone. Use a name like Europe/Berlin.")
                        return@launch
                    }
                    zoneStatus.clear()
                    save(quiet.copy(timezone = zoneId))
                }
            },
        )
        FmInlineStatus(zoneStatus)
    }

    val field = picking
    if (field != null) {
        val initial = if (field == "start") quiet.start else quiet.end
        val state = rememberTimePickerState(
            initialHour = initial / 60,
            initialMinute = initial % 60,
            is24Hour = DateFormat.is24HourFormat(context),
        )
        FmAlertDialog(
            onDismissRequest = { picking = null },
            title = if (field == "start") "Quiet from" else "Quiet until",
            containerColor = c.bgOverlay,
            shape = RoundedCornerShape(FmRadius.lg),
            actions = {
                FmTextButton(onClick = { picking = null }) { Text("Cancel") }
                FmTextButton(onClick = {
                    val minutes = state.hour * 60 + state.minute
                    picking = null
                    save(if (field == "start") quiet.copy(start = minutes) else quiet.copy(end = minutes))
                }) { Text("OK") }
            },
            content = { TimePicker(state = state) },
        )
    }
}

@Composable
private fun DayChip(label: String, selected: Boolean, modifier: Modifier = Modifier, onClick: () -> Unit) {
    val c = Fm.colors
    FilterChip(
        selected = selected,
        onClick = onClick,
        modifier = modifier,
        label = {
            Text(
                label,
                style = TextStyle(fontFamily = FmType.ui, fontSize = FmType.small, fontWeight = if (selected) FmType.emphasis else FmType.regular),
            )
        },
        shape = RoundedCornerShape(FmRadius.sm),
        colors = FilterChipDefaults.filterChipColors(
            containerColor = c.bgBase,
            labelColor = c.textSecondary,
            selectedContainerColor = c.accentSubtle,
            selectedLabelColor = c.textPrimary,
        ),
        border = FilterChipDefaults.filterChipBorder(
            enabled = true,
            selected = selected,
            borderColor = c.border,
            selectedBorderColor = c.accent.copy(alpha = 0.22f),
            borderWidth = 1.dp,
            selectedBorderWidth = 1.dp,
        ),
    )
}

@Composable
private fun ColumnCaptions(title: String, captions: List<String>, width: androidx.compose.ui.unit.Dp) {
    Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.Bottom) {
        Text(title, modifier = Modifier.weight(1f), style = emphasisStyle())
        captions.forEach { caption ->
            Text(
                caption,
                modifier = Modifier.width(width),
                textAlign = TextAlign.Center,
                style = Fm.text.labelSmall,
            )
        }
    }
}

private val SWITCH_COLUMN = 60.dp
private val CHECK_COLUMN = 56.dp

@Composable
private fun KindGroupCard(
    settings: PingSettings,
    group: PingKindGroup,
    highlightKind: String?,
    change: (PingSettingsChange) -> Unit,
) {
    val c = Fm.colors
    val anyEmail = group.kinds.any { it.email }
    FmCard(modifier = Modifier.fillMaxWidth(), radius = FmRadius.lg) {
        Column(Modifier.fillMaxWidth()) {
            ColumnCaptions(group.label, if (anyEmail) listOf("Ping", "Email") else listOf("Ping"), SWITCH_COLUMN)
            VGap(FmSpace.x2)
            group.kinds.forEachIndexed { index, kind ->
                if (index > 0) HorizontalDivider(thickness = 1.dp, color = c.border)
                KindRow(settings, kind, anyEmail, highlighted = kind.kind == highlightKind, change = change)
            }
        }
    }
}

@Composable
private fun KindRow(
    settings: PingSettings,
    kind: PingKind,
    emailColumn: Boolean,
    highlighted: Boolean,
    change: (PingSettingsChange) -> Unit,
) {
    val c = Fm.colors
    val wash by animateColorAsState(if (highlighted) c.accentSubtle else Color.Transparent, label = "kind-highlight")
    val enabled = isKindEnabled(settings, kind.kind)
    Row(
        Modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(FmRadius.md))
            .background(wash)
            .padding(vertical = FmSpace.x1)
            .testTag("ping-kind-${kind.kind}"),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(
            kind.label,
            modifier = Modifier
                .weight(1f)
                .padding(start = if (highlighted) FmSpace.x2 else 0.dp),
            maxLines = 2,
            overflow = TextOverflow.Ellipsis,
            style = Fm.text.bodyMedium.copy(color = if (enabled) c.textPrimary else c.textTertiary),
        )
        Box(Modifier.width(SWITCH_COLUMN), contentAlignment = Alignment.Center) {
            FmSwitch(
                checked = enabled,
                onCheckedChange = { change(withKindEnabled(settings, kind.kind, it)) },
                modifier = Modifier.semantics { contentDescription = "${kind.label} pings" },
            )
        }
        if (emailColumn) {
            Box(Modifier.width(SWITCH_COLUMN), contentAlignment = Alignment.Center) {
                if (kind.email) {
                    FmSwitch(
                        checked = emailSwitchChecked(settings, kind),
                        enabled = emailSwitchEnabled(settings, kind),
                        onCheckedChange = { change(withKindEmail(settings, kind.kind, it)) },
                        modifier = Modifier.semantics { contentDescription = "${kind.label} email" },
                    )
                } else {
                    Text("—", style = Fm.text.bodySmall.copy(color = c.textTertiary))
                }
            }
        }
    }
}

@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun EmailCard(settings: PingSettings, change: (PingSettingsChange) -> Unit) {
    val c = Fm.colors
    Column(Modifier.fillMaxWidth()) {
        SwitchRow(
            title = "Email me about pings",
            note = "Only to a verified address. Turn a type's email off above.",
            checked = settings.emailNotifications,
            onCheckedChange = { change(withEmailNotifications(settings, it)) },
            modifier = Modifier.testTag("ping-email-master"),
        )
        VGap(FmSpace.x3)
        HorizontalDivider(thickness = 1.dp, color = c.border)
        VGap(FmSpace.x3)
        Text(
            "How often",
            style = emphasisStyle().copy(color = if (settings.emailNotifications) c.textPrimary else c.textTertiary),
        )
        VGap(FmSpace.x2)
        FlowRow(horizontalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
            listOf("digest" to "Digest", "instant" to "Instant").forEach { (mode, label) ->
                DayChip(label, selected = settings.notificationDelivery == mode) {
                    if (settings.emailNotifications && settings.notificationDelivery != mode) change(withDelivery(settings, mode))
                }
            }
        }
        VGap(FmSpace.x1)
        Text(
            if (settings.notificationDelivery == "instant") {
                "Each ping is emailed as it arrives."
            } else {
                "Unread pings are rolled into one email."
            },
            style = noteStyle(),
        )
        VGap(FmSpace.x3)
        HorizontalDivider(thickness = 1.dp, color = c.border)
        VGap(FmSpace.x3)
        SwitchRow(
            title = "#general chat digest",
            note = "A summary of the public #general room.",
            checked = settings.emailNotifications && isGeneralChatDigestOn(settings),
            enabled = settings.emailNotifications,
            onCheckedChange = { change(withKindEmail(settings, "general_chat", it)) },
        )
    }
}

@Composable
private fun MonitorAlertsCard(
    settings: PingSettings,
    monitors: List<StatusMonitorAlert>,
    change: (PingSettingsChange) -> Unit,
) {
    val c = Fm.colors
    Column(Modifier.fillMaxWidth()) {
        Text(
            "Platform-wide /status monitors. Continual repeats a still-down alert every 5 minutes.",
            style = noteStyle(),
        )
        VGap(FmSpace.x3)
        ColumnCaptions("Monitor", listOf("Ping", "Email", "Repeat"), CHECK_COLUMN)
        monitors.forEachIndexed { index, monitor ->
            if (index > 0) HorizontalDivider(thickness = 1.dp, color = c.border)
            Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                Text(
                    monitor.label,
                    modifier = Modifier.weight(1f),
                    maxLines = 2,
                    overflow = TextOverflow.Ellipsis,
                    style = Fm.text.bodyMedium,
                )
                listOf(
                    Triple(MonitorAlertField.PINGS, monitor.pings, "ping"),
                    Triple(MonitorAlertField.EMAILS, monitor.emails, "email"),
                    Triple(MonitorAlertField.CONTINUAL, monitor.continual, "repeat"),
                ).forEach { (field, checked, word) ->
                    Box(Modifier.width(CHECK_COLUMN), contentAlignment = Alignment.Center) {
                        FmCheckbox(
                            checked = checked,
                            onCheckedChange = { change(withMonitorAlert(settings, monitor.id, field, it)) },
                            modifier = Modifier.semantics { contentDescription = "${monitor.label} $word" },
                        )
                    }
                }
            }
        }
    }
}
