package com.forkmesh.nativeapp.feature.account.settings

import com.forkmesh.nativeapp.ui.kit.FmMessenger
import android.text.format.DateFormat
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.DatePicker
import androidx.compose.material3.DatePickerDialog
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.FilterChip
import androidx.compose.material3.FilterChipDefaults
import androidx.compose.material3.SelectableDates
import androidx.compose.material3.Text
import androidx.compose.material3.TimePicker
import androidx.compose.material3.rememberDatePickerState
import androidx.compose.material3.rememberTimePickerState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.forkmesh.nativeapp.feature.account.AuthService
import com.forkmesh.nativeapp.feature.account.HGap
import com.forkmesh.nativeapp.feature.account.VGap
import com.forkmesh.nativeapp.model.StatusClearAfter
import com.forkmesh.nativeapp.model.USER_STATUS_PRESETS
import com.forkmesh.nativeapp.model.USER_STATUS_QUICK_EMOJI
import com.forkmesh.nativeapp.model.USER_STATUS_TEXT_MAX
import com.forkmesh.nativeapp.model.UserStatus
import com.forkmesh.nativeapp.model.UserStatusException
import com.forkmesh.nativeapp.model.statusClearAfterFor
import com.forkmesh.nativeapp.model.statusCustomExpiresAt
import com.forkmesh.nativeapp.model.statusExpiresAt
import com.forkmesh.nativeapp.model.userStatusAwayBody
import com.forkmesh.nativeapp.model.userStatusClearBody
import com.forkmesh.nativeapp.model.userStatusDraftError
import com.forkmesh.nativeapp.model.userStatusErrorMessage
import com.forkmesh.nativeapp.model.userStatusExpiryLabel
import com.forkmesh.nativeapp.model.userStatusLabel
import com.forkmesh.nativeapp.model.userStatusSetBody
import com.forkmesh.nativeapp.ui.kit.FmAlertDialog
import com.forkmesh.nativeapp.ui.kit.FmField
import com.forkmesh.nativeapp.ui.kit.FmSoundOnShow
import com.forkmesh.nativeapp.ui.kit.FmSwitch
import com.forkmesh.nativeapp.ui.kit.FmTextButton
import com.forkmesh.nativeapp.ui.kit.FmTextField
import com.forkmesh.nativeapp.ui.kit.FmInlineStatus
import com.forkmesh.nativeapp.ui.kit.LocalFmMessenger
import com.forkmesh.nativeapp.ui.kit.rememberFmStatus
import com.forkmesh.nativeapp.ui.kit.fmTapSounds
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmRadius
import com.forkmesh.nativeapp.ui.theme.FmSpace
import com.forkmesh.nativeapp.ui.theme.FmType
import java.time.Instant
import java.time.LocalDate
import java.time.LocalTime
import java.time.ZoneId
import java.time.ZoneOffset
import java.time.format.DateTimeFormatter
import kotlin.coroutines.cancellation.CancellationException
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch

@Composable
internal fun rememberNowMillis(periodMs: Long = 30_000): Long {
    var now by remember { mutableLongStateOf(System.currentTimeMillis()) }
    LaunchedEffect(periodMs) {
        while (true) {
            delay(periodMs)
            now = System.currentTimeMillis()
        }
    }
    return now
}

suspend fun showUserStatusSheet(messenger: FmMessenger, auth: AuthService) {
    if (auth.session.value == null || auth.isGuest.value) return
    messenger.showModalBottomSheet<Unit>(showDragHandle = true, isScrollControlled = true) {
        Column(Modifier.fillMaxWidth().padding(horizontal = FmSpace.x4, vertical = FmSpace.x3)) {
            Text("Your status", style = Fm.text.titleMedium)
            VGap(FmSpace.x3)
            UserStatusControls(auth)
            VGap(FmSpace.x4)
        }
    }
}

