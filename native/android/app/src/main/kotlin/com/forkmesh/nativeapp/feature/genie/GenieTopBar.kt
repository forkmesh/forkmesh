package com.forkmesh.nativeapp.feature.genie

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.produceState
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.rotate
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.semantics.CustomAccessibilityAction
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.customActions
import androidx.compose.ui.semantics.role
import androidx.compose.ui.semantics.selected
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.semantics.testTag
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.forkmesh.nativeapp.feature.navigation.NavigationFeature
import com.forkmesh.nativeapp.genie.GenieAutoSendReceipt
import com.forkmesh.nativeapp.genie.GenieAutoSendStage
import com.forkmesh.nativeapp.genie.GenieOutreachChannel
import com.forkmesh.nativeapp.navigation.Route
import com.forkmesh.nativeapp.ui.kit.FmInlineStatusLine
import com.forkmesh.nativeapp.ui.kit.FmModalBottomSheet
import com.forkmesh.nativeapp.ui.kit.FmSwitch
import com.forkmesh.nativeapp.ui.kit.FmTextButton
import com.forkmesh.nativeapp.ui.kit.FmTextField
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmHaptics
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmSpace
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch

private object MailChip {
    val size = 44.dp
    val badge = 16.dp
    val count = 18.dp
    val ringGap = 6.dp
}

private fun now(): Double = System.currentTimeMillis() / 1000.0

private fun symbol(stage: GenieAutoSendStage?, paused: Boolean, channel: GenieOutreachChannel?): String = when (stage) {
    GenieAutoSendStage.Finding -> "search"
    GenieAutoSendStage.Writing -> "edit"
    GenieAutoSendStage.Sending -> "send"
    GenieAutoSendStage.Sent -> "check"
    null -> if (paused) "pause" else if (channel == GenieOutreachChannel.Mastodon) "chat_bubble_outline" else "mail"
}

private fun stageLabel(stage: GenieAutoSendStage?, paused: String): String = when (stage) {
    GenieAutoSendStage.Finding -> "Picking who to email."
    GenieAutoSendStage.Writing -> "Writing the email."
    GenieAutoSendStage.Sending -> "Sending the email."
    GenieAutoSendStage.Sent -> "Sent."
    null -> if (paused.isNotEmpty()) "Auto-send is waiting: $paused" else ""
}

@OptIn(ExperimentalFoundationApi::class)
@Composable
fun ShellMailChip() {
    val on by ShellSuggestions.on.collectAsState()
    val suggestion by ShellSuggestions.contact.collectAsState()
    val autoOn by ShellAutoSend.on.collectAsState()
    val sentToday by ShellAutoSend.sentToday.collectAsState()
    val pausedFor by ShellAutoSend.pausedFor.collectAsState()
    val stage by GenieMailStatus.stage.collectAsState()
    if (!on || (suggestion == null && !autoOn && stage == null)) return
    val c = Fm.colors
    val haptics = FmHaptics.current
    val pick = suggestion?.pick
    val paused = autoOn && pausedFor.isNotEmpty()
    val busy = stage != null && stage != GenieAutoSendStage.Sent
    val sent = if (sentToday == 1) "1 email sent today" else "$sentToday emails sent today"
    var state = if (autoOn) "Auto-send on, $sent" else sent
    ShellAutoSend.countdown(now())?.let { window ->
        val minutes = maxOf(1, Math.ceil((window.end - now()) / 60).toInt())
        state += if (minutes == 1) ". Next email in 1 minute" else ". Next email in $minutes minutes"
    }
    val label = stageLabel(stage, if (paused) pausedFor else "")
    if (label.isNotEmpty()) state = "$label $state"
    val described = when {
        pick == null -> state
        autoOn -> "Next: ${pick.contact.displayName}. $state"
        else -> "${pick.caption} $state"
    }
    val toggleLabel = if (autoOn) "Turn auto-send off" else "Turn auto-send on"
    Box(
        Modifier
            .size(MailChip.size)
            .combinedClickable(
                onClick = {
                    haptics.tap()
                    ShellSuggestions.contactTapped()
                },
                onLongClick = {
                    haptics.commit()
                    ShellAutoSend.toggle()
                },
            )
            .semantics {
                role = Role.Button
                contentDescription = described
                testTag = "top-suggestion-contact"
                customActions = listOf(
                    CustomAccessibilityAction(toggleLabel) { ShellAutoSend.toggle(); true },
                    CustomAccessibilityAction("Not now") { ShellSuggestions.skipContact(); true },
                    CustomAccessibilityAction("Open the CRM") { NavigationFeature.open(Route.Crm(null)); true },
                )
            },
    ) {
        Box(Modifier.fillMaxSize().clip(CircleShape), contentAlignment = Alignment.Center) {
            val image = suggestion?.image
            if (pick != null && image != null) {
                Image(image, null, Modifier.fillMaxSize(), contentScale = ContentScale.Crop)
            } else {
                Box(Modifier.fillMaxSize().background(c.accent.copy(alpha = 0.18f)), contentAlignment = Alignment.Center) {
                    if (pick != null) {
                        Text(pick.contact.initials, color = c.accent, fontWeight = FontWeight.Bold, fontSize = 16.sp)
                    } else {
                        FmIcon("mail", size = 18.dp, tint = c.accent)
                    }
                }
            }
        }
        if (autoOn) Box(Modifier.fillMaxSize().border(2.5.dp, c.accent, CircleShape))
        if (autoOn || sentToday > 0) {
            Box(Modifier.align(Alignment.TopEnd).offset(3.dp, (-3).dp), contentAlignment = Alignment.Center) {
                if (autoOn && !busy) CountdownRing(MailChip.count + MailChip.ringGap)
                Box(
                    Modifier.size(MailChip.count).background(if (autoOn) c.accent else c.textSecondary, CircleShape),
                    contentAlignment = Alignment.Center,
                ) {
                    Text("$sentToday", color = Color.White, fontSize = if (sentToday > 99) 7.sp else 10.sp, fontWeight = FontWeight.Bold, maxLines = 1)
                }
            }
        }
        if (pick != null || stage != null || paused) {
            Box(Modifier.align(Alignment.BottomEnd).offset(2.dp, 2.dp), contentAlignment = Alignment.Center) {
                if (busy) MailSpinner(MailChip.badge + MailChip.ringGap)
                Box(
                    Modifier.size(MailChip.badge).background(if (stage == null && paused) c.warning else c.accent, CircleShape),
                    contentAlignment = Alignment.Center,
                ) {
                    FmIcon(symbol(stage, paused, pick?.channel), size = 10.dp, tint = Color.White)
                }
            }
        }
    }
}

