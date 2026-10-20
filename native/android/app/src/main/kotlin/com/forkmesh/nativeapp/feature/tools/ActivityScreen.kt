package com.forkmesh.nativeapp.feature.tools

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.RowScope
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.account.settings.SponsoredAdCard
import com.forkmesh.nativeapp.feature.chat.ChatFeature
import com.forkmesh.nativeapp.model.FundsReceivedEntry
import com.forkmesh.nativeapp.model.NetworkLeaderboards
import com.forkmesh.nativeapp.model.NetworkStats
import com.forkmesh.nativeapp.model.PayoutNode
import com.forkmesh.nativeapp.tools.ActivityLogLine
import com.forkmesh.nativeapp.tools.ActivityRules
import com.forkmesh.nativeapp.tools.LoadState
import com.forkmesh.nativeapp.tools.clockTime12From24
import com.forkmesh.nativeapp.ui.kit.FmCard
import com.forkmesh.nativeapp.ui.kit.FmEmptyState
import com.forkmesh.nativeapp.ui.kit.FmSectionHeader
import com.forkmesh.nativeapp.ui.kit.FmSkeletonList
import com.forkmesh.nativeapp.ui.kit.FmTextButton
import com.forkmesh.nativeapp.ui.kit.FmTooltip
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmRadius
import com.forkmesh.nativeapp.ui.theme.FmSpace
import com.forkmesh.nativeapp.ui.theme.FmType
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

class ActivityScreen : Screen() {
    override val title: String get() = "Activity"

    private var stats by mutableStateOf<LoadState<NetworkStats>>(LoadState.Loading)
    private var leaderboards by mutableStateOf<LoadState<NetworkLeaderboards>>(LoadState.Loading)
    private var showAllLogs by mutableStateOf(false)
    private var generation by mutableIntStateOf(0)

    private fun reload() {
        stats = LoadState.Loading
        leaderboards = LoadState.Loading
        generation++
    }

    @Composable
    override fun Content(navigator: Navigator) {
        LaunchedEffect(generation) {
            val api = Services.get<ApiService>()
            if (api == null) {
                stats = LoadState.Failed("ForkMesh is not configured.")
                leaderboards = LoadState.Failed("ForkMesh is not configured.")
                return@LaunchedEffect
            }
            stats = withContext(Dispatchers.IO) { ActivityRules.loadStats(api) }
            leaderboards = withContext(Dispatchers.IO) { ActivityRules.loadLeaderboards(api) }
        }
        val log by ToolsFeature.relayLog.collectAsState()
        Column(Modifier.fillMaxSize().background(Fm.colors.canvas)) {
            ToolsAppBar("Activity", navigator)
            LazyColumn(contentPadding = PaddingValues(FmSpace.x4), modifier = Modifier.fillMaxSize()) {
                item { ChatFeature.NodeActivitySection(navigator) }
                item {
                    FmSectionHeader("Network")
                    Spacer(Modifier.height(FmSpace.x2))
                    when (val s = stats) {
                        LoadState.Loading -> FmSkeletonList(Modifier.height(96.dp), rows = 1, hasLeading = false, label = ActivityRules.LOADING_NETWORK)
                        is LoadState.Failed -> ActivityErrorState(ActivityRules.NETWORK_ERROR_TITLE, s.message, ::reload)
                        is LoadState.Loaded -> Row {
                            StatCard("Nodes online", "${s.value.nodesOnline}")
                            Spacer(Modifier.width(FmSpace.x3))
                            StatCard("Hosts online", "${s.value.hostsOnline}")
                            Spacer(Modifier.width(FmSpace.x3))
                            StatCard("Repositories", "${s.value.repos}")
                        }
                    }
                    Spacer(Modifier.height(FmSpace.x5))
                }
                item {
                    val s = stats
                    if (s is LoadState.Loaded) {
                        PayoutReadinessSection(s.value.payoutNodes)
                        Spacer(Modifier.height(FmSpace.x5))
                    }
                }
                item {
                    when (val b = leaderboards) {
                        LoadState.Loading -> FmSkeletonList(Modifier.height(96.dp), rows = 1, hasLeading = false, label = ActivityRules.LOADING_FUNDS)
                        is LoadState.Failed -> ActivityErrorState(ActivityRules.FUNDS_ERROR_TITLE, b.message, ::reload)
                        is LoadState.Loaded -> FundsReceivedSection(b.value)
                    }
                    Spacer(Modifier.height(FmSpace.x5))
                }
                item {
                    FmSectionHeader(
                        "Activity log",
                        trailing = {
                            Text(
                                ActivityRules.eventsLabel(log.size),
                                style = Fm.text.bodySmall.copy(fontWeight = FmType.emphasis, color = Fm.colors.textTertiary),
                            )
                        },
                    )
                    Spacer(Modifier.height(FmSpace.x2))
                    FmCard(radius = FmRadius.lg, color = Fm.colors.bgRaised) {
                        if (log.isEmpty()) {
                            FmEmptyState(
                                "history",
                                "No activity yet",
                                message = "Connection events and network updates will appear here.",
                            )
                        } else {
                            Column(Modifier.fillMaxWidth()) {
                                for (line in ActivityRules.visibleLogs(log, showAllLogs)) ActivityLogRow(line)
                                if (ActivityRules.hasMoreLogs(log)) {
                                    Spacer(Modifier.height(FmSpace.x3))
                                    FmTextButton(onClick = { showAllLogs = !showAllLogs }) {
                                        FmIcon(if (showAllLogs) "expand_less" else "expand_more", size = 18.dp, tint = Fm.colors.accent)
                                        Spacer(Modifier.width(FmSpace.x1))
                                        Text(ActivityRules.toggleLabel(showAllLogs, log.size))
                                    }
                                }
                            }
                        }
                    }
                }
                item { SponsoredAdCard(Modifier.padding(top = FmSpace.x5)) }
            }
        }
    }
}