@Composable
internal fun UserStatusControls(auth: AuthService) {
    val c = Fm.colors
    val messenger = LocalFmMessenger.current
    val scope = rememberCoroutineScope()
    val status by auth.userStatus.collectAsState()
    val now = rememberNowMillis()
    val context = LocalContext.current
    val notice = rememberFmStatus()
    var pendingAway by remember { mutableStateOf<Boolean?>(null) }

    Column(Modifier.fillMaxWidth()) {
        Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.Top) {
            Column(Modifier.weight(1f)) {
                Text("Away", style = Fm.text.bodyMedium.copy(fontWeight = FmType.emphasis))
                VGap(FmSpace.x1)
                Text(
                    "Let people know you're not around. You stay away until you turn this off.",
                    style = Fm.text.bodySmall.copy(color = c.textSecondary, lineHeight = FmType.meta * FmType.leadingSnug),
                )
            }
            HGap(FmSpace.x3)
            FmSwitch(
                checked = pendingAway ?: status.away,
                enabled = pendingAway == null,
                onCheckedChange = { away ->
                    pendingAway = away
                    notice.clear()
                    scope.launch {
                        try {
                            auth.updateUserStatus(userStatusAwayBody(away))
                        } catch (cancelled: CancellationException) {
                            throw cancelled
                        } catch (failure: Exception) {
                            notice.error(failure.message ?: userStatusErrorMessage(""))
                        } finally {
                            pendingAway = null
                        }
                    }
                },
                modifier = Modifier.testTag("settings-away"),
            )
        }
        VGap(FmSpace.x3)
        val label = userStatusLabel(status, now)
        val expiry = userStatusExpiryLabel(status.visible(now).expiresAt, now, use24Hour = DateFormat.is24HourFormat(context))
        Row(
            Modifier
                .fillMaxWidth()
                .background(c.bgBase, RoundedCornerShape(FmRadius.md))
                .clickable(onClickLabel = "Set a status", role = Role.Button) {
                    scope.launch {
                        val message = messenger.showDialog<String> { done -> UserStatusDialog(auth, status, done) }
                        if (!message.isNullOrEmpty()) notice.show(message)
                    }
                }
                .padding(FmSpace.x3)
                .testTag("settings-set-status"),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            val emoji = status.displayEmoji(now)
            Box(Modifier.size(28.dp), contentAlignment = Alignment.Center) {
                if (emoji.isNotEmpty()) {
                    Text(emoji, style = TextStyle(fontFamily = FmType.ui, fontSize = 20.sp))
                } else {
                    FmIcon("add_reaction_outlined", size = 20.dp, tint = c.textSecondary)
                }
            }
            HGap(FmSpace.x3)
            Column(Modifier.weight(1f)) {
                Text(
                    label.ifEmpty { "Set a status" },
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                    style = Fm.text.bodyMedium.copy(
                        fontWeight = FmType.emphasis,
                        color = if (label.isEmpty()) c.textSecondary else c.textPrimary,
                    ),
                )
                if (expiry.isNotEmpty()) {
                    VGap(FmSpace.x1)
                    Text(expiry, style = Fm.text.bodySmall.copy(color = c.textTertiary))
                }
            }
            FmIcon("chevron_right", size = 18.dp, tint = c.textTertiary)
        }
        FmInlineStatus(notice, Modifier.padding(top = FmSpace.x2))
    }
}

private fun utcDayMillis(date: LocalDate): Long = date.atStartOfDay(ZoneOffset.UTC).toInstant().toEpochMilli()

