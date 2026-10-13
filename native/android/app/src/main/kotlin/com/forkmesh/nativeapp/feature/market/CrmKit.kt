package com.forkmesh.nativeapp.feature.market

import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.content.Intent
import android.net.Uri
import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.RowScope
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.forkmesh.nativeapp.format.relativeTime
import com.forkmesh.nativeapp.market.CrmColumn
import com.forkmesh.nativeapp.market.CrmContact
import com.forkmesh.nativeapp.market.CrmFlow
import com.forkmesh.nativeapp.market.CrmModel
import com.forkmesh.nativeapp.market.CrmStageLabel
import com.forkmesh.nativeapp.market.CrmText
import com.forkmesh.nativeapp.ui.kit.FmInlineStatusLine
import com.forkmesh.nativeapp.ui.kit.FmStatus
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

internal object CrmRevision {
    var value by mutableIntStateOf(0)

    fun bump() {
        value++
    }
}

internal class CrmRunner(private val scope: CoroutineScope, private val model: CrmModel) {
    var busy by mutableIntStateOf(0)
        private set
    val status: FmStatus get() = shared

    fun <T> run(work: CrmModel.() -> T, done: (T) -> Unit = {}) {
        busy++
        status.clear()
        scope.launch {
            val result = withContext(Dispatchers.IO) { model.work() }
            busy--
            CrmRevision.bump()
            val notice = model.notice
            if (notice.isNotEmpty()) {
                model.notice = ""
                status.show(notice)
            } else if ((result == null || result == false) && model.errorText.isNotEmpty()) {
                status.error(model.errorText)
            }
            done(result)
        }
    }

    companion object {
        private val shared = FmStatus()
    }
}

@Composable
internal fun CrmRunnerStatus(runner: CrmRunner, model: CrmModel, modifier: Modifier = Modifier) {
    val status = runner.status
    FmInlineStatusLine(
        status.message,
        status.isError,
        modifier,
        actionLabel = status.actionLabel,
        onAction = status::runAction,
        onDismiss = {
            if (status.isError) model.clearError()
            status.clear()
        },
    )
}

@Composable
internal fun CrmErrorLine(model: CrmModel, runner: CrmRunner) {
    ErrorLine(if (model.errorText == runner.status.message) "" else model.errorText)
}

internal object CrmColors {
    private val closed = setOf("passed", "rejected", "inactive", "closed", "lost", "churned", "declined")
    private val won = setOf("customer", "committed", "hired", "active", "won", "signed", "covered", "renewal")
    private val hues = listOf(
        Color(0xFF3B82F6), Color(0xFF6366F1), Color(0xFFA855F7), Color(0xFFF97316), Color(0xFF14B8A6), Color(0xFF10B981), Color(0xFF22C55E),
    )
    val green = Color(0xFF16A34A)
    val orange = Color(0xFFEA580C)
    val purple = Color(0xFF9333EA)
    val blue = Color(0xFF2563EB)

    fun stage(stage: String, flow: CrmFlow?): Color {
        if (stage in closed) return Color(0xFF9CA3AF)
        if (stage in won) return green
        val index = flow?.stages?.indexOf(stage) ?: -1
        return if (index < 0) Color(0xFF9CA3AF) else hues[minOf(index, hues.size - 1)]
    }

    fun person(name: String): Color {
        val palette = listOf(blue, purple, orange, green, Color(0xFFDB2777), Color(0xFF0891B2), Color(0xFF4F46E5), Color(0xFFCA8A04))
        return palette[(name.ifEmpty { "?" }.hashCode() and 0x7fffffff) % palette.size]
    }
}

internal fun crmOpen(context: Context, url: String) {
    runCatching { context.startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(url)).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)) }
}

internal fun crmShare(context: Context, text: String, subject: String = "") {
    val send = Intent(Intent.ACTION_SEND).setType("text/plain").putExtra(Intent.EXTRA_TEXT, text)
    if (subject.isNotEmpty()) send.putExtra(Intent.EXTRA_SUBJECT, subject)
    runCatching { context.startActivity(Intent.createChooser(send, null).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)) }
}

internal fun crmCopy(context: Context, text: String) {
    (context.getSystemService(Context.CLIPBOARD_SERVICE) as? ClipboardManager)?.setPrimaryClip(ClipData.newPlainText("CRM", text))
}

@Composable
internal fun CrmAvatar(name: String, size: Int = 36) {
    val initials = name.split(' ', '.', '@').filter { it.isNotEmpty() }.take(2).joinToString("") { it.first().uppercase() }.ifEmpty { "?" }
    Box(Modifier.size(size.dp).clip(CircleShape).background(CrmColors.person(name)), contentAlignment = Alignment.Center) {
        Text(initials, color = Color.White, style = Fm.text.labelSmall.copy(fontWeight = FontWeight.Bold, fontSize = maxOf(9f, size * 0.36f).sp))
    }
}

