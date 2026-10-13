package com.forkmesh.nativeapp.feature.agents

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.feature.code.DetailScaffold
import com.forkmesh.nativeapp.runners.RunnerAccount
import com.forkmesh.nativeapp.runners.RunnerAccounts
import com.forkmesh.nativeapp.runners.RunnerAccountsSection
import com.forkmesh.nativeapp.runners.RunnerAccountsState
import com.forkmesh.nativeapp.runners.RunnerAttention
import com.forkmesh.nativeapp.ui.kit.FmCard
import com.forkmesh.nativeapp.ui.kit.FmEmptyState
import com.forkmesh.nativeapp.ui.kit.FmSectionHeader
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmRadius
import com.forkmesh.nativeapp.ui.theme.FmSpace

internal class RunnerAccountsScreen : Screen() {
    override val title: String get() = "Runner accounts"

    @Composable
    override fun Content(navigator: Navigator) {
        val state by RunnerAccountsFeature.state.collectAsState()
        LaunchedEffect(Unit) { RunnerAccountsFeature.refresh(force = true) }
        DetailScaffold(title, navigator) {
            val sections = state.sections
            when {
                state.hidden -> FmEmptyState(
                    "manage_accounts",
                    "Runner accounts",
                    message = "Sign in to your ForkMesh account to manage runner accounts.",
                )
                !state.loaded && state.error.isEmpty() -> Column(
                    Modifier.fillMaxSize(),
                    verticalArrangement = Arrangement.Center,
                    horizontalAlignment = Alignment.CenterHorizontally,
                ) { CircularProgressIndicator(color = Fm.colors.accent) }
                else -> Column(
                    Modifier
                        .fillMaxSize()
                        .navigationBarsPadding()
                        .verticalScroll(rememberScrollState())
                        .padding(FmSpace.x4)
                        .testTag("runner-accounts"),
                    verticalArrangement = Arrangement.spacedBy(FmSpace.x3),
                ) {
                    Text(
                        "Choose the Claude Code or Codex account each runner uses. The runner applies a switch on its next check-in.",
                        style = Fm.text.bodySmall.copy(color = Fm.colors.textSecondary),
                    )
                    StatusLines(state)
                    if (sections.isEmpty() && state.loaded) {
                        Text(
                            "No runner has Claude Code or Codex installed.",
                            style = Fm.text.bodyMedium.copy(color = Fm.colors.textSecondary),
                        )
                    }
                    val now = System.currentTimeMillis()
                    for ((runner, group) in sections.groupBy { it.runner }) {
                        FmSectionHeader(runner, icon = "dns")
                        for (section in group) SectionCard(navigator, section, now)
                    }
                }
            }
        }
    }

    @Composable
    private fun StatusLines(state: RunnerAccountsState) {
        val c = Fm.colors
        if (state.error.isNotEmpty()) Text(state.error, style = Fm.text.bodySmall.copy(color = c.danger))
        if (state.notice.isNotEmpty()) {
            Text(state.notice, style = Fm.text.bodySmall.copy(color = if (state.noticeError) c.danger else c.textSecondary))
        }
    }

