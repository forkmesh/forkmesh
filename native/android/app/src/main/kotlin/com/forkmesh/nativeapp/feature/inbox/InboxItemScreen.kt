package com.forkmesh.nativeapp.feature.inbox

import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.account.pingDetailWhen
import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.collab.ReferenceLinks
import com.forkmesh.nativeapp.control.HubControlCalling
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.agents.AgentsFeature
import com.forkmesh.nativeapp.feature.code.rememberCopyText
import com.forkmesh.nativeapp.feature.navigation.NavigationFeature
import com.forkmesh.nativeapp.feature.navigation.RoutePage
import com.forkmesh.nativeapp.inbox.DesktopPingFixes
import com.forkmesh.nativeapp.inbox.InboxItem
import com.forkmesh.nativeapp.inbox.InboxRoutes
import com.forkmesh.nativeapp.inbox.InboxSource
import com.forkmesh.nativeapp.inbox.InboxSyncState
import com.forkmesh.nativeapp.inbox.PingQuickFix
import com.forkmesh.nativeapp.inbox.PingQuickFixes
import com.forkmesh.nativeapp.navigation.Route
import com.forkmesh.nativeapp.platform.ProjectLifecycle
import com.forkmesh.nativeapp.ui.kit.FmCard
import com.forkmesh.nativeapp.ui.kit.FmConfirmDialog
import com.forkmesh.nativeapp.ui.kit.FmEmptyState
import com.forkmesh.nativeapp.ui.kit.FmIconTile
import com.forkmesh.nativeapp.ui.kit.FmInlineStatus
import com.forkmesh.nativeapp.ui.kit.FmKeyValueRow
import com.forkmesh.nativeapp.ui.kit.FmListRow
import com.forkmesh.nativeapp.ui.kit.FmRowDivider
import com.forkmesh.nativeapp.ui.kit.FmSectionHeader
import com.forkmesh.nativeapp.ui.kit.LinkedText
import com.forkmesh.nativeapp.ui.kit.rememberFmStatus
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmSpace
import com.forkmesh.nativeapp.feature.agents.RunnerAttentionFeature
import com.forkmesh.nativeapp.runners.RunnerAttention
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

internal fun inboxWebLink(item: InboxItem): String? {
    val origin = Services.get<ApiService>()?.gitOrigin.orEmpty()
    return if (origin.isEmpty()) item.href.takeIf { it.startsWith("http") } else InboxRoutes.webLink(item.href, origin)
}

class InboxItemScreen(private val item: InboxItem) : Screen() {
    override val title: String get() = item.kindLabel

