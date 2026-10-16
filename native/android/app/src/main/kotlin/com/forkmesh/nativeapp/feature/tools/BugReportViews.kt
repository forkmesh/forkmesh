package com.forkmesh.nativeapp.feature.tools

import android.provider.OpenableColumns
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.detectDragGestures
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.interaction.collectIsPressedAsState
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.asPaddingValues
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawing
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.scale
import androidx.compose.ui.draw.shadow
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.api.BugReportAttachment
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.account.AccountFilledButton
import com.forkmesh.nativeapp.feature.account.AuthService
import com.forkmesh.nativeapp.feature.account.SettingsService
import com.forkmesh.nativeapp.tools.BugButtonGeometry
import com.forkmesh.nativeapp.tools.BugButtonPoint
import com.forkmesh.nativeapp.tools.ReportBugCopy
import com.forkmesh.nativeapp.tools.ReportBugDraft
import com.forkmesh.nativeapp.tools.sendBugReport
import com.forkmesh.nativeapp.ui.kit.FmField
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmInlineStatusLine
import com.forkmesh.nativeapp.ui.kit.FmModalBottomSheet
import com.forkmesh.nativeapp.ui.kit.FmTextField
import com.forkmesh.nativeapp.ui.kit.FmTone
import com.forkmesh.nativeapp.ui.kit.FmToneButton
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmRadius
import com.forkmesh.nativeapp.ui.theme.FmSpace
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.ByteArrayOutputStream
import kotlin.math.roundToInt

@Composable
internal fun BugReportOverlayView(screen: String, sharedText: MutableStateFlow<String?>, content: @Composable () -> Unit) {
    val settings = Services.get<SettingsService>()
    val shared by sharedText.collectAsState()
    var sheetOpen by remember { mutableStateOf(false) }
    var prefill by remember { mutableStateOf("") }
    if (shared != null) {
        prefill = shared ?: ""
        sharedText.value = null
        sheetOpen = true
    }
    BoxWithConstraints {
        content()
        if (settings != null) {
            val density = LocalDensity.current
            val insets = WindowInsets.safeDrawing.asPaddingValues()
            val anchor by settings.bugButtonAnchor.collectAsState()
            val onLeft by settings.bugButtonOnLeft.collectAsState()
            val geometry = BugButtonGeometry(
                layerWidth = maxWidth.value,
                layerHeight = maxHeight.value,
                safeTop = insets.calculateTopPadding().value,
                safeBottom = insets.calculateBottomPadding().value,
            )
            var dragAt by remember { mutableStateOf<BugButtonPoint?>(null) }
            val at = dragAt ?: geometry.restingAt(anchor, onLeft)
            val source = remember { MutableInteractionSource() }
            val pressed by source.collectIsPressedAsState()
            val active = dragAt != null || pressed
            val opacity by animateFloatAsState(if (active) 1f else BugButtonGeometry.IDLE_OPACITY, label = "bugOpacity")
            val scale by animateFloatAsState(if (dragAt != null) 1.08f else if (pressed) 0.94f else 1f, label = "bugScale")
            val c = Fm.colors
            Box(
                Modifier
                    .offset { IntOffset(with(density) { at.x.dp.roundToPx() }, with(density) { at.y.dp.roundToPx() }) }
                    .pointerInput(geometry) {
                        detectDragGestures(
                            onDragEnd = {
                                val released = dragAt
                                dragAt = null
                                if (released != null) {
                                    val placement = geometry.released(released)
                                    settings.setBugButtonPlacement(placement.anchor, placement.onLeft)
                                }
                            },
                            onDragCancel = { dragAt = null },
                        ) { change, amount ->
                            change.consume()
                            val from = dragAt ?: geometry.restingAt(anchor, onLeft)
                            dragAt = geometry.dragged(from, amount.x / density.density, amount.y / density.density)
                        }
                    }
                    .alpha(opacity)
                    .scale(scale)
                    .shadow(if (dragAt != null) 6.dp else 2.dp, CircleShape)
                    .clip(CircleShape)
                    .background(c.bgElevated)
                    .border(1.dp, c.border, CircleShape)
                    .size(BugButtonGeometry.SIZE_DP.dp)
                    .clickable(interactionSource = source, indication = null, role = Role.Button) {
                        prefill = ""
                        sheetOpen = true
                    }
                    .semantics { contentDescription = ReportBugCopy.BUTTON_LABEL },
                contentAlignment = Alignment.Center,
            ) {
                FmIcon("bug_report_outlined", size = 21.dp, tint = c.textSecondary)
            }
        }
    }
    if (sheetOpen) {
        ReportBugSheet(screen = screen, initialMessage = prefill) { sheetOpen = false }
    }
}