@Composable
internal fun CrmChip(text: String, tint: Color, modifier: Modifier = Modifier, icon: String? = null, filled: Boolean = false, onTap: (() -> Unit)? = null) {
    val base = modifier
        .clip(RoundedCornerShape(50))
        .background(if (filled) tint else tint.copy(alpha = 0.14f))
        .let { if (onTap != null) it.combinedClickableCompat(onTap) else it }
        .padding(horizontal = 8.dp, vertical = 3.dp)
    Row(base, verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(3.dp)) {
        if (icon != null) FmIcon(icon, size = 12.dp, tint = if (filled) Color.White else tint)
        Text(text, maxLines = 1, overflow = TextOverflow.Ellipsis, style = Fm.text.labelSmall.copy(color = if (filled) Color.White else tint, fontWeight = FontWeight.SemiBold))
    }
}

@OptIn(ExperimentalFoundationApi::class)
internal fun Modifier.combinedClickableCompat(onTap: () -> Unit, onLongPress: (() -> Unit)? = null): Modifier =
    combinedClickable(onClick = onTap, onLongClick = onLongPress)

@Composable
internal fun RowScope.CrmStatTile(value: String, label: String, icon: String, tint: Color) {
    Column(
        Modifier.weight(1f).clip(RoundedCornerShape(12.dp)).background(tint.copy(alpha = 0.09f)).padding(horizontal = 10.dp, vertical = 8.dp),
        verticalArrangement = Arrangement.spacedBy(1.dp),
    ) {
        FmIcon(icon, size = 14.dp, tint = tint)
        Text(value, maxLines = 1, style = Fm.text.titleMedium.copy(fontWeight = FontWeight.Bold), color = Fm.colors.textPrimary)
        Text(label, maxLines = 1, style = Fm.text.labelSmall, color = Fm.colors.textSecondary)
    }
}

@Composable
internal fun CrmStageDots(flow: CrmFlow, stage: String, modifier: Modifier = Modifier) {
    val current = flow.stages.indexOf(stage)
    Row(modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(3.dp)) {
        flow.stages.forEachIndexed { index, item ->
            Box(
                Modifier.weight(1f).height(4.dp).clip(RoundedCornerShape(2.dp))
                    .background(if (index <= current) CrmColors.stage(item, flow) else Fm.colors.border),
            )
        }
    }
}

@Composable
internal fun CrmContactRowContent(contact: CrmContact, flow: CrmFlow?, showStage: Boolean, columns: List<CrmColumn> = emptyList(), selected: Boolean? = null) {
    Row(Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 9.dp), verticalAlignment = Alignment.Top) {
        if (selected != null) {
            FmIcon(if (selected) "check_circle" else "radio_button_unchecked", size = 22.dp, tint = if (selected) Fm.colors.accent else Fm.colors.textTertiary)
            Spacer(Modifier.width(10.dp))
        }
        CrmAvatar(contact.name.ifEmpty { contact.email }, 36)
        Spacer(Modifier.width(10.dp))
        Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(2.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                Text(contact.displayName, Modifier.weight(1f, fill = false), maxLines = 1, overflow = TextOverflow.Ellipsis, style = Fm.text.bodyLarge.copy(fontWeight = FontWeight.SemiBold), color = Fm.colors.textPrimary)
                if (showStage) CrmChip(CrmStageLabel.title(contact.stage), CrmColors.stage(contact.stage, flow))
                Spacer(Modifier.weight(1f))
                if (contact.value > 0) Text(CrmText.money(contact.value), style = Fm.text.labelSmall.copy(fontWeight = FontWeight.Bold), color = CrmColors.green)
            }
            if (contact.subtitle.isNotEmpty()) Text(contact.subtitle, maxLines = 1, overflow = TextOverflow.Ellipsis, style = Fm.text.bodySmall, color = Fm.colors.textSecondary)
            if (contact.nextStep.isNotEmpty()) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    FmIcon("subdirectory_arrow_right", size = 13.dp, tint = Fm.colors.textSecondary)
                    Spacer(Modifier.width(3.dp))
                    Text(contact.nextStep, maxLines = 1, overflow = TextOverflow.Ellipsis, style = Fm.text.bodySmall, color = Fm.colors.textPrimary)
                }
            }
            columns.take(2).forEach { column ->
                val value = contact.valueOf(column.field)
                if (value.isNotEmpty()) Text("${column.label}: $value", maxLines = 1, overflow = TextOverflow.Ellipsis, style = Fm.text.labelSmall, color = Fm.colors.textSecondary)
            }
            if (contact.tags.isNotEmpty() || contact.owner.isNotEmpty() || contact.lastContactedAt > 0) {
                Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(4.dp)) {
                    contact.tags.take(3).forEach { CrmChip("#$it", CrmColors.purple) }
                    if (contact.owner.isNotEmpty()) CrmChip(contact.owner, Fm.colors.textSecondary, icon = "person")
                    Spacer(Modifier.weight(1f))
                    if (contact.lastContactedAt > 0) Text(relativeTime(contact.lastContactedAt), style = Fm.text.labelSmall, color = Fm.colors.textTertiary)
                }
            }
        }
    }
}

@Composable
internal fun CrmCard(modifier: Modifier = Modifier, content: @Composable () -> Unit) {
    Box(modifier.clip(RoundedCornerShape(14.dp)).background(Fm.colors.bgRaised).border(1.dp, Fm.colors.border, RoundedCornerShape(14.dp)).padding(10.dp)) {
        content()
    }
}
