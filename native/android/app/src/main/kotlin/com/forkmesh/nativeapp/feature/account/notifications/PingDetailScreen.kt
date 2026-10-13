package com.forkmesh.nativeapp.feature.account.notifications

import android.content.Context
import android.content.Intent
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.account.PING_DELETED_TEXT
import com.forkmesh.nativeapp.account.PING_DELETE_BODY
import com.forkmesh.nativeapp.account.PING_DELETE_LABEL
import com.forkmesh.nativeapp.account.PING_DELETE_TITLE
import com.forkmesh.nativeapp.account.PING_DETAIL_TITLE
import com.forkmesh.nativeapp.account.PingDetailFact
import com.forkmesh.nativeapp.account.notificationIconForKind
import com.forkmesh.nativeapp.account.notificationPills
import com.forkmesh.nativeapp.account.pingAgentPrompt
import com.forkmesh.nativeapp.account.pingDeleteErrorText
import com.forkmesh.nativeapp.account.pingDetailFacts
import com.forkmesh.nativeapp.account.pingKindSettingsLabel
import com.forkmesh.nativeapp.account.pingOpenLabel
import com.forkmesh.nativeapp.account.pingWebLink
import com.forkmesh.nativeapp.collab.ReferenceLinks
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.account.AccountScreen
import com.forkmesh.nativeapp.feature.account.HGap
import com.forkmesh.nativeapp.feature.account.NotificationService
import com.forkmesh.nativeapp.feature.account.VGap
import com.forkmesh.nativeapp.feature.account.openExternalUrl
import com.forkmesh.nativeapp.feature.push.PushFeature
import com.forkmesh.nativeapp.feature.agents.RunnerAttentionFeature
import com.forkmesh.nativeapp.runners.RunnerAttention
import com.forkmesh.nativeapp.model.ForkNotification
import com.forkmesh.nativeapp.model.NotificationDeepLink
import com.forkmesh.nativeapp.ui.kit.FmAlertDialog
import com.forkmesh.nativeapp.ui.kit.FmCard
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmIconTile
import com.forkmesh.nativeapp.ui.kit.FmInlineStatus
import com.forkmesh.nativeapp.ui.kit.FmListRow
import com.forkmesh.nativeapp.ui.kit.FmPanelHeader
import com.forkmesh.nativeapp.ui.kit.FmRowDivider
import com.forkmesh.nativeapp.ui.kit.FmSectionHeader
import com.forkmesh.nativeapp.ui.kit.FmStatus
import com.forkmesh.nativeapp.ui.kit.FmTextButton
import com.forkmesh.nativeapp.ui.kit.LinkedText
import com.forkmesh.nativeapp.ui.kit.LocalFmMessenger
import com.forkmesh.nativeapp.ui.kit.copyMessageText
import com.forkmesh.nativeapp.ui.kit.fmSafeAreaInsets
import com.forkmesh.nativeapp.ui.kit.rememberFmStatus
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmRadius
import com.forkmesh.nativeapp.ui.theme.FmSpace
import com.forkmesh.nativeapp.ui.theme.FmType
import kotlin.coroutines.cancellation.CancellationException
import kotlinx.coroutines.launch

