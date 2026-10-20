package com.forkmesh.nativeapp.feature.market

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.feature.tools.ToolsAppBar
import com.forkmesh.nativeapp.market.CrmModel
import com.forkmesh.nativeapp.market.CrmNoteDraft
import com.forkmesh.nativeapp.market.CrmZohoCode
import com.forkmesh.nativeapp.market.MarketingDates
import com.forkmesh.nativeapp.ui.kit.FmCard
import com.forkmesh.nativeapp.ui.kit.FmConfirmDialog
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenu
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenuItem
import com.forkmesh.nativeapp.ui.kit.FmListRow
import com.forkmesh.nativeapp.ui.kit.FmTextButton
import com.forkmesh.nativeapp.ui.kit.FmTextField
import com.forkmesh.nativeapp.ui.kit.FmTone
import com.forkmesh.nativeapp.ui.kit.FmToneButton
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import java.time.ZoneId

@Composable
internal fun CrmLastContactedField(value: Long, onChange: (Long) -> Unit) {
    val zone = remember { ZoneId.systemDefault() }
    var text by remember { mutableStateOf(if (value > 0) MarketingDates.text(value, true, zone) else "") }
    val invalid = text.isNotBlank() && MarketingDates.parse(text, zone) == null
    Row(verticalAlignment = Alignment.CenterVertically) {
        FmTextField(
            text,
            { next ->
                text = next
                if (next.isBlank()) onChange(0) else MarketingDates.parse(next, zone)?.let(onChange)
            },
            Modifier.weight(1f), hint = "Last contacted (YYYY-MM-DD HH:MM)", errorText = if (invalid) "Write it as YYYY-MM-DD or YYYY-MM-DD HH:MM." else null,
        )
        FmTextButton(onClick = {
            val now = System.currentTimeMillis()
            text = MarketingDates.text(now, true, zone)
            onChange(now)
        }) { Text("Now") }
    }
}

class CrmNoteDraftsScreen(private val model: CrmModel) : Screen() {
    override val title: String get() = "Unsent drafts"

    private var replying by mutableStateOf<CrmNoteDraft?>(null)

    @Composable
    override fun Content(navigator: Navigator) {
        val scope = rememberCoroutineScope()
        val context = LocalContext.current
        val runner = remember { CrmRunner(scope, model) }
        @Suppress("UNUSED_EXPRESSION") CrmRevision.value
        val drafts = model.noteDrafts
        Column(Modifier.fillMaxSize().background(Fm.colors.canvas)) {
            ToolsAppBar("Unsent drafts", navigator)
            Column(Modifier.verticalScroll(rememberScrollState()).padding(16.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
                CrmRunnerStatus(runner, model)
                if (drafts.isEmpty()) {
                    Text("No unsent drafts. A draft is a contact note paragraph that starts with \"${CrmNoteDraft.MARK}\"", style = Fm.text.bodyMedium, color = Fm.colors.textSecondary)
                } else {
                    val mastodon = drafts.count { it.mastodon }
                    Text("${drafts.size} unsent: ${drafts.size - mastodon} by email, $mastodon as Mastodon replies. Sending an email draft marks it sent in the contact's notes.", style = Fm.text.labelSmall, color = Fm.colors.textSecondary)
                }
                drafts.forEach { draft ->
                    FmCard {
                        Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                            Row {
                                Text(draft.name, Modifier.weight(1f), style = Fm.text.bodyMedium, color = Fm.colors.textPrimary)
                                Text(draft.kindTitle, style = Fm.text.labelSmall, color = Fm.colors.textSecondary)
                            }
                            if (draft.to.isNotEmpty()) Text(draft.to, style = Fm.text.labelSmall, color = Fm.colors.textSecondary)
                            Text(if (draft.subject.isEmpty()) draft.body else draft.subject + " — " + draft.body, style = Fm.text.bodySmall, color = Fm.colors.textPrimary, maxLines = 3)
                            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                                FmToneButton(if (draft.mastodon) "Reply on Mastodon" else "Send", onPressed = if (draft.canSend) ({
                                    if (draft.mastodon) {
                                        crmCopy(context, draft.body)
                                        crmOpen(context, draft.thread)
                                        replying = draft
                                    } else {
                                        navigator.push(CrmComposeScreen(model, draft.emailDraft(model.contact(draft.contactId)), false))
                                    }
                                }) else null, tone = FmTone.Accent, filled = true)
                                FmToneButton("Mark sent", onPressed = { runner.run({ markNoteDraftSent(draft.contactId, draft.mastodon) }) })
                            }
                            if (!draft.canSend) Text(if (draft.mastodon) "No outreach toot is linked to this contact." else "The contact has no email address.", style = Fm.text.labelSmall, color = CrmColors.orange)
                        }
                    }
                }
                CrmErrorLine(model, runner)
            }
        }
        replying?.let { draft ->
            FmConfirmDialog(
                title = "Mark this draft as sent?",
                body = "The reply is on the clipboard and the thread is open. Paste it there and post it, then mark the draft sent.",
                confirmLabel = "Mark sent",
                onDismissRequest = { replying = null },
                onConfirm = {
                    replying = null
                    runner.run({ markNoteDraftSent(draft.contactId, true) })
                },
            )
        }
    }
}

