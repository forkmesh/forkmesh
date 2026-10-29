package com.forkmesh.nativeapp.feature.chat

import android.app.ActivityManager
import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.net.ConnectivityManager
import android.net.NetworkCapabilities
import android.os.Build
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.liveRegion
import androidx.compose.ui.semantics.LiveRegionMode
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.forkmesh.nativeapp.api.PrefStore
import com.forkmesh.nativeapp.chat.CallDiagnosis
import com.forkmesh.nativeapp.chat.CallDiagnosisIssue
import com.forkmesh.nativeapp.chat.CallDiagnosisSeverity
import com.forkmesh.nativeapp.chat.CallDiagnostics
import com.forkmesh.nativeapp.chat.CallDiagnosticsEnvironment
import com.forkmesh.nativeapp.chat.CallDiagnosticsMemory
import com.forkmesh.nativeapp.chat.CallDiagnosticsTracker
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.ui.kit.FmModalBottomSheet
import com.forkmesh.nativeapp.ui.kit.FmTextButton
import com.forkmesh.nativeapp.ui.kit.FmTextField
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmColors
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmSpace

internal object CallDiagnosticsBook {
    var memory by mutableStateOf(CallDiagnosticsMemory())
        private set

    private val tracker = CallDiagnosticsTracker()

    private val prefs: PrefStore? get() = Services.get<PrefStore>()

    private fun load(): CallDiagnosticsMemory = CallDiagnosticsMemory.parse(prefs?.getString(CallDiagnostics.KEY))

    private fun save(next: CallDiagnosticsMemory) {
        prefs?.putString(CallDiagnostics.KEY, next.json)
        memory = next.clone()
    }

    fun opened(diagnosis: CallDiagnosis) {
        val next = load()
        tracker.opened(diagnosis, next)
        save(next)
    }

    fun refreshed(diagnosis: CallDiagnosis) {
        if (tracker.pending.isEmpty()) return
        val next = load()
        if (tracker.refreshed(diagnosis, next)) save(next)
    }

    fun finish() {
        if (tracker.pending.isEmpty()) return
        val next = load()
        if (tracker.finished(next)) save(next)
    }

    fun saveLesson(text: String): Boolean {
        val next = load()
        if (!CallDiagnostics.rememberNote(next, text)) return false
        save(next)
        return true
    }

    fun environment(context: Context): CallDiagnosticsEnvironment {
        val version = runCatching { context.packageManager.getPackageInfo(context.packageName, 0).versionName }.getOrNull().orEmpty()
        val agent = "ForkMesh Android $version (${Build.MANUFACTURER} ${Build.MODEL}; Android ${Build.VERSION.RELEASE}; WebView call page)"
        val info = ActivityManager.MemoryInfo()
        runCatching { context.getSystemService(ActivityManager::class.java)?.getMemoryInfo(info) }
        val memory = Math.round(info.totalMem / 1_073_741_824.0).toDouble()
        return CallDiagnosticsEnvironment(
            userAgent = agent,
            cores = Runtime.getRuntime().availableProcessors(),
            memory = memory,
            connection = connection(context),
        )
    }

    private fun connection(context: Context): String = runCatching {
        val manager = context.getSystemService(ConnectivityManager::class.java) ?: return@runCatching ""
        val caps = manager.getNetworkCapabilities(manager.activeNetwork) ?: return@runCatching ""
        val kind = when {
            caps.hasTransport(NetworkCapabilities.TRANSPORT_WIFI) -> "wifi"
            caps.hasTransport(NetworkCapabilities.TRANSPORT_ETHERNET) -> "ethernet"
            caps.hasTransport(NetworkCapabilities.TRANSPORT_CELLULAR) -> "cellular"
            caps.hasTransport(NetworkCapabilities.TRANSPORT_VPN) -> "vpn"
            else -> ""
        }
        val down = caps.linkDownstreamBandwidthKbps
        listOf(kind, if (down > 0) "${CallDiagnostics.round(down / 1000.0)} Mb/s down" else "").filter { it.isNotEmpty() }.joinToString(", ")
    }.getOrDefault("")
}