    @Composable
    override fun Content(navigator: Navigator) {
        val c = Fm.colors
        val model = InboxFeature.model
        val context = LocalContext.current
        val status = rememberFmStatus()
        val copy = rememberCopyText(status)
        var confirmDelete by remember { mutableStateOf(false) }
        val scope = rememberCoroutineScope()
        val fixClient = remember(item.id) {
            if (item.fromDesktop) runCatching { AgentsFeature.hub }.getOrNull()?.let { DesktopPingFixes(HubControlCalling(it)) } else null
        }
        var quickFixes by remember(item.id) { mutableStateOf(PingQuickFixes()) }
        var checkingFixes by remember(item.id) { mutableStateOf(fixClient != null) }
        var fixing by remember(item.id) { mutableStateOf<String?>(null) }
        LaunchedEffect(item.id) {
            if (model != null && item.unread) {
                withContext(Dispatchers.IO) { model.markRead(item) }
                InboxFeature.publish()
            }
        }
        LaunchedEffect(item.id) {
            val client = fixClient ?: return@LaunchedEffect
            quickFixes = withContext(Dispatchers.IO) { runCatching { client.fixes(item) }.getOrNull() } ?: PingQuickFixes()
            checkingFixes = false
        }
        val applyFix: (PingQuickFix) -> Unit = applyFix@{ fix ->
            val client = fixClient ?: return@applyFix
            if (fixing != null) return@applyFix
            fixing = fix.id
            status.clear()
            scope.launch {
                val result = withContext(Dispatchers.IO) { runCatching { client.apply(fix, item) } }
                result.onSuccess {
                    quickFixes = it
                    status.show(it.message.ifEmpty { "Done" })
                }.onFailure { status.error(DesktopPingFixes.explain(it)) }
                fixing = null
            }
        }
        RoutePage(item.kindLabel, navigator) {
            if (model == null) {
                FmEmptyState("inbox", "Inbox", message = "Sign in to see what needs you.")
                return@RoutePage
            }
            val web = inboxWebLink(item)
            val link = web ?: item.href.trim()
            val route = item.route
            Column(
                Modifier
                    .fillMaxSize()
                    .verticalScroll(rememberScrollState())
                    .padding(horizontal = FmSpace.x4, vertical = FmSpace.x3),
            ) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    FmIconTile(item.icon, tint = c.accent, size = 40.dp, iconSize = 20.dp)
                    Spacer(Modifier.width(FmSpace.x3))
                    Column(Modifier.weight(1f)) {
                        Text(item.kindLabel, style = Fm.text.labelSmall.copy(color = c.textTertiary))
                        val whenLabel = pingDetailWhen(item.ts)
                        if (whenLabel.isNotEmpty()) {
                            Text(whenLabel, Modifier.testTag("ping-detail-time"), style = Fm.text.bodySmall.copy(color = c.textSecondary))
                        }
                    }
                }
                Spacer(Modifier.height(FmSpace.x3))
                SelectionContainer {
                    Column {
                        Text(item.title, style = Fm.text.titleLarge.copy(color = c.textPrimary))
                        if (item.body.isNotEmpty()) {
                            Spacer(Modifier.height(FmSpace.x2))
                            LinkedText(item.body, ReferenceLinks.repoFor(slug = item.repo), style = Fm.text.bodyMedium.copy(color = c.textSecondary), commits = false)
                        }
                    }
                }
                if (item.sync == InboxSyncState.Pending || item.sync == InboxSyncState.Failed) {
                    Spacer(Modifier.height(FmSpace.x2))
                    Text(
                        if (item.sync == InboxSyncState.Pending) "Waiting to sync" else "Could not sync",
                        style = Fm.text.labelSmall.copy(color = if (item.sync == InboxSyncState.Failed) c.danger else c.textTertiary),
                    )
                }
                if (item.repo.isNotEmpty() || item.actor.isNotEmpty() || link.isNotEmpty()) {
                    Spacer(Modifier.height(FmSpace.x3))
                    if (item.repo.isNotEmpty()) FmKeyValueRow("Repository", item.repo)
                    if (item.actor.isNotEmpty()) FmKeyValueRow("From", "@${item.actor}")
                    if (link.isNotEmpty()) FmKeyValueRow("Link", link)
                }
                Spacer(Modifier.height(FmSpace.x4))
                FmSectionHeader("Actions")
                Spacer(Modifier.height(FmSpace.x2))
                FmCard(Modifier.fillMaxWidth(), padding = PaddingValues()) {
                    Column {
                        if (checkingFixes && quickFixes.fixes.isEmpty()) {
                            FmListRow(
                                title = "Looking for a quick fix\u2026",
                                modifier = Modifier.testTag("ping-detail-fix-checking"),
                                leading = { FmIconTile("build_outlined", tint = c.textTertiary) },
                                showChevron = false,
                                titleColor = c.textTertiary,
                            )
                            FmRowDivider()
                        }
                        for (fix in quickFixes.fixes) {
                            FmListRow(
                                title = if (fixing == fix.id) "${fix.label}\u2026" else fix.label,
                                modifier = Modifier.testTag(if (fix.primary) "ping-detail-fix" else "ping-detail-fix-${fix.id}"),
                                subtitle = listOf(fix.detail, if (fix.trusted) "Auto" else "").filter { it.isNotEmpty() }.joinToString(" \u00B7 ").ifEmpty { null },
                                leading = { FmIconTile(fix.materialIcon, tint = if (fix.primary) c.accent else c.textSecondary) },
                                onTap = { applyFix(fix) },
                                showChevron = false,
                                titleColor = if (fix.primary) c.accent else null,
                            )
                            FmRowDivider()
                        }
                        val runnerTarget = if (RunnerAttention.isRunnerPing(item.kind, item.href)) RunnerAttention.pingTarget(item.href) else null
                        if (runnerTarget != null) {
                            ActionRow("login", RunnerAttention.connectLabel(runnerTarget.provider), "ping-detail-runner-sign-in") {
                                RunnerAttentionFeature.open(navigator, runnerTarget) { url -> NavigationFeature.openUri(context, url) }
                            }
                            FmRowDivider()
                        }
                        if (route != null) {
                            ActionRow("open_in_new", "Open", "ping-detail-open") { NavigationFeature.open(route) }
                            FmRowDivider()
                        } else if (web != null) {
                            ActionRow("open_in_browser", "Open in browser", "ping-detail-open") { NavigationFeature.openUri(context, web) }
                            FmRowDivider()
                        }
                        ActionRow("send_outlined", "Send to agent", "ping-detail-agent") { NavigationFeature.compose(model.prompt(item)) }
                        FmRowDivider()
                        ActionRow("copy_all_outlined", "Copy ping", "ping-detail-copy") {
                            copy("Ping", model.prompt(item).text ?: item.title)
                        }
                        if (link.isNotEmpty()) {
                            FmRowDivider()
                            ActionRow("link", "Copy link", "ping-detail-copy-link") { copy("Link", link) }
                        }
                        if (item.source == InboxSource.Ping) {
                            FmRowDivider()
                            ActionRow("tune_rounded", "Ping settings", "ping-detail-settings") { NavigationFeature.open(Route.PingSettings) }
                        }
                        FmRowDivider()
                        ActionRow("delete_outline", "Delete", "ping-detail-delete", danger = true) { confirmDelete = true }
                    }
                }
                FmInlineStatus(status, Modifier.padding(top = FmSpace.x2))
            }
            if (confirmDelete) {
                FmConfirmDialog(
                    title = if (item.source == InboxSource.Ping) "Delete this ping?" else "Remove this from the inbox?",
                    body = if (item.source == InboxSource.Ping) {
                        "This removes it from your ForkMesh inbox on every device. This cannot be undone."
                    } else {
                        null
                    },
                    confirmLabel = "Delete",
                    destructive = true,
                    onDismissRequest = { confirmDelete = false },
                    onConfirm = {
                        confirmDelete = false
                        navigator.pop()
                        ProjectLifecycle.scope.launch {
                            withContext(Dispatchers.IO) { model.delete(item) }
                            InboxFeature.publish()
                        }
                    },
                )
            }
        }
    }
}

@Composable
private fun ActionRow(icon: String, title: String, tag: String, danger: Boolean = false, onTap: () -> Unit) {
    val c = Fm.colors
    val tone = if (danger) c.danger else c.accent
    FmListRow(
        title = title,
        modifier = Modifier.testTag(tag),
        leading = { FmIconTile(icon, tint = tone) },
        onTap = onTap,
        showChevron = !danger,
        titleColor = if (danger) tone else null,
    )
}