class CrmZohoCodeScreen(private val model: CrmModel) : Screen() {
    override val title: String get() = "Zoho Self Client"

    private var code by mutableStateOf(CrmZohoCode.of(model.data?.settings?.zoho))
    private var regionMenu by mutableStateOf(false)

    @Composable
    override fun Content(navigator: Navigator) {
        val scope = rememberCoroutineScope()
        val context = LocalContext.current
        val runner = remember { CrmRunner(scope, model) }
        Column(Modifier.fillMaxSize().background(Fm.colors.canvas)) {
            ToolsAppBar("Zoho Self Client", navigator) {
                FmTextButton(onClick = { runner.run({ exchangeZoho(code) }) { ok -> if (ok) navigator.pop() } }, enabled = code.isComplete && runner.busy == 0) { Text("Connect") }
            }
            Column(Modifier.verticalScroll(rememberScrollState()).padding(16.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
                Text(
                    "In the Zoho API Console choose Add Client → Self Client → Create, and copy its client ID and secret. On Generate Code enter the scope below, pick 10 minutes and copy the code.",
                    style = Fm.text.bodySmall, color = Fm.colors.textSecondary,
                )
                FmToneButton("Open the Zoho API Console", onPressed = { crmOpen(context, CrmZohoCode.CONSOLE_URL) }, icon = "open_in_new")
                FmListRow(CrmZohoCode.SCOPES, subtitle = "Scope (tap to copy)", onTap = { crmCopy(context, CrmZohoCode.SCOPES) }, showChevron = false)
                FmTextField(code.clientId, { code = code.copy(clientId = it) }, hint = "Client ID")
                FmTextField(code.clientSecret, { code = code.copy(clientSecret = it) }, hint = if (code.usesSavedSecret) "Saved secret (leave blank to keep)" else "Client secret", visualTransformation = PasswordVisualTransformation())
                FmTextField(code.code, { code = code.copy(code = it) }, hint = "Code")
                Box {
                    FmListRow(code.region, subtitle = "Data center", onTap = { regionMenu = true })
                    FmDropdownMenu(expanded = regionMenu, onDismissRequest = { regionMenu = false }) {
                        CrmZohoCode.REGIONS.forEach { region -> FmDropdownMenuItem(region, onClick = { regionMenu = false; code = code.copy(region = region) }) }
                    }
                }
                Text(
                    "ForkMesh redeems the code within its 10 minutes, tries every Zoho data center and reads the mail account. The secret and refresh token are stored encrypted.",
                    style = Fm.text.labelSmall, color = Fm.colors.textTertiary,
                )
                CrmErrorLine(model, runner)
                CrmRunnerStatus(runner, model)
            }
        }
    }
}