@Composable
private fun ActivityErrorState(title: String, message: String, onRetry: () -> Unit) {
    FmCard(radius = FmRadius.lg, color = Fm.colors.bgRaised) {
        Column(Modifier.fillMaxWidth(), horizontalAlignment = Alignment.CenterHorizontally) {
            FmIcon("error_outline", size = 32.dp, tint = Fm.colors.danger)
            Spacer(Modifier.height(FmSpace.x2))
            Text(title, textAlign = TextAlign.Center, style = Fm.text.titleSmall.copy(color = Fm.colors.textPrimary))
            Spacer(Modifier.height(FmSpace.x1))
            Text(message, textAlign = TextAlign.Center, style = Fm.text.bodyMedium.copy(color = Fm.colors.textSecondary))
            Spacer(Modifier.height(FmSpace.x2))
            FmTooltip(ActivityRules.RETRY_TOOLTIP) {
                FmTextButton(onClick = onRetry) {
                    FmIcon("refresh", size = 18.dp, tint = Fm.colors.accent)
                    Spacer(Modifier.width(FmSpace.x1))
                    Text("Retry")
                }
            }
        }
    }
}

@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun PayoutReadinessSection(nodes: List<PayoutNode>) {
    val visible = ActivityRules.visiblePayoutNodes(nodes)
    FmSectionHeader("Payout readiness")
    Spacer(Modifier.height(FmSpace.x2))
    FmCard(radius = FmRadius.lg, color = Fm.colors.bgRaised) {
        Column(Modifier.fillMaxWidth()) {
            Text(ActivityRules.PAYOUT_NOTE, style = Fm.text.bodySmall.copy(color = Fm.colors.textSecondary))
            Spacer(Modifier.height(FmSpace.x3))
            if (visible.isEmpty()) {
                FmEmptyState(
                    "account_balance_wallet_outlined",
                    "No payout-ready nodes reported",
                    message = "Payout-eligible nodes appear here when the mainnode reports public payout readiness.",
                )
            } else {
                for (node in visible) PayoutNodeRow(node)
                ActivityRules.moreNodesLabel(nodes)?.let {
                    Text(
                        it,
                        modifier = Modifier.padding(top = FmSpace.x2),
                        style = Fm.text.bodySmall.copy(fontWeight = FmType.emphasis, color = Fm.colors.textTertiary),
                    )
                }
            }
        }
    }
}

@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun PayoutNodeRow(node: PayoutNode) {
    val c = Fm.colors
    Column(
        Modifier
            .fillMaxWidth()
            .padding(bottom = FmSpace.x2)
            .background(c.bgBase, RoundedCornerShape(FmRadius.md))
            .border(1.dp, c.border, RoundedCornerShape(FmRadius.md))
            .padding(FmSpace.x3),
    ) {
        Row(verticalAlignment = Alignment.Top) {
            Text(
                ActivityRules.nodeName(node),
                modifier = Modifier.weight(1f),
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
                style = Fm.text.labelMedium.copy(color = c.textPrimary),
            )
            Spacer(Modifier.width(FmSpace.x2))
            MiniBadge(if (node.online) "online" else "offline", if (node.online) c.success else c.textTertiary)
            Spacer(Modifier.width(FmSpace.x1))
            MiniBadge(if (node.payoutEligible) "eligible" else "not eligible", if (node.payoutEligible) c.success else c.warning)
        }
        val details = ActivityRules.payoutDetails(node)
        if (details.isNotEmpty()) {
            Spacer(Modifier.height(FmSpace.x2))
            FlowRow(horizontalArrangement = Arrangement.spacedBy(FmSpace.x2), verticalArrangement = Arrangement.spacedBy(FmSpace.x1)) {
                for (detail in details) DetailToken(detail)
            }
        }
    }
}