@OptIn(ExperimentalLayoutApi::class, androidx.compose.material3.ExperimentalMaterial3Api::class)
@Composable
internal fun ReportBugSheet(
    screen: String,
    initialMessage: String = "",
    packageId: String? = null,
    packageName: String? = null,
    onClosed: () -> Unit,
) {
    val context = LocalContext.current
    var draft by remember { mutableStateOf(ReportBugDraft(message = initialMessage)) }
    var sent by remember { mutableStateOf(false) }
    var attachments by remember { mutableStateOf(emptyList<BugReportAttachment>()) }
    val scope = rememberCoroutineScope()
    val signedIn = Services.get<AuthService>()?.isAuthenticated ?: false
    val c = Fm.colors
    val files = rememberLauncherForActivityResult(ActivityResultContracts.OpenMultipleDocuments()) { uris ->
        scope.launch {
            val picked = withContext(Dispatchers.IO) {
                uris.take(5 - attachments.size).mapNotNull { uri ->
                    runCatching {
                        val resolver = context.contentResolver
                        val name = resolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use { cursor ->
                            if (cursor.moveToFirst()) cursor.getString(0) else null
                        } ?: uri.lastPathSegment ?: "attachment"
                        val data = resolver.openInputStream(uri)?.use { input ->
                            val out = ByteArrayOutputStream()
                            val chunk = ByteArray(64 * 1024)
                            while (out.size() <= 4 * 1024 * 1024) {
                                val count = input.read(chunk)
                                if (count < 0) break
                                out.write(chunk, 0, count)
                            }
                            out.toByteArray()
                        } ?: return@runCatching null
                        if (data.isEmpty() || data.size > 4 * 1024 * 1024) return@runCatching null
                        BugReportAttachment(name, resolver.getType(uri) ?: "", data)
                    }.getOrNull()
                }
            }
            attachments = attachments + picked
        }
    }
    fun submit() {
        val step = draft.submit(apiAvailable = Services.get<ApiService>() != null)
        draft = step.draft
        if (!step.send) return
        val api = Services.require<ApiService>()
        scope.launch {
            val result = withContext(Dispatchers.IO) { sendBugReport(api, draft, screen, DiagnosticsRuntime.build, packageId, attachments) }
            if (!result.ok) draft = draft.refused(result.message) else sent = true
        }
    }
    FmModalBottomSheet(onDismissRequest = { if (!draft.busy) onClosed() }, isScrollControlled = true, containerColor = c.bgRaised) {
        Column(Modifier.imePadding()) {
            Row(Modifier.padding(start = FmSpace.x4, top = FmSpace.x4, end = FmSpace.x3, bottom = FmSpace.x3), verticalAlignment = Alignment.CenterVertically) {
                Column(Modifier.weight(1f)) {
                    Text(ReportBugCopy.title(packageName), style = Fm.text.titleLarge.copy(color = c.textPrimary))
                    Spacer(Modifier.height(2.dp))
                    Text(ReportBugCopy.subtitle(packageName), style = Fm.text.bodySmall.copy(color = c.textTertiary))
                }
                FmIconButton("close", ReportBugCopy.CLOSE, onPressed = if (draft.busy) null else onClosed, color = c.textPrimary)
            }
            if (sent) {
                FmInlineStatusLine(ReportBugCopy.THANKS, isError = false, modifier = Modifier.padding(horizontal = FmSpace.x4, vertical = FmSpace.x3))
                AccountFilledButton(
                    "Done",
                    "check",
                    onClick = onClosed,
                    modifier = Modifier.fillMaxWidth().padding(horizontal = FmSpace.x4, vertical = FmSpace.x3).height(48.dp),
                )
            } else {
                Column(
                    Modifier
                        .weight(1f, fill = false)
                        .verticalScroll(rememberScrollState())
                        .padding(start = FmSpace.x4, end = FmSpace.x4, bottom = FmSpace.x4),
                ) {
                    FmTextField(
                        value = draft.message,
                        onValueChange = { draft = draft.withMessage(it) },
                        hint = ReportBugCopy.messageHint(packageName),
                        minLines = 4,
                        maxLines = 8,
                        autofocus = true,
                        enabled = !draft.busy,
                    )
                    Spacer(Modifier.height(FmSpace.x4))
                    FmField(label = ReportBugCopy.SEVERITY_LABEL) {
                        FlowRow(horizontalArrangement = Arrangement.spacedBy(FmSpace.x2), verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                            for (severity in ReportBugCopy.SEVERITIES) {
                                val selected = severity.value == draft.severity
                                FmToneButton(
                                    severity.label,
                                    onPressed = if (draft.busy) null else ({ draft = draft.withSeverity(severity.value) }),
                                    tone = if (selected) FmTone.Accent else FmTone.Neutral,
                                    filled = selected,
                                )
                            }
                        }
                    }
                    Spacer(Modifier.height(FmSpace.x4))
                    FmField(label = ReportBugCopy.contactLabel(signedIn)) {
                        FmTextField(
                            value = draft.contact,
                            onValueChange = { draft = draft.withContact(it) },
                            hint = ReportBugCopy.contactHint(signedIn),
                            enabled = !draft.busy,
                            keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Email),
                        )
                    }
                    Spacer(Modifier.height(FmSpace.x3))
                    FmToneButton("Add files", onPressed = if (draft.busy || attachments.size >= 5) null else ({ files.launch(arrayOf("*/*")) }), tone = FmTone.Neutral)
                    if (attachments.isNotEmpty()) {
                        Spacer(Modifier.height(FmSpace.x2))
                        Text(
                            attachments.joinToString(" · ") { it.name },
                            style = Fm.text.bodySmall.copy(color = c.textTertiary),
                        )
                    }
                    Spacer(Modifier.height(FmSpace.x3))
                    Row(verticalAlignment = Alignment.Top) {
                        FmIcon("info_outline_rounded", size = 15.dp, tint = c.textDisabled)
                        Spacer(Modifier.width(FmSpace.x2))
                        Text(
                            ReportBugCopy.attached(DiagnosticsRuntime.build, screen, signedIn),
                            style = Fm.text.bodySmall.copy(color = c.textTertiary),
                        )
                    }
                    if (draft.problem.isNotEmpty()) {
                        Spacer(Modifier.height(FmSpace.x3))
                        Row(
                            Modifier
                                .fillMaxWidth()
                                .background(c.danger.copy(alpha = .12f), RoundedCornerShape(FmRadius.md))
                                .padding(FmSpace.x3),
                            verticalAlignment = Alignment.Top,
                        ) {
                            FmIcon("error_outline_rounded", size = 16.dp, tint = c.danger)
                            Spacer(Modifier.width(FmSpace.x2))
                            Text(draft.problem, style = Fm.text.bodySmall.copy(color = c.textPrimary))
                        }
                    }
                }
                AccountFilledButton(
                    if (draft.busy) ReportBugCopy.SENDING else ReportBugCopy.SEND,
                    "send_rounded",
                    onClick = ::submit,
                    enabled = !draft.busy,
                    modifier = Modifier.fillMaxWidth().padding(horizontal = FmSpace.x4, vertical = FmSpace.x3).height(48.dp),
                )
            }
        }
    }
}