@OptIn(ExperimentalLayoutApi::class, ExperimentalMaterial3Api::class)
@Composable
private fun UserStatusDialog(auth: AuthService, initial: UserStatus, done: (String?) -> Unit) {
    val c = Fm.colors
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    val zone = remember { ZoneId.systemDefault() }
    val use24Hour = remember { DateFormat.is24HourFormat(context) }
    val openedAt = remember { System.currentTimeMillis() }
    val shown = remember { initial.visible(openedAt) }
    var emoji by remember { mutableStateOf(shown.emoji) }
    var text by remember { mutableStateOf(shown.text) }
    var clearAfter by remember { mutableStateOf(statusClearAfterFor(shown.expiresAt)) }
    var customAt by remember {
        val fallback = Instant.ofEpochMilli(openedAt).atZone(zone).plusHours(1).withMinute(0).withSecond(0).withNano(0)
        mutableLongStateOf(if (shown.expiresAt > openedAt) shown.expiresAt else fallback.toInstant().toEpochMilli())
    }
    var busy by remember { mutableStateOf(false) }
    var error by remember { mutableStateOf<String?>(null) }
    var pickingDate by remember { mutableStateOf(false) }
    var pickingTime by remember { mutableStateOf(false) }
    val now = rememberNowMillis()

    val empty = emoji.isBlank() && text.isBlank()
    val previewExpiresAt = if (empty) 0 else statusExpiresAt(clearAfter, now, zone, customAt)

    fun send(payload: Map<String, Any?>, success: String) {
        busy = true
        error = null
        scope.launch {
            try {
                auth.updateUserStatus(payload)
                done(success)
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (refused: UserStatusException) {
                error = refused.message
            } catch (failure: Exception) {
                error = userStatusErrorMessage("")
            } finally {
                busy = false
            }
        }
    }

    fun save() {
        if (busy) return
        if (empty) {
            send(userStatusClearBody(), "Status cleared.")
            return
        }
        val at = System.currentTimeMillis()
        val expiresAt = statusExpiresAt(clearAfter, at, zone, customAt)
        val refused = userStatusDraftError(emoji, text, expiresAt, at)
        if (refused != null) {
            error = userStatusErrorMessage(refused)
            return
        }
        send(userStatusSetBody(emoji, text, expiresAt), "Status updated.")
    }

    FmAlertDialog(
        onDismissRequest = { if (!busy) done(null) },
        title = "Set a status",
        containerColor = c.bgOverlay,
        shape = RoundedCornerShape(FmRadius.lg),
        actions = {
            if (initial.hasCustom) {
                FmTextButton(
                    onClick = { if (!busy) send(userStatusClearBody(), "Status cleared.") },
                    contentColor = c.danger,
                ) { Text("Clear status") }
            }
            Spacer(Modifier.weight(1f))
            FmTextButton(onClick = { if (!busy) done(null) }) { Text("Cancel") }
            Button(onClick = { save() }, enabled = !busy) { Text(if (busy) "Saving…" else "Save") }
        },
        content = {
            Column(
                Modifier
                    .width(480.dp)
                    .verticalScroll(rememberScrollState()),
            ) {
                FlowRow(
                    horizontalArrangement = Arrangement.spacedBy(FmSpace.x1),
                    verticalArrangement = Arrangement.spacedBy(FmSpace.x1),
                ) {
                    for (pick in USER_STATUS_QUICK_EMOJI) {
                        EmojiPick(pick, selected = emoji.trim() == pick) {
                            emoji = if (emoji.trim() == pick) "" else pick
                            error = null
                        }
                    }
                }
                VGap(FmSpace.x3)
                FmField(label = "Emoji") {
                    FmTextField(
                        value = emoji,
                        onValueChange = {
                            emoji = it
                            error = null
                        },
                        hint = "Type or paste one emoji",
                        keyboardOptions = KeyboardOptions(imeAction = ImeAction.Next),
                    )
                }
                VGap(FmSpace.x3)
                FmField(label = "Status") {
                    FmTextField(
                        value = text,
                        onValueChange = {
                            val oneLine = it.replace('\n', ' ').replace('\r', ' ')
                            text = if (oneLine.codePointCount(0, oneLine.length) <= USER_STATUS_TEXT_MAX) {
                                oneLine
                            } else {
                                oneLine.substring(0, oneLine.offsetByCodePoints(0, USER_STATUS_TEXT_MAX))
                            }
                            error = null
                        },
                        hint = "What's your status?",
                        helperText = "${text.codePointCount(0, text.length)}/$USER_STATUS_TEXT_MAX",
                    )
                }
                VGap(FmSpace.x3)
                Text("Clear after", style = Fm.text.bodySmall.copy(fontWeight = FmType.emphasis, color = c.textPrimary))
                VGap(FmSpace.x2)
                FlowRow(
                    horizontalArrangement = Arrangement.spacedBy(FmSpace.x2),
                    verticalArrangement = Arrangement.spacedBy(FmSpace.x1),
                ) {
                    for (choice in StatusClearAfter.entries) {
                        ClearAfterChip(choice.label, selected = clearAfter == choice) {
                            clearAfter = choice
                            error = null
                        }
                    }
                }
                if (clearAfter == StatusClearAfter.Custom) {
                    val custom = Instant.ofEpochMilli(customAt).atZone(zone)
                    VGap(FmSpace.x2)
                    Row(horizontalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                        PickerButton(
                            icon = "calendar_today_outlined",
                            label = custom.format(DateTimeFormatter.ofPattern("EEE, MMM d, yyyy")),
                            onClick = { pickingDate = true },
                        )
                        PickerButton(
                            icon = "access_time_outlined",
                            label = custom.format(DateTimeFormatter.ofPattern(if (use24Hour) "HH:mm" else "h:mm a")),
                            onClick = { pickingTime = true },
                        )
                    }
                }
                VGap(FmSpace.x2)
                Text(
                    when {
                        empty -> "Nothing to show: saving clears your status."
                        previewExpiresAt == 0L -> "Stays until you clear it."
                        else -> userStatusExpiryLabel(previewExpiresAt, now, zone, use24Hour).ifEmpty { "That time has passed." }
                    },
                    style = Fm.text.bodySmall.copy(color = c.textTertiary),
                )
                VGap(FmSpace.x4)
                Text("Presets", style = Fm.text.bodySmall.copy(fontWeight = FmType.emphasis, color = c.textPrimary))
                VGap(FmSpace.x1)
                for (preset in USER_STATUS_PRESETS) {
                    Text(
                        preset.label,
                        modifier = Modifier
                            .fillMaxWidth()
                            .clickable(onClickLabel = preset.label, role = Role.Button) {
                                emoji = preset.emoji
                                text = preset.text
                                clearAfter = preset.clearAfter
                                error = null
                            }
                            .padding(vertical = FmSpace.x2),
                        style = Fm.text.bodyMedium.copy(color = c.textPrimary),
                    )
                }
                error?.let { message ->
                    VGap(FmSpace.x3)
                    Text(message, style = Fm.text.bodyMedium.copy(color = c.danger))
                }
            }
        },
    )

    if (pickingDate) {
        val today = remember { LocalDate.now(zone) }
        val current = Instant.ofEpochMilli(customAt).atZone(zone)
        val state = rememberDatePickerState(
            initialSelectedDateMillis = utcDayMillis(current.toLocalDate()),
            selectableDates = remember(today) {
                object : SelectableDates {
                    override fun isSelectableDate(utcTimeMillis: Long): Boolean {
                        val day = Instant.ofEpochMilli(utcTimeMillis).atZone(ZoneOffset.UTC).toLocalDate()
                        return !day.isBefore(today) && !day.isAfter(today.plusDays(365))
                    }

                    override fun isSelectableYear(year: Int): Boolean = year in today.year..today.year + 1
                }
            },
        )
        FmSoundOnShow()
        DatePickerDialog(
            onDismissRequest = { pickingDate = false },
            modifier = Modifier.fmTapSounds(),
            confirmButton = {
                FmTextButton(onClick = {
                    state.selectedDateMillis?.let { picked ->
                        val day = Instant.ofEpochMilli(picked).atZone(ZoneOffset.UTC).toLocalDate()
                        customAt = statusCustomExpiresAt(day, current.toLocalTime(), zone)
                        error = null
                    }
                    pickingDate = false
                }) { Text("OK") }
            },
            dismissButton = { FmTextButton(onClick = { pickingDate = false }) { Text("Cancel") } },
        ) {
            DatePicker(state = state)
        }
    }

    if (pickingTime) {
        val current = Instant.ofEpochMilli(customAt).atZone(zone)
        val state = rememberTimePickerState(initialHour = current.hour, initialMinute = current.minute, is24Hour = use24Hour)
        FmAlertDialog(
            onDismissRequest = { pickingTime = false },
            title = "Clear at",
            containerColor = c.bgOverlay,
            shape = RoundedCornerShape(FmRadius.lg),
            actions = {
                FmTextButton(onClick = { pickingTime = false }) { Text("Cancel") }
                FmTextButton(onClick = {
                    customAt = statusCustomExpiresAt(current.toLocalDate(), LocalTime.of(state.hour, state.minute), zone)
                    error = null
                    pickingTime = false
                }) { Text("OK") }
            },
            content = { TimePicker(state = state) },
        )
    }
}

@Composable
private fun EmojiPick(emoji: String, selected: Boolean, onClick: () -> Unit) {
    val c = Fm.colors
    val shape = RoundedCornerShape(FmRadius.md)
    Box(
        Modifier
            .size(44.dp)
            .background(if (selected) c.accentSubtle else Color.Transparent, shape)
            .border(1.dp, if (selected) c.accent else c.border, shape)
            .clickable(onClickLabel = "Use $emoji", role = Role.Button, onClick = onClick),
        contentAlignment = Alignment.Center,
    ) {
        Text(emoji, style = TextStyle(fontFamily = FmType.ui, fontSize = 22.sp))
    }
}

@Composable
private fun ClearAfterChip(label: String, selected: Boolean, onClick: () -> Unit) {
    val c = Fm.colors
    FilterChip(
        selected = selected,
        onClick = onClick,
        label = { Text(label) },
        leadingIcon = if (selected) {
            { FmIcon("check", size = 18.dp) }
        } else {
            null
        },
        colors = FilterChipDefaults.filterChipColors(
            containerColor = Color.Transparent,
            labelColor = c.textPrimary,
            selectedContainerColor = c.bgBase,
            selectedLabelColor = c.textPrimary,
            selectedLeadingIconColor = c.textPrimary,
        ),
        border = FilterChipDefaults.filterChipBorder(
            enabled = true,
            selected = selected,
            borderColor = c.border,
            selectedBorderColor = c.accent,
        ),
    )
}

@Composable
private fun PickerButton(icon: String, label: String, onClick: () -> Unit) {
    val c = Fm.colors
    val shape = RoundedCornerShape(FmRadius.md)
    Row(
        Modifier
            .border(1.dp, c.border, shape)
            .clickable(onClickLabel = label, role = Role.Button, onClick = onClick)
            .padding(horizontal = FmSpace.x3, vertical = FmSpace.x2),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        FmIcon(icon, size = 16.dp, tint = c.textSecondary)
        HGap(FmSpace.x2)
        Text(label, style = Fm.text.bodyMedium.copy(color = c.textPrimary))
    }
}
