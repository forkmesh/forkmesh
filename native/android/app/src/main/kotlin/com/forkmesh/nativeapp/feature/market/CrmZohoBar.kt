package com.forkmesh.nativeapp.feature.market

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.format.relativeTime
import com.forkmesh.nativeapp.market.CrmModel
import com.forkmesh.nativeapp.market.CrmOrganizationData
import com.forkmesh.nativeapp.market.CrmZohoStatus
import com.forkmesh.nativeapp.market.CrmZohoStep
import com.forkmesh.nativeapp.ui.kit.FmCircularProgressIndicator
import com.forkmesh.nativeapp.ui.kit.FmTone
import com.forkmesh.nativeapp.ui.kit.FmToneButton
import com.forkmesh.nativeapp.ui.kit.fmGlass
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon

internal object CrmZohoText {
    fun badge(zoho: CrmZohoStatus): Pair<String, Boolean> = when {
        !zoho.connected -> "Not connected" to false
        !zoho.canSend -> "Read only" to false
        zoho.lastSyncError.isNotEmpty() -> "Sync failed" to false
        else -> "Reads and sends" to true
    }

    fun detail(zoho: CrmZohoStatus, relative: (Long) -> String): String {
        if (!zoho.connected) return "The CRM cannot read or send from your mailbox yet."
        val parts = arrayListOf(if (zoho.lastSyncAt > 0) "Synced ${relative(zoho.lastSyncAt)}" else "Never synced")
        if (zoho.lastSentAt > 0) parts.add("sent ${relative(zoho.lastSentAt)}")
        return parts.joinToString(" · ")
    }

    fun guide(canManage: Boolean): String =
        if (canManage) "Open CRM settings to connect with a Self Client code." else "Ask an organization owner or admin."
}

@Composable
internal fun CrmZohoBar(model: CrmModel, data: CrmOrganizationData, runner: CrmRunner, modifier: Modifier = Modifier) {
    val context = LocalContext.current
    val c = Fm.colors
    val zoho = data.settings.zoho
    val (label, healthy) = CrmZohoText.badge(zoho)
    val working = runner.busy > 0
    Column(
        modifier.fillMaxWidth().fmGlass(RoundedCornerShape(12.dp), shadow = false).padding(10.dp),
        verticalArrangement = Arrangement.spacedBy(6.dp),
    ) {
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(6.dp)) {
            FmIcon("mail_outlined", size = 14.dp, tint = c.textSecondary)
            Text("Zoho Mail", style = Fm.text.bodyMedium.copy(fontWeight = FontWeight.SemiBold), color = c.textPrimary)
            Text(label, style = Fm.text.labelSmall.copy(fontWeight = FontWeight.SemiBold), color = if (healthy) CrmColors.green else CrmColors.orange)
            Spacer(Modifier.weight(1f).width(4.dp))
            if (working) FmCircularProgressIndicator(Modifier.size(16.dp), strokeWidth = 2.dp)
        }
        Text(CrmZohoText.detail(zoho) { relativeTime(it) }, style = Fm.text.labelSmall, color = c.textSecondary)
        zoho.steps.forEach { step ->
            Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
                Text(step.text, style = Fm.text.labelSmall, color = c.textPrimary)
                CrmZohoStepAction(step, zoho, model.canManage, working, onSync = {
                    runner.run({ syncMail() }) { url -> if (url != null) crmOpen(context, url) }
                }, onConnect = {
                    runner.run({ connectZoho() }) { url -> if (url != null) crmOpen(context, url) }
                })
            }
        }
    }
}

@Composable
private fun CrmZohoStepAction(step: CrmZohoStep, zoho: CrmZohoStatus, canManage: Boolean, working: Boolean, onSync: () -> Unit, onConnect: () -> Unit) {
    val note: @Composable () -> Unit = {
        Text(CrmZohoText.guide(canManage), style = Fm.text.labelSmall.copy(color = Fm.colors.textSecondary, fontSize = Fm.text.labelSmall.fontSize * 0.92f))
    }
    when {
        step.action == "sync" && zoho.connected ->
            FmToneButton("Sync now", onPressed = if (working) null else onSync, icon = "refresh", height = 28.dp)
        step.action == "connect" || step.action == "reconnect" ->
            if (zoho.oneClick && canManage) {
                FmToneButton(
                    if (step.action == "connect") "Connect Zoho Mail" else "Reconnect Zoho Mail",
                    onPressed = if (working) null else onConnect,
                    icon = "link",
                    tone = FmTone.Accent,
                    filled = true,
                    height = 28.dp,
                )
            } else {
                note()
            }
        else -> note()
    }
}