class PingDetailScreen(
    private val initial: ForkNotification,
    private val listStatus: FmStatus? = null,
) : AccountScreen() {
    private var markedRead = false
    private var deleting = false

    override val title: String get() = PING_DETAIL_TITLE

    @OptIn(ExperimentalLayoutApi::class)
    @Composable
    override fun Content(navigator: Navigator) {
        val service = Services.require<NotificationService>()
        val items by service.notifications.collectAsState()
        val ping = items.firstOrNull { initial.id.isNotEmpty() && it.id == initial.id } ?: initial
        val messenger = LocalFmMessenger.current
        val context = LocalContext.current
        val c = Fm.colors
        val openStatus = rememberFmStatus()
        val deleteStatus = rememberFmStatus()
        val copyStatus = rememberFmStatus()
        LaunchedEffect(Unit) {
            if (!markedRead) {
                markedRead = true
                scope.launch {
                    try {
                        service.markRead(ping)
                    } catch (cancelled: CancellationException) {
                        throw cancelled
                    } catch (_: Exception) {
                    }
                }
            }
        }
        val link = NotificationDeepLink.parse(ping.href)
            ?: if (ping.href.isBlank()) NotificationDeepLink.parse("/${ping.repo.trim()}") else null
        val runnerTarget = if (RunnerAttention.isRunnerPing(ping.kind, ping.href)) RunnerAttention.pingTarget(ping.href) else null
        val web = if (link == null) pingWebLink(ping.href) else null
        val openLabel = runnerTarget?.let { RunnerAttention.connectLabel(it.provider) } ?: pingOpenLabel(link, web)
        val open: () -> Unit = {
            openStatus.clear()
            when {
                runnerTarget != null -> RunnerAttentionFeature.open(navigator, runnerTarget) { url ->
                    if (!openExternalUrl(context, url)) openStatus.error("Could not open $url")
                }
                link != null -> PushFeature.openLink(navigator, link)
                web != null -> if (!openExternalUrl(context, web)) openStatus.error("Could not open $web")
            }
        }
        val delete: () -> Unit = {
            scope.launch {
                if (deleting) return@launch
                val confirmed = messenger.showDialog<Boolean> { done ->
                    FmAlertDialog(
                        onDismissRequest = { done(null) },
                        title = PING_DELETE_TITLE,
                        containerColor = Fm.colors.bgOverlay,
                        actions = {
                            FmTextButton(onClick = { done(false) }) { Text("Cancel") }
                            Button(
                                onClick = { done(true) },
                                colors = ButtonDefaults.buttonColors(containerColor = Fm.colors.danger),
                            ) { Text("Delete") }
                        },
                        content = { Text(PING_DELETE_BODY) },
                    )
                }
                if (confirmed != true) return@launch
                deleting = true
                deleteStatus.clear()
                try {
                    service.delete(ping)
                    if (listStatus != null) listStatus.show(PING_DELETED_TEXT) else deleteStatus.show(PING_DELETED_TEXT)
                    if (navigator.stack.lastOrNull() === this@PingDetailScreen) navigator.pop()
                } catch (cancelled: CancellationException) {
                    throw cancelled
                } catch (failure: Exception) {
                    deleteStatus.error(pingDeleteErrorText(failure))
                } finally {
                    deleting = false
                }
            }
        }

        Column(
            Modifier
                .fillMaxSize()
                .background(c.bgBase)
                .windowInsetsPadding(fmSafeAreaInsets()),
        ) {
            Box(
                Modifier
                    .fillMaxWidth()
                    .background(c.bgRaised),
            ) {
                FmPanelHeader(
                    title = PING_DETAIL_TITLE,
                    subtitle = ping.kindLabel,
                    leading = {
                        FmIconButton(icon = "arrow_back", label = "Back", onPressed = { navigator.pop() }, color = c.textPrimary)
                    },
                    trailing = if (ping.id.isNotEmpty()) {
                        {
                            FmIconButton(
                                icon = "delete_outline",
                                label = PING_DELETE_LABEL,
                                onPressed = delete,
                                color = c.danger,
                                modifier = Modifier.testTag("ping-detail-delete"),
                            )
                        }
                    } else {
                        null
                    },
                )
            }
            Column(
                Modifier
                    .weight(1f)
                    .fillMaxWidth()
                    .verticalScroll(rememberScrollState())
                    .padding(horizontal = FmSpace.x4, vertical = FmSpace.x4),
            ) {
                FmInlineStatus(deleteStatus, Modifier.padding(bottom = FmSpace.x3))
                PingHero(ping)
                if (openLabel != null) {
                    VGap(FmSpace.x4)
                    Button(
                        onClick = open,
                        modifier = Modifier
                            .fillMaxWidth()
                            .testTag("ping-detail-open"),
                        shape = RoundedCornerShape(FmRadius.md),
                        colors = ButtonDefaults.buttonColors(containerColor = c.accent),
                    ) {
                        FmIcon(if (link != null) "open_in_new_rounded" else "public", size = 18.dp)
                        HGap(FmSpace.x2)
                        Text(openLabel)
                    }
                    FmInlineStatus(openStatus, Modifier.padding(top = FmSpace.x2))
                }
                VGap(FmSpace.x5)
                FmSectionHeader("Actions")
                VGap(FmSpace.x2)
                FmCard(padding = androidx.compose.foundation.layout.PaddingValues(0.dp)) {
                    Column {
                        FmListRow(
                            title = "Copy text",
                            leading = { FmIconTile(icon = "copy_outlined") },
                            onTap = { copyMessageText(context, pingAgentPrompt(ping), copyStatus) },
                            showChevron = false,
                        )
                        FmRowDivider()
                        FmListRow(
                            title = "Share",
                            leading = { FmIconTile(icon = "share_outlined") },
                            onTap = { sharePing(context, ping) },
                            showChevron = false,
                        )
                        FmRowDivider()
                        FmListRow(
                            title = pingKindSettingsLabel(ping.kindLabel),
                            leading = { FmIconTile(icon = "tune_rounded") },
                            onTap = { navigator.push(PingSettingsScreen(ping.kind)) },
                        )
                        if (ping.id.isNotEmpty()) {
                            FmRowDivider()
                            FmListRow(
                                title = PING_DELETE_LABEL,
                                leading = { FmIconTile(icon = "delete_outline", tint = c.danger) },
                                onTap = delete,
                                showChevron = false,
                                titleColor = c.danger,
                            )
                        }
                    }
                }
                FmInlineStatus(copyStatus, Modifier.padding(top = FmSpace.x2))
                VGap(FmSpace.x5)
                FmSectionHeader("Details")
                VGap(FmSpace.x2)
                FmCard {
                    SelectionContainer {
                        Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x3)) {
                            pingDetailFacts(ping, System.currentTimeMillis()).forEach { PingFactRow(it) }
                        }
                    }
                }
            }
        }
    }
}