@Composable
private fun CountdownRing(side: Dp) {
    val c = Fm.colors
    val left by produceState(1.0) {
        while (true) {
            value = ShellAutoSend.countdown(now())?.takeIf { it.duration > 0 }?.left(now()) ?: -1.0
            delay(1000)
        }
    }
    if (left < 0) return
    Canvas(Modifier.size(side)) {
        drawCircle(c.bgBase)
        val stroke = 2.dp.toPx()
        val inset = stroke / 2
        drawCircle(c.accent.copy(alpha = 0.25f), radius = size.minDimension / 2 - inset, style = Stroke(stroke))
        drawArc(
            c.accent, -90f, (360 * left).toFloat(), false, Offset(inset, inset), Size(size.width - stroke, size.height - stroke),
            style = Stroke(stroke, cap = StrokeCap.Round),
        )
    }
}

@Composable
private fun MailSpinner(side: Dp) {
    val c = Fm.colors
    val turn by produceState(0f) {
        while (true) {
            value = ((System.currentTimeMillis() % 1000) / 1000f) * 360f
            delay(16)
        }
    }
    Canvas(Modifier.size(side).rotate(turn)) {
        drawCircle(c.bgBase)
        val stroke = 2.dp.toPx()
        val inset = stroke / 2
        drawCircle(c.accent.copy(alpha = 0.25f), radius = size.minDimension / 2 - inset, style = Stroke(stroke))
        drawArc(c.accent, 0f, 108f, false, Offset(inset, inset), Size(size.width - stroke, size.height - stroke), style = Stroke(stroke, cap = StrokeCap.Round))
    }
}

@Composable
fun GenieMailNoticeLine(modifier: Modifier = Modifier) {
    val notice by GenieMailStatus.notice.collectAsState()
    val current = notice ?: return
    FmInlineStatusLine(
        current.line, current.isError,
        modifier.semantics { testTag = "shell-mail-notice" },
        onDismiss = GenieMailStatus::dismiss,
    )
}