@Composable
internal fun DetailToken(label: String) {
    val c = Fm.colors
    Text(
        label,
        modifier = Modifier
            .background(c.bgRaised, RoundedCornerShape(FmRadius.sm))
            .border(1.dp, c.border, RoundedCornerShape(FmRadius.sm))
            .padding(horizontal = FmSpace.x2, vertical = FmSpace.x1),
        style = Fm.text.bodySmall.copy(fontSize = FmType.micro, fontWeight = FmType.emphasis, color = c.textSecondary),
    )
}

@Composable
private fun FundsReceivedSection(boards: NetworkLeaderboards) {
    FmSectionHeader("Funds received")
    Spacer(Modifier.height(FmSpace.x2))
    FmCard(radius = FmRadius.lg, color = Fm.colors.bgRaised) {
        Column(Modifier.fillMaxWidth()) {
            Text(ActivityRules.FUNDS_NOTE, style = Fm.text.bodySmall.copy(color = Fm.colors.textSecondary))
            Spacer(Modifier.height(FmSpace.x3))
            if (boards.isEmpty) {
                FmEmptyState(
                    "leaderboard_outlined",
                    "No funds received yet",
                    message = "Public funds-received totals appear here after the mainnode reports them.",
                )
            } else {
                FundsGroup("Mainnodes", boards.fundsMainnodes)
                FundsGroup("Contributors", boards.fundsContributors)
                FundsGroup("Projects", boards.fundsProjects)
            }
        }
    }
}

@Composable
private fun FundsGroup(title: String, entries: List<FundsReceivedEntry>) {
    val visible = ActivityRules.visibleFunds(entries)
    if (visible.isEmpty()) return
    Column(Modifier.fillMaxWidth().padding(bottom = FmSpace.x3)) {
        Text(
            title,
            style = Fm.text.bodySmall.copy(fontSize = FmType.micro, fontWeight = FmType.emphasis, color = Fm.colors.textTertiary),
        )
        Spacer(Modifier.height(FmSpace.x2))
        for (entry in visible) {
            Row(Modifier.padding(bottom = FmSpace.x1)) {
                Text(
                    ActivityRules.fundsName(entry),
                    modifier = Modifier.weight(1f),
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                    style = Fm.text.labelMedium.copy(color = Fm.colors.textPrimary),
                )
                Spacer(Modifier.width(FmSpace.x3))
                Text(
                    entry.amountLabel,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                    textAlign = TextAlign.End,
                    style = Fm.text.bodySmall.copy(fontWeight = FmType.emphasis, color = Fm.colors.textSecondary),
                )
            }
        }
    }
}

@Composable
private fun MiniBadge(label: String, color: Color) {
    Text(
        label,
        maxLines = 1,
        overflow = TextOverflow.Ellipsis,
        modifier = Modifier
            .background(color.copy(alpha = 31f / 255f), RoundedCornerShape(FmRadius.full))
            .padding(horizontal = FmSpace.x2, vertical = FmSpace.x1),
        style = Fm.text.bodySmall.copy(fontSize = FmType.micro, fontWeight = FmType.emphasis, color = color),
    )
}

@Composable
private fun ActivityLogRow(raw: String) {
    val line = ActivityLogLine.parse(raw)
    val c = Fm.colors
    val (icon, tint) = when (line.tone) {
        ActivityLogLine.Tone.Failure -> "error_outline" to c.danger
        ActivityLogLine.Tone.Connected -> "check_circle_outline" to c.success
        ActivityLogLine.Tone.Left -> "logout" to c.textTertiary
        ActivityLogLine.Tone.Event -> "circle_outlined" to c.textTertiary
    }
    Row(Modifier.fillMaxWidth().padding(vertical = FmSpace.x1), verticalAlignment = Alignment.Top) {
        FmIcon(icon, size = 16.dp, tint = tint)
        Spacer(Modifier.width(FmSpace.x2))
        Column(Modifier.weight(1f)) {
            Text(line.body, style = Fm.text.bodySmall.copy(color = c.textPrimary))
            if (line.timestamp.isNotEmpty()) {
                Text(clockTime12From24(line.timestamp), style = Fm.text.labelSmall.copy(color = c.textTertiary))
            }
        }
    }
}

@Composable
private fun RowScope.StatCard(label: String, value: String) {
    FmCard(modifier = Modifier.weight(1f), radius = FmRadius.lg, color = Fm.colors.bgRaised) {
        Column {
            Text(value, style = Fm.text.displaySmall.copy(color = Fm.colors.textPrimary))
            Spacer(Modifier.height(FmSpace.x2))
            Text(label, maxLines = 2, overflow = TextOverflow.Ellipsis, style = Fm.text.bodySmall.copy(color = Fm.colors.textSecondary))
        }
    }
}