@Composable
internal fun CallDiagnosticsButton(diagnosis: CallDiagnosis) {
    var shown by remember { mutableStateOf<CallDiagnosis?>(null) }
    val bad = diagnosis.severity == CallDiagnosisSeverity.BAD
    Box(
        Modifier
            .size(44.dp)
            .clip(CircleShape)
            .clickable(role = Role.Button, onClickLabel = "Show diagnostics and how to fix them") {
                CallDiagnosticsBook.opened(diagnosis)
                shown = diagnosis
            }
            .semantics { contentDescription = "Call diagnostics: ${diagnosis.summary}" },
        contentAlignment = Alignment.Center,
    ) {
        Box(
            Modifier
                .size(26.dp)
                .clip(CircleShape)
                .background(Color(0xFF0B0B0C).copy(alpha = .55f))
                .border(if (bad) 1.dp else 0.5.dp, if (bad) FmColors.dangerOnDark else Color.White.copy(alpha = .12f), CircleShape),
            contentAlignment = Alignment.Center,
        ) {
            Box(
                Modifier
                    .size(8.dp)
                    .clip(CircleShape)
                    .background(if (diagnosis.problem) FmColors.dangerOnDark else Color.White.copy(alpha = .4f)),
            )
        }
    }
    shown?.let { opened -> CallDiagnosticsSheet(opened) { shown = null } }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun CallDiagnosticsSheet(diagnosis: CallDiagnosis, onDismiss: () -> Unit) {
    val context = LocalContext.current
    val colors = Fm.colors
    val memory = CallDiagnosticsBook.memory
    val environment = remember { CallDiagnosticsBook.environment(context) }
    val prompt = CallDiagnostics.prompt(diagnosis, memory, environment)
    var lesson by remember { mutableStateOf("") }
    var status by remember { mutableStateOf("") }
    FmModalBottomSheet(onDismissRequest = onDismiss, showDragHandle = true, isScrollControlled = true) {
        Column(Modifier.fillMaxWidth().fillMaxHeight(0.92f).navigationBarsPadding().imePadding()) {
            Row(
                Modifier.fillMaxWidth().padding(start = FmSpace.x4, end = FmSpace.x2),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Text(
                    diagnosis.heading,
                    modifier = Modifier.weight(1f).semantics { heading() },
                    style = Fm.text.titleMedium,
                )
                FmTextButton(onClick = onDismiss) { Text("Done") }
            }
            Column(
                Modifier.fillMaxWidth().verticalScroll(rememberScrollState()).padding(FmSpace.x4),
                verticalArrangement = Arrangement.spacedBy(FmSpace.x4),
            ) {
                if (diagnosis.issues.isEmpty()) {
                    val lines = CallDiagnostics.describe(diagnosis.metrics)
                    Text(
                        if (lines.isEmpty()) "Measurements start once the call connects." else lines.joinToString(". "),
                        fontSize = 15.sp,
                        color = colors.textPrimary,
                    )
                }
                diagnosis.issues.forEachIndexed { index, issue -> DiagnosisIssue(index + 1, issue, memory) }
                Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                    Text(
                        CallDiagnostics.promptLabel(diagnosis, memory),
                        fontSize = 13.sp,
                        fontWeight = FontWeight.SemiBold,
                        color = colors.textSecondary,
                    )
                    SelectionContainer {
                        Text(
                            prompt,
                            style = Fm.mono(size = 12.sp),
                            modifier = Modifier
                                .fillMaxWidth()
                                .clip(RoundedCornerShape(8.dp))
                                .background(colors.textSecondary.copy(alpha = .12f))
                                .padding(FmSpace.x3),
                        )
                    }
                    FmTextButton(onClick = {
                        val clipboard = context.getSystemService(Context.CLIPBOARD_SERVICE) as? ClipboardManager
                        clipboard?.setPrimaryClip(ClipData.newPlainText("ForkMesh call diagnostics", prompt))
                        status = if (clipboard == null) "Could not reach the clipboard." else "Copied. Paste it into your AI assistant."
                    }) {
                        FmIcon("content_copy", size = 18.dp)
                        Spacer(Modifier.size(FmSpace.x2))
                        Text("Copy prompt")
                    }
                }
                Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                    FmTextField(
                        value = lesson,
                        onValueChange = { lesson = it },
                        modifier = Modifier.fillMaxWidth().semantics { contentDescription = "Lesson from the answer" },
                        hint = "Paste the LESSON line from the answer",
                        maxLines = 4,
                    )
                    FmTextButton(onClick = {
                        if (CallDiagnosticsBook.saveLesson(lesson)) {
                            lesson = ""
                            status = "Saved. The next prompt starts from it."
                        } else {
                            status = "Paste the LESSON line first."
                        }
                    }) { Text("Save lesson") }
                }
                if (status.isNotEmpty()) {
                    Text(
                        status,
                        fontSize = 13.sp,
                        color = colors.textSecondary,
                        modifier = Modifier.semantics { liveRegion = LiveRegionMode.Polite },
                    )
                }
                Spacer(Modifier.height(FmSpace.x4))
            }
        }
    }
}

@Composable
private fun DiagnosisIssue(number: Int, issue: CallDiagnosisIssue, memory: CallDiagnosticsMemory) {
    val colors = Fm.colors
    Column(Modifier.semantics(mergeDescendants = true) {}, verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
        Row(horizontalArrangement = Arrangement.spacedBy(6.dp), verticalAlignment = Alignment.CenterVertically) {
            Box(
                Modifier
                    .size(8.dp)
                    .clip(CircleShape)
                    .background(if (issue.severity == CallDiagnosisSeverity.BAD) colors.danger else colors.warning),
            )
            Text("$number. ${issue.title}", fontSize = 15.sp, fontWeight = FontWeight.SemiBold, color = colors.textPrimary)
        }
        Text(issue.detail + " " + issue.cause, fontSize = 14.sp, color = colors.textSecondary)
        CallDiagnostics.rankFixes(issue, memory).forEachIndexed { index, fix ->
            Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                Text("${index + 1}.", fontSize = 14.sp, color = colors.textSecondary)
                Text(CallDiagnostics.fixLabel(fix), fontSize = 14.sp, color = colors.textPrimary)
            }
        }
    }
}