@Composable
fun GenieAutoSendReviewHost() {
    val receipt by ShellAutoSend.review.collectAsState()
    val current = receipt ?: return
    GenieAutoSendReview(current) { ShellAutoSend.next() }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun GenieAutoSendReview(receipt: GenieAutoSendReceipt, onDone: () -> Unit) {
    val c = Fm.colors
    val haptics = FmHaptics.current
    val waiting by ShellAutoSend.waiting.collectAsState()
    val sentToday by ShellAutoSend.sentToday.collectAsState()
    var rating by remember(receipt.id) { mutableIntStateOf(0) }
    var note by remember(receipt.id) { mutableStateOf("") }
    var saving by remember(receipt.id) { mutableStateOf(false) }
    var failure by remember(receipt.id) { mutableStateOf("") }
    val scope = rememberCoroutineScope()
    FmModalBottomSheet(onDismissRequest = onDone, modifier = Modifier.semantics { testTag = "auto-send-review" }) {
        Row(Modifier.fillMaxWidth().padding(horizontal = FmSpace.x2), verticalAlignment = Alignment.CenterVertically) {
            FmTextButton(onClick = onDone, modifier = Modifier.semantics { testTag = "auto-send-review-skip" }) { Text("Skip") }
            Text(
                if (waiting > 0) "Sent · $waiting more" else "Sent",
                Modifier.weight(1f), style = Fm.text.titleMedium, textAlign = TextAlign.Center,
            )
            FmTextButton(
                onClick = {
                    saving = true
                    failure = ""
                    scope.launch {
                        val error = ShellAutoSend.rate(receipt, rating, note.trim())
                        saving = false
                        if (error != null) {
                            failure = error
                            haptics.error()
                        } else {
                            haptics.success()
                            onDone()
                        }
                    }
                },
                enabled = rating > 0 && !saving,
                modifier = Modifier.semantics { testTag = "auto-send-review-save" },
            ) { Text("Save") }
        }
        Column(
            Modifier.verticalScroll(rememberScrollState()).padding(FmSpace.x4),
            verticalArrangement = Arrangement.spacedBy(FmSpace.x3),
        ) {
            Column(Modifier.semantics(mergeDescendants = true) {}, verticalArrangement = Arrangement.spacedBy(2.dp)) {
                Text("To ${receipt.contactName} <${receipt.to}>", style = Fm.text.labelLarge, color = c.textPrimary)
                val parts = listOfNotNull(
                    receipt.fromEmail.takeIf { it.isNotEmpty() }?.let { "From $it" },
                    receipt.viaLabel.takeIf { it.isNotEmpty() }?.let { "via $it" },
                    receipt.model.takeIf { it.isNotEmpty() }?.let { "written by $it" },
                    if (sentToday == 1) "1 sent today" else "$sentToday sent today",
                )
                Text(parts.joinToString(" · "), style = Fm.text.bodySmall, color = c.textSecondary)
            }
            Text(receipt.subject, style = Fm.text.titleSmall, color = c.textPrimary, modifier = Modifier.semantics { testTag = "auto-send-review-subject" })
            Text(receipt.text, style = Fm.text.bodyMedium, color = c.textPrimary, modifier = Modifier.fillMaxWidth().semantics { testTag = "auto-send-review-body" })
            Box(Modifier.fillMaxWidth().heightIn(min = 0.5.dp).background(c.textPrimary.copy(alpha = 0.12f)))
            Row(horizontalArrangement = Arrangement.spacedBy(FmSpace.x2), modifier = Modifier.semantics { testTag = "auto-send-review-stars" }) {
                for (value in 1..5) {
                    Box(
                        Modifier
                            .size(44.dp)
                            .clip(CircleShape)
                            .clickable {
                                haptics.selection()
                                rating = value
                            }
                            .semantics {
                                role = Role.Button
                                contentDescription = if (value == 1) "1 star" else "$value stars"
                                selected = value == rating
                            },
                        contentAlignment = Alignment.Center,
                    ) {
                        FmIcon(if (value <= rating) "star" else "star_border", size = 28.dp, tint = if (value <= rating) c.accent else c.textTertiary)
                    }
                }
            }
            FmTextField(
                note, { note = it }, Modifier.fillMaxWidth().semantics { testTag = "auto-send-review-note" },
                hint = "Adjust the next ones (optional)", minLines = 2, maxLines = 4,
            )
            if (failure.isNotEmpty()) Text(failure, style = Fm.text.bodySmall, color = c.danger)
        }
    }
}

@Composable
fun SuggestionModeRow() {
    val c = Fm.colors
    val on by ShellSuggestions.on.collectAsState()
    Row(
        Modifier
            .fillMaxWidth()
            .heightIn(min = 36.dp)
            .semantics(mergeDescendants = true) { testTag = "hub-suggestion-mode" },
        verticalAlignment = Alignment.CenterVertically,
    ) {
        FmIcon("auto_awesome", size = 20.dp, tint = c.accent, outlined = true)
        Spacer(Modifier.width(FmSpace.x3))
        Text("Suggestion mode", Modifier.weight(1f), style = Fm.text.bodyLarge.copy(fontSize = Fm.text.bodyMedium.fontSize * 1.15f))
        FmSwitch(checked = on, onCheckedChange = ShellSuggestions::setOn)
    }
}

@Composable
fun SuggestionModeNote() {
    Text(
        "Shows the next best new CRM contact at the top. Tap the contact and the Genie writes to them. Touch and hold it to turn auto-send on or off: up to 100 emails a day through Zoho Mail, one about every 14 minutes while the app is open, each shown for you to rate. The number on the picture counts today's sends.",
        style = Fm.text.bodySmall, color = Fm.colors.textSecondary,
    )
}

@Composable
fun GenieSpiderSwitchRow() {
    val c = Fm.colors
    val hidden by GenieSpiderState.hidden.collectAsState()
    Row(
        Modifier.fillMaxWidth().heightIn(min = 36.dp).semantics(mergeDescendants = true) { testTag = "settings-genie-spider" },
        verticalAlignment = Alignment.CenterVertically,
    ) {
        FmIcon("bug_report", size = 20.dp, tint = c.accent, outlined = true)
        Spacer(Modifier.width(FmSpace.x3))
        Text("Show the Genie spider", Modifier.weight(1f), style = Fm.text.bodyLarge.copy(fontSize = Fm.text.bodyMedium.fontSize * 1.15f))
        FmSwitch(checked = !hidden, onCheckedChange = { GenieSpiderState.setHidden(!it) })
    }
}