    @Composable
    private fun SectionCard(navigator: Navigator, section: RunnerAccountsSection, nowMs: Long) {
        val c = Fm.colors
        FmCard(Modifier.fillMaxWidth().testTag("runner-accounts-${section.key}"), radius = FmRadius.lg) {
            Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x1)) {
                Text(
                    RunnerAttention.providerLong(section.provider),
                    style = Fm.text.titleSmall.copy(color = c.textPrimary, fontWeight = FontWeight.SemiBold),
                )
                for (account in section.accounts) {
                    AccountRow(section, account, account.id in section.pendingAccounts, nowMs)
                }
                ActionRow(
                    label = RunnerAccounts.addLabel(section),
                    icon = if (section.accounts.isEmpty()) "login" else "person_add",
                    enabled = section.canSignIn,
                    onClick = { RunnerAccountsFeature.signIn(navigator, section.runner, section.provider) },
                )
            }
        }
    }

    @Composable
    private fun AccountRow(section: RunnerAccountsSection, account: RunnerAccount, pending: Boolean, nowMs: Long) {
        val c = Fm.colors
        val canSwitch = !account.active && !pending
        Row(
            Modifier
                .fillMaxWidth()
                .heightIn(min = 48.dp)
                .clickable(enabled = canSwitch, role = Role.RadioButton) {
                    RunnerAccountsFeature.activate(section.runner, section.provider, account)
                }
                .padding(vertical = FmSpace.x2)
                .testTag("runner-account-${section.runner}-${account.id}"),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            when {
                pending -> CircularProgressIndicator(Modifier.padding(2.dp).size(18.dp), strokeWidth = 2.dp, color = c.accent)
                account.active -> FmIcon("check_circle", size = 22.dp, tint = c.accent, contentDescription = "Active")
                else -> FmIcon("radio_button_unchecked", size = 22.dp, tint = c.textTertiary)
            }
            Spacer(Modifier.width(FmSpace.x3))
            Column(Modifier.weight(1f)) {
                Text(
                    RunnerAccounts.accountTitle(account, pending = false),
                    style = Fm.text.bodyMedium.copy(
                        color = if (pending) c.textSecondary else c.textPrimary,
                        fontWeight = if (account.active) FontWeight.SemiBold else FontWeight.Normal,
                    ),
                )
                Text(RunnerAccounts.usageText(account, nowMs), style = Fm.text.bodySmall.copy(color = c.textTertiary))
                if (pending) {
                    Text("Switching…", style = Fm.text.labelSmall.copy(color = c.accent, fontWeight = FontWeight.SemiBold))
                } else if (canSwitch) {
                    Text(
                        RunnerAccounts.accountHint(account, section.provider, section.runner),
                        style = Fm.text.labelSmall.copy(color = c.textTertiary),
                    )
                }
            }
        }
    }

    @Composable
    private fun ActionRow(label: String, icon: String, enabled: Boolean, onClick: () -> Unit) {
        val c = Fm.colors
        val tint = if (enabled) c.accent else c.textDisabled
        Row(
            Modifier
                .fillMaxWidth()
                .heightIn(min = 44.dp)
                .clickable(enabled = enabled, onClick = onClick)
                .padding(vertical = FmSpace.x2),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            FmIcon(icon, size = 20.dp, tint = tint)
            Spacer(Modifier.width(FmSpace.x3))
            Text(label, style = Fm.text.bodyMedium.copy(color = tint, fontWeight = FontWeight.SemiBold))
        }
    }
}

@Composable
internal fun RunnerAccountsEntry(navigator: Navigator, modifier: Modifier = Modifier) {
    val state by RunnerAccountsFeature.state.collectAsState()
    LaunchedEffect(Unit) { RunnerAccountsFeature.refresh() }
    if (!state.hasAccounts) return
    val c = Fm.colors
    val pending = state.anyPending
    Row(
        modifier
            .fillMaxWidth()
            .background(c.bgRaised)
            .clickable { RunnerAccountsFeature.open(navigator) }
            .padding(horizontal = FmSpace.x4, vertical = FmSpace.x2)
            .testTag("runner-accounts-entry"),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        FmIcon("manage_accounts", size = 18.dp, tint = c.textSecondary)
        Spacer(Modifier.width(FmSpace.x2))
        Text(
            "Runner accounts",
            modifier = Modifier.weight(1f),
            style = Fm.text.bodyMedium.copy(color = c.textPrimary, fontWeight = FontWeight.SemiBold),
            maxLines = 1,
        )
        Text(
            if (pending) "Switching…" else "${RunnerAccounts.accountCount(state.runners)} saved",
            style = Fm.text.bodySmall.copy(color = c.textSecondary),
        )
        Spacer(Modifier.width(FmSpace.x1))
        FmIcon("chevron_right", size = 20.dp, tint = c.textDisabled)
    }
}
