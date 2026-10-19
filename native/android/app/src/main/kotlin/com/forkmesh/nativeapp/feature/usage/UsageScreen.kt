package com.forkmesh.nativeapp.feature.usage

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.Text
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
import androidx.compose.ui.platform.LocalContext
import com.forkmesh.nativeapp.agents.agentProviderLabel
import com.forkmesh.nativeapp.agents.ops.AgentUsageModel
import com.forkmesh.nativeapp.agents.ops.UsageAccount
import com.forkmesh.nativeapp.agents.ops.UsageFormat
import com.forkmesh.nativeapp.agents.ops.UsageProviderMeter
import com.forkmesh.nativeapp.agents.ops.UsageSummary
import com.forkmesh.nativeapp.feature.agents.AgentOpsFeature
import com.forkmesh.nativeapp.feature.agents.AgentsFeature
import com.forkmesh.nativeapp.feature.agents.ChipRow
import com.forkmesh.nativeapp.feature.agents.DesktopTerminalScreen
import com.forkmesh.nativeapp.feature.agents.RunnerAccountsFeature
import com.forkmesh.nativeapp.feature.agents.UsageWindowRows
import com.forkmesh.nativeapp.feature.agents.opsIo
import com.forkmesh.nativeapp.feature.agents.rememberCenterTick
import com.forkmesh.nativeapp.feature.agents.rememberOpsTick
import com.forkmesh.nativeapp.feature.code.DetailScaffold
import com.forkmesh.nativeapp.format.relativeTime
import com.forkmesh.nativeapp.ui.kit.FmCard
import com.forkmesh.nativeapp.ui.kit.FmCircularProgressIndicator
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenu
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenuItem
import com.forkmesh.nativeapp.ui.kit.FmEmptyState
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmMessenger
import com.forkmesh.nativeapp.ui.kit.FmPullToRefreshBox
import com.forkmesh.nativeapp.ui.kit.FmStatusBadge
import com.forkmesh.nativeapp.ui.kit.FmTone
import com.forkmesh.nativeapp.ui.kit.FmToneButton
import com.forkmesh.nativeapp.ui.kit.LocalFmMessenger
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmRadius
import com.forkmesh.nativeapp.ui.theme.FmSpace
import com.forkmesh.nativeapp.ui.theme.FmType
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.launch

internal class UsageScreen : Screen() {
    override val title: String get() = "Usage"
    private val model = AgentUsageModel(AgentOpsFeature.client)
    private var refreshing by mutableStateOf(false)

    @Composable
    override fun Content(navigator: Navigator) {
        val context = LocalContext.current
        val scope = rememberCoroutineScope()
        val messenger = LocalFmMessenger.current
        val tick = rememberOpsTick(model)
        val center = AgentsFeature.control
        val centerTick = rememberCenterTick(center)
        val runnerAccounts by RunnerAccountsFeature.state.collectAsState()
        fun reload(pulled: Boolean, reread: Boolean = true) {
            if (model.loading) return
            refreshing = pulled
            scope.launch {
                try {
                    opsIo { model.refresh(reread) }
                } finally {
                    refreshing = false
                }
                AgentOpsFeature.scheduleUsageReminders(context.applicationContext, model.reminders, model.nowMs)
            }
        }
        LaunchedEffect(Unit) { reload(false, reread = false) }
        tick.let { }
        centerTick.let { }
        val actions = AccountActions(navigator, scope, messenger)
        DetailScaffold(
            title,
            navigator,
            actions = {
                FmIconButton("refresh", "Refresh", onPressed = if (model.loading) null else ({ reload(false) }), color = Fm.colors.textPrimary)
            },
        ) {
            FmPullToRefreshBox(isRefreshing = refreshing, onRefresh = { reload(true) }, modifier = Modifier.fillMaxSize()) {
                val summary = model.summary
                val paired = center.hub.desktop != null
                LazyColumn(
                    Modifier.fillMaxSize(),
                    contentPadding = PaddingValues(FmSpace.x4),
                    verticalArrangement = Arrangement.spacedBy(FmSpace.x3),
                ) {
                    when {
                        !paired && summary == null -> item {
                            FmEmptyState(
                                "desktop_access_disabled",
                                "No desktop connected",
                                message = "Sign in to ForkMesh on your desktop with this account, then pull down to refresh.",
                            )
                        }
                        summary == null && model.loading -> item {
                            Box(Modifier.fillMaxWidth().padding(FmSpace.x6), contentAlignment = Alignment.Center) { FmCircularProgressIndicator() }
                        }
                        summary == null && model.unsupported -> item {
                            FmEmptyState(
                                "update",
                                "Update ForkMesh on the desktop",
                                message = "This desktop doesn't report model usage yet. Update its ForkMesh app, then pull down to refresh.",
                            )
                        }
                        summary == null -> item {
                            FmEmptyState(
                                "cloud_off",
                                "Couldn't read usage",
                                message = model.errorText.ifEmpty { "Pull down or tap Refresh to try again." },
                            )
                        }
                        else -> {
                            item(key = "header") { Header(center.hub.reach.label, summary, model.errorText) }
                            item(key = "actions") {
                                ChipRow {
                                    FmToneButton("New agent in terminal", onPressed = { navigator.push(DesktopTerminalScreen()) }, icon = "terminal", tone = FmTone.Accent)
                                    if (runnerAccounts.hasAccounts) {
                                        FmToneButton("Runner accounts", onPressed = { RunnerAccountsFeature.open(navigator) }, icon = "manage_accounts")
                                    }
                                }
                            }
                            if (summary.providers.isEmpty()) {
                                item(key = "empty") {
                                    FmEmptyState(
                                        "speed",
                                        "No usage yet",
                                        message = "This desktop has no Claude Code or Codex account usage yet. Run an agent there, then refresh.",
                                    )
                                }
                            }
                            items(summary.providers, key = { "provider-${it.id}" }) { provider ->
                                ProviderCard(provider, summary, model.nowMs, model.busy, actions)
                            }
                        }
                    }
                }
            }
        }
    }