@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun PingHero(ping: ForkNotification) {
    val c = Fm.colors
    Column {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Box(
                Modifier
                    .size(52.dp)
                    .background(if (ping.isUnread) c.accentSubtle else c.bgRaised, CircleShape),
                contentAlignment = Alignment.Center,
            ) {
                FmIcon(notificationIconForKind(ping.kind), size = 24.dp, tint = c.accent)
            }
            HGap(FmSpace.x3)
            FlowRow(
                Modifier.weight(1f),
                horizontalArrangement = Arrangement.spacedBy(FmSpace.x2),
                verticalArrangement = Arrangement.spacedBy(FmSpace.x1),
            ) {
                notificationPills(ping).forEach { DetailPill(it) }
            }
        }
        VGap(FmSpace.x4)
        SelectionContainer {
            Column {
                Text(
                    ping.title,
                    style = Fm.text.titleLarge.copy(color = c.textPrimary, lineHeight = FmType.title * 1.2f),
                )
                val body = ping.body.trim()
                if (body.isNotEmpty()) {
                    VGap(FmSpace.x3)
                    LinkedText(
                        body,
                        ReferenceLinks.repoFor(ping.href, ping.repo),
                        style = Fm.text.bodyLarge.copy(color = c.textSecondary, lineHeight = FmType.body * 1.5f),
                        commits = false,
                    )
                }
            }
        }
    }
}

@Composable
private fun PingFactRow(fact: PingDetailFact) {
    val c = Fm.colors
    Row(verticalAlignment = Alignment.Top) {
        Text(
            fact.label,
            modifier = Modifier.width(104.dp),
            style = Fm.text.bodySmall.copy(fontWeight = FmType.emphasis, color = c.textTertiary),
        )
        HGap(FmSpace.x3)
        Text(fact.value, modifier = Modifier.weight(1f), style = Fm.text.bodyMedium.copy(color = c.textPrimary))
    }
}

@Composable
private fun DetailPill(label: String) {
    val c = Fm.colors
    val shape = RoundedCornerShape(FmRadius.sm)
    Text(
        label,
        modifier = Modifier
            .background(c.bgRaised, shape)
            .border(1.dp, c.border, shape)
            .padding(horizontal = FmSpace.x2, vertical = FmSpace.x1),
        style = Fm.text.bodySmall.copy(
            fontSize = FmType.micro,
            lineHeight = FmType.micro * FmType.leadingSnug,
            fontWeight = FmType.emphasis,
            color = c.textSecondary,
        ),
    )
}

private fun sharePing(context: Context, ping: ForkNotification) {
    val send = Intent(Intent.ACTION_SEND)
        .setType("text/plain")
        .putExtra(Intent.EXTRA_SUBJECT, ping.title)
        .putExtra(Intent.EXTRA_TEXT, pingAgentPrompt(ping))
    runCatching {
        context.startActivity(Intent.createChooser(send, null).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK))
    }
}