    private inner class AccountActions(val navigator: Navigator, val scope: CoroutineScope, val messenger: FmMessenger) {
        fun select(provider: UsageProviderMeter, account: UsageAccount) {
            scope.launch { opsIo { model.selectAccount(provider.id, account.id) } }
        }

        fun rename(provider: UsageProviderMeter, account: UsageAccount) {
            scope.launch {
                val label = messenger.showPromptDialog(
                    "Rename ${account.name}",
                    hint = "Account name",
                    initialValue = account.label,
                    confirmLabel = "Rename",
                ) ?: return@launch
                if (label.isEmpty() || label == account.label) return@launch
                opsIo { model.renameAccount(provider.id, account.id, label) }
            }
        }

        fun add(provider: UsageProviderMeter) {
            scope.launch {
                val label = messenger.showPromptDialog(
                    "Add a ${provider.label} account",
                    body = "Name it, for example \"Work\". The desktop keeps a separate sign-in for it and starts the provider's own sign-in next.",
                    hint = "Account name",
                    confirmLabel = "Add",
                ) ?: return@launch
                if (label.isEmpty()) return@launch
                val created = opsIo { model.addAccount(provider.id, label) } ?: return@launch
                signIn(provider, UsageAccount(created, label, active = true), "login")
            }
        }

        fun delete(provider: UsageProviderMeter, account: UsageAccount) {
            scope.launch {
                val confirmed = messenger.showConfirmDialog(
                    "Delete ${account.name}?",
                    "The desktop forgets this ${provider.label} account and its sign-in. Agents already running keep going.",
                    confirmLabel = "Delete",
                    destructive = true,
                )
                if (confirmed) opsIo { model.deleteAccount(provider.id, account.id) }
            }
        }

        fun signIn(provider: UsageProviderMeter, account: UsageAccount, mode: String) {
            navigator.push(DesktopTerminalScreen(provider = provider.id, account = account.id, accountLabel = account.name, mode = mode))
        }

        fun logOut(provider: UsageProviderMeter, account: UsageAccount) {
            scope.launch {
                val confirmed = messenger.showConfirmDialog(
                    "Log ${account.name} out of ${provider.label}?",
                    "The desktop runs the provider's own sign-out for this account. Agents that use it stop working until you log in again.",
                    confirmLabel = "Log out",
                    destructive = true,
                )
                if (confirmed) signIn(provider, account, "logout")
            }
        }

        fun terminal(provider: UsageProviderMeter, account: UsageAccount?) {
            navigator.push(DesktopTerminalScreen(provider = provider.id, account = account?.id.orEmpty(), accountLabel = account?.name.orEmpty()))
        }
    }

    @Composable
    private fun Header(desktop: String, summary: UsageSummary, error: String) {
        val c = Fm.colors
        Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x1)) {
            Text(desktop, style = Fm.text.bodyMedium.copy(color = c.textPrimary, fontWeight = FmType.emphasis))
            val line = listOfNotNull(
                summary.atMs.takeIf { it > 0 }?.let { "Updated ${relativeTime(it)}" },
                summary.totalSpendUsd?.let { "${UsageFormat.usd(it)} spent" },
            ).joinToString(" · ")
            if (line.isNotEmpty()) Text(line, style = Fm.text.bodySmall.copy(color = c.textSecondary))
            if (error.isNotEmpty()) Text(error, style = Fm.text.bodySmall.copy(color = c.danger))
        }
    }

    @Composable
    private fun ProviderCard(provider: UsageProviderMeter, summary: UsageSummary, nowMs: Long, busy: Boolean, actions: AccountActions) {
        val c = Fm.colors
        FmCard(Modifier.fillMaxWidth(), radius = FmRadius.lg) {
            Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x3)) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text(
                        provider.label.ifEmpty { agentProviderLabel(provider.id) },
                        Modifier.weight(1f),
                        style = Fm.text.titleSmall.copy(color = c.textPrimary),
                    )
                    summary.spendUsd(provider.id)?.let { Text(UsageFormat.usd(it), style = Fm.text.bodySmall.copy(color = c.textSecondary)) }
                }
                if (summary.usedForNewAgents(provider.id)) {
                    Text("Used for new agents", style = Fm.text.bodySmall.copy(color = c.success))
                }
                provider.credits?.let { Text(it.label, style = Fm.text.bodySmall.copy(color = c.textSecondary)) }
                if (provider.accounts.isEmpty()) {
                    if (provider.windows.isEmpty()) Muted("No current usage reading. Run ${provider.label} on this desktop, then refresh.")
                    else Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x1)) { UsageWindowRows(provider.windows, nowMs) }
                }
                for (account in provider.accounts) AccountBlock(provider, account, nowMs, busy, actions)
                ChipRow {
                    FmToneButton("New ${provider.label} agent", onPressed = { actions.terminal(provider, null) }, icon = "terminal")
                    FmToneButton("Add account", onPressed = if (busy) null else ({ actions.add(provider) }), icon = "person_add")
                }
            }
        }
    }

    @Composable
    private fun AccountBlock(provider: UsageProviderMeter, account: UsageAccount, nowMs: Long, busy: Boolean, actions: AccountActions) {
        val c = Fm.colors
        var menu by remember { mutableStateOf(false) }
        Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x1)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(account.name, Modifier.weight(1f), style = Fm.text.bodyMedium.copy(color = c.textPrimary, fontWeight = FmType.emphasis))
                FmStatusBadge(
                    account.statusLabel,
                    when {
                        account.signedOut -> c.warning
                        account.active -> c.success
                        else -> c.textTertiary
                    },
                )
                Spacer(Modifier.width(FmSpace.x1))
                Box {
                    FmIconButton("more_vert", "More for ${account.name}", onPressed = { menu = true }, color = c.textSecondary)
                    FmDropdownMenu(expanded = menu, onDismissRequest = { menu = false }) {
                        FmDropdownMenuItem(
                            text = if (account.signedOut) "Log in" else "Log in again",
                            icon = "login",
                            onClick = { menu = false; actions.signIn(provider, account, "login") },
                        )
                        if (!account.signedOut) {
                            FmDropdownMenuItem(text = "Log out", icon = "logout", onClick = { menu = false; actions.logOut(provider, account) })
                        }
                        FmDropdownMenuItem(text = "Rename", icon = "edit", enabled = !busy, onClick = { menu = false; actions.rename(provider, account) })
                        if (!account.isDefault) {
                            FmDropdownMenuItem(
                                text = "Delete",
                                icon = "delete",
                                enabled = !busy,
                                textColor = c.danger,
                                onClick = { menu = false; actions.delete(provider, account) },
                            )
                        }
                    }
                }
            }
            Text(account.detail, style = Fm.text.bodySmall.copy(color = if (account.signedOut) c.warning else c.textSecondary))
            val windows = provider.windowsFor(account)
            if (windows.isEmpty()) Muted("No current usage reading. Run the account on this desktop, then refresh.")
            else UsageWindowRows(windows, nowMs)
            ChipRow {
                if (account.signedOut) {
                    FmToneButton("Log in", onPressed = { actions.signIn(provider, account, "login") }, icon = "login", tone = FmTone.Accent)
                } else {
                    FmToneButton("Terminal as this account", onPressed = { actions.terminal(provider, account) }, icon = "terminal")
                }
                if (!account.active) {
                    FmToneButton("Use for new agents", onPressed = if (busy) null else ({ actions.select(provider, account) }), icon = "check_circle")
                }
            }
        }
    }

    @Composable
    private fun Muted(text: String) {
        Text(text, style = Fm.text.bodySmall.copy(color = Fm.colors.textTertiary))
    }
}
