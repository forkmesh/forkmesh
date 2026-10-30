@file:OptIn(ExperimentalLayoutApi::class)

package com.forkmesh.nativeapp.feature.code.collab

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.LocalTextStyle
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.MutableState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.ci.RepoRef
import com.forkmesh.nativeapp.collab.IssueDetail
import com.forkmesh.nativeapp.collab.IssueTimeline
import com.forkmesh.nativeapp.collab.IssueTimelineEntry
import com.forkmesh.nativeapp.collab.SubIssues
import com.forkmesh.nativeapp.collab.THREAD_SUBSCRIPTION_NOTE
import com.forkmesh.nativeapp.collab.ThreadSubscription
import com.forkmesh.nativeapp.collab.dartErrorText
import com.forkmesh.nativeapp.collab.issueParentLinkLabel
import com.forkmesh.nativeapp.collab.runFailureText
import com.forkmesh.nativeapp.model.Issue
import com.forkmesh.nativeapp.model.IssueEvent
import com.forkmesh.nativeapp.model.IssueMention
import com.forkmesh.nativeapp.model.Repository
import com.forkmesh.nativeapp.model.issueMentionsOf
import com.forkmesh.nativeapp.model.solanaExplorerAddressUrl
import com.forkmesh.nativeapp.model.solanaExplorerSignatureUrl
import com.forkmesh.nativeapp.model.subIssueNumbersOf
import com.forkmesh.nativeapp.ui.kit.FmInboxPendingNote
import com.forkmesh.nativeapp.ui.kit.FmInlineStatus
import com.forkmesh.nativeapp.ui.kit.FmStatus
import com.forkmesh.nativeapp.ui.kit.rememberFmStatus
import com.forkmesh.nativeapp.ui.kit.FmLinearProgressIndicator
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmRadius
import com.forkmesh.nativeapp.ui.theme.FmType
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

internal class IssueDetailScreen(
    private val repo: Repository,
    private val number: Int,
    initial: Issue?,
) : Screen() {
    private var issue by mutableStateOf(initial)

    private var allIssues by mutableStateOf<List<Issue>?>(null)
    private var loadError by mutableStateOf<String?>(null)
    private var loadStarted = false
    private var bountyBusy by mutableStateOf(false)
    private var bountyMessage by mutableStateOf("")
    private val subscribed = mutableStateOf(false)
    private var pullScreen by mutableStateOf<PullDetailScreen?>(null)

    override val title: String get() = pullScreen?.title ?: "#${issue?.number ?: number}"

    override fun onDisposed() {
        pullScreen?.onDisposed()
    }

    @Composable
    override fun Content(navigator: Navigator) {
        val pull = pullScreen
        if (pull != null) {
            pull.Content(navigator)
        } else {
            IssueContent(navigator)
        }
    }

    @Composable
    private fun IssueContent(navigator: Navigator) {
        val env = rememberCollabEnv(navigator)
        LaunchedEffect(Unit) {
            if (!loadStarted) {
                loadStarted = true
                env.launch { load() }
            }
        }
        CollabScaffold(topBar = { CollabTopBar(navigator, title = title) }) {
            val current = issue
            val error = loadError
            when {
                current != null -> Detail(env, current, allIssues ?: emptyList())
                error != null -> MessageList(padding = 20.dp) { EmptyCard(error) }
                allIssues == null -> MessageList(padding = 20.dp) { LoadingCard("Loading…") }
                else -> MessageList(padding = 20.dp) { EmptyCard("Issue #$number is not published.") }
            }
        }
    }

    private suspend fun CollabEnv.load() {
        try {
            val all = withContext(Dispatchers.IO) { api.publishedIssues(repo.owner, repo.name) }
            if (issue == null) issue = all.firstOrNull { it.number == number }
            if (issue == null) {
                val pull = try {
                    withContext(Dispatchers.IO) { api.publishedPull(repo.owner, repo.name, number) }
                } catch (error: CancellationException) {
                    throw error
                } catch (_: Exception) {
                    null
                }
                if (pull != null) pullScreen = PullDetailScreen(repo, number, pull)
            }
            allIssues = all
        } catch (error: CancellationException) {
            throw error
        } catch (error: Exception) {
            if (issue == null) loadError = dartErrorText(error)
            allIssues = emptyList()
        }
    }

    private fun CollabEnv.refreshBounty() {
        if (bountyBusy) return
        bountyBusy = true
        bountyMessage = ""
        launch {
            val current = issue ?: return@launch
            try {
                val bounty = withContext(Dispatchers.IO) { api.issueBountyStatus(repo.owner, repo.name, current.number) }
                val (updated, message) = IssueDetail.bountyRefreshed(current, bounty)
                issue = updated
                bountyMessage = message
            } catch (error: CancellationException) {
                throw error
            } catch (error: Exception) {
                bountyMessage = IssueDetail.bountyRefreshFailed(error)
            } finally {
                bountyBusy = false
            }
        }
    }

    @Composable
    private fun Detail(env: CollabEnv, issue: Issue, all: List<Issue>) {
        val find = remember(all) { { wanted: Int -> all.firstOrNull { it.number == wanted } } }
        val parents = remember(issue) { IssueDetail.parents(issue) }
        val children = remember(all, issue) { subIssueNumbersOf(all, issue) }
        val mentions = remember(all, issue.number) { issueMentionsOf(all, issue.number) }
        val openIssue = { target: Issue -> env.navigator.push(IssueDetailScreen(repo, target.number, target)) }
        val subIssueStatus = rememberFmStatus()
        LazyColumn(Modifier.fillMaxSize(), contentPadding = PaddingValues(20.dp)) {
            item {
                InfoCard(issue.title) {
                    if (parents.isNotEmpty()) {
                        IssueParentLinks(parents, find, openIssue)
                        Spacer(Modifier.height(12.dp))
                    }
                    FlowRow(
                        horizontalArrangement = Arrangement.spacedBy(8.dp),
                        verticalArrangement = Arrangement.spacedBy(8.dp),
                    ) {
                        for (chip in IssueDetail.chips(issue, repo.fullName)) ChipView(chip)
                    }
                    Spacer(Modifier.height(16.dp))
                    SelectableBody(IssueDetail.body(issue), style = LocalTextStyle.current.withHeight(1.45f), repo = RepoRef(repo.owner, repo.name))
                }
            }
            item {
                Spacer(Modifier.height(12.dp))
                com.forkmesh.nativeapp.feature.code.review.IssueToolsCard(repo, issue, env.navigator) { this@IssueDetailScreen.issue = it }
            }
            item {
                Spacer(Modifier.height(12.dp))
                SubIssuesCard(
                    numbers = children,
                    find = find,
                    onOpen = openIssue,
                    status = subIssueStatus,
                    onAdd = { env.launch { openNewIssueComposer(repo, parentNumber = issue.number, status = subIssueStatus) } },
                )
            }
            item {
                Spacer(Modifier.height(12.dp))
                InfoCard("Timeline") {
                    IssueTimelineView(issue.events, mentions, RepoRef(repo.owner, repo.name)) { source -> find(source)?.let(openIssue) }
                    Spacer(Modifier.height(10.dp))
                    Text(THREAD_SUBSCRIPTION_NOTE)
                    Spacer(Modifier.height(14.dp))
                    IssueQuickActions(env, repo, issue, subscribed)
                    FmInlineStatus(env.status, Modifier.padding(top = 8.dp))
                }
            }
            if (!repo.isPrivate) {
                item {
                    Spacer(Modifier.height(12.dp))
                    FederatedRepliesCard(repo, kind = "issue", number = issue.number)
                }
            }
            item {
                Spacer(Modifier.height(12.dp))
                BountyFundingCard(
                    issue = issue,
                    busy = bountyBusy,
                    message = bountyMessage,
                    gateCopy = IssueDetail.FUNDING_GATE_COPY,
                    onRefresh = { env.refreshBounty() },
                )
            }
            item { NavigationBarSpacer() }
        }
    }
}

@Composable
private fun BountyFundingCard(
    issue: Issue,
    busy: Boolean,
    message: String,
    gateCopy: String,
    onRefresh: () -> Unit,
) {
    val colors = Fm.colors
    val addressExplorerUrl = solanaExplorerAddressUrl(issue.bountyAddress)
    val payoutExplorerUrl = solanaExplorerSignatureUrl(issue.bountyPayoutSig)
    InfoCard("Bounty funding") {
        val pledge = IssueDetail.pledgeText(issue)
        if (pledge != null) {
            Text(pledge, color = colors.textPrimary, fontWeight = FmType.emphasis)
        } else {
            Text("No pledge amount published yet.", color = colors.textSecondary)
        }
        Spacer(Modifier.height(8.dp))
        Text("Status: ${issue.bountyStatusLabel}")
        if (issue.bountyProgressLabel.isNotEmpty()) {
            Spacer(Modifier.height(8.dp))
            Text(issue.bountyProgressLabel)
        }
        if (issue.bountyAddress.isNotEmpty()) {
            Spacer(Modifier.height(12.dp))
            CopyableValue(
                label = "Legacy address (do not fund)",
                value = issue.bountyAddress,
                copyLabel = "Bounty deposit address",
                tag = "issue-bounty-copy-address",
            )
            if (addressExplorerUrl.isNotEmpty()) {
                Spacer(Modifier.height(8.dp))
                CopyableValue(
                    label = "Address explorer URL",
                    value = addressExplorerUrl,
                    copyLabel = "Bounty address explorer URL",
                    tag = "issue-bounty-copy-address-explorer",
                )
            }
        }
        if (issue.bountyPayoutSig.isNotEmpty()) {
            Spacer(Modifier.height(12.dp))
            CopyableValue(
                label = "Payout signature",
                value = issue.bountyPayoutSig,
                copyLabel = "Bounty payout signature",
                tag = "issue-bounty-copy-payout-sig",
            )
            if (payoutExplorerUrl.isNotEmpty()) {
                Spacer(Modifier.height(8.dp))
                CopyableValue(
                    label = "Payout transaction explorer URL",
                    value = payoutExplorerUrl,
                    copyLabel = "Bounty payout explorer URL",
                    tag = "issue-bounty-copy-payout-explorer",
                )
            }
        }
        Spacer(Modifier.height(12.dp))
        Text(
            IssueDetail.NON_CUSTODIAL_COPY,
            style = Fm.text.bodySmall.copy(color = colors.textSecondary).withHeight(1.35f),
        )
        Spacer(Modifier.height(12.dp))
        FlowRow(
            horizontalArrangement = Arrangement.spacedBy(10.dp),
            verticalArrangement = Arrangement.spacedBy(10.dp),
            itemVerticalAlignment = Alignment.CenterVertically,
        ) {
            CollabOutlinedButton(
                label = "Refresh bounty status",
                icon = "refresh",
                onClick = if (busy) null else onRefresh,
                modifier = Modifier.testTag("issue-bounty-refresh-status"),
            )
            CollabFilledButton(
                label = "Custodial funding disabled",
                icon = "account_balance_wallet_outlined",
                onClick = null,
                modifier = Modifier.testTag("issue-bounty-prepare-deposit"),
            )
        }
        if (gateCopy.isNotEmpty()) {
            Spacer(Modifier.height(10.dp))
            Text(gateCopy, style = Fm.text.bodySmall.copy(color = colors.textTertiary))
        }
        if (message.isNotEmpty()) {
            Spacer(Modifier.height(10.dp))
            Text(
                message,
                style = Fm.text.bodySmall.copy(
                    color = if (IssueDetail.bountyMessageIsError(message)) colors.danger else colors.textSecondary,
                ),
            )
        }
    }
}

@Composable
private fun IssueParentLinks(numbers: List<Int>, find: (Int) -> Issue?, onOpen: (Issue) -> Unit) {
    FlowRow(
        Modifier.testTag("issue-parent-links"),
        horizontalArrangement = Arrangement.spacedBy(8.dp),
        verticalArrangement = Arrangement.spacedBy(6.dp),
        itemVerticalAlignment = Alignment.CenterVertically,
    ) {
        Text("Sub-issue of", style = Fm.text.bodySmall.copy(color = Fm.colors.textSecondary))
        for (number in numbers) {
            val parent = find(number)
            CollabActionChip(
                label = issueParentLinkLabel(number, parent),
                icon = "account_tree_outlined",
                onClick = parent?.let { { onOpen(it) } },
            )
        }
    }
}

@Composable
private fun SubIssuesCard(
    numbers: List<Int>,
    find: (Int) -> Issue?,
    onOpen: (Issue) -> Unit,
    status: FmStatus,
    onAdd: () -> Unit,
) {
    val colors = Fm.colors
    InfoCard(SubIssues.title(numbers, find)) {
        if (numbers.isEmpty()) {
            Text("No sub-issues yet.")
        } else {
            for (number in numbers) {
                val child = find(number)
                Column(
                    Modifier
                        .testTag("sub-issue-$number")
                        .fillMaxWidth()
                        .clip(RoundedCornerShape(FmRadius.lg))
                        .clickable(enabled = child != null) { child?.let(onOpen) }
                        .padding(vertical = 8.dp),
                ) {
                    Text(SubIssues.rowTitle(number, child), fontWeight = FmType.emphasis)
                    Spacer(Modifier.height(2.dp))
                    Text(SubIssues.details(child), style = Fm.text.bodySmall.copy(color = colors.textSecondary))
                    if (child != null) {
                        Spacer(Modifier.height(6.dp))
                        FmLinearProgressIndicator(Modifier.height(3.dp), progress = { SubIssues.progress(child) })
                    }
                }
            }
        }
        Spacer(Modifier.height(10.dp))
        CollabOutlinedButton(
            label = "Add sub-issue",
            icon = "add",
            onClick = onAdd,
            modifier = Modifier.testTag("issue-add-sub-issue"),
        )
        FmInlineStatus(status, Modifier.padding(top = 8.dp))
    }
}

@Composable
private fun IssueTimelineView(events: List<IssueEvent>, mentions: List<IssueMention>, repo: RepoRef, onOpenMention: (Int) -> Unit) {
    if (events.isEmpty() && mentions.isEmpty()) {
        Text(IssueTimeline.EMPTY)
        return
    }
    val entries = remember(events, mentions) { IssueTimeline.entries(events, mentions) }
    val colors = Fm.colors
    Column(Modifier.fillMaxWidth()) {
        for (entry in entries) {
            when (entry) {
                is IssueTimelineEntry.Event -> TimelineTile(
                    icon = IssueTimeline.icon(entry.event),
                    iconTint = if (IssueTimeline.isStatus(entry.event)) colors.accent else colors.textTertiary,
                    title = IssueTimeline.title(entry.event),
                    author = entry.event.displayAuthor,
                    body = entry.event.body,
                    repo = repo,
                    profile = entry.event.authorName,
                )
                is IssueTimelineEntry.Mention -> TimelineTile(
                    icon = "link",
                    iconTint = colors.textTertiary,
                    title = IssueTimeline.mentionTitle(entry.mention),
                    author = entry.mention.authorName,
                    modifier = Modifier.testTag("issue-mention-${entry.mention.sourceNumber}"),
                    onTap = { onOpenMention(entry.mention.sourceNumber) },
                )
            }
        }
    }
}

@Composable
private fun IssueQuickActions(env: CollabEnv, repo: Repository, issue: Issue, subscribed: MutableState<Boolean>) {
    FlowRow(
        horizontalArrangement = Arrangement.spacedBy(10.dp),
        verticalArrangement = Arrangement.spacedBy(10.dp),
        itemVerticalAlignment = Alignment.CenterVertically,
    ) {
        CollabFilledButton(
            label = "Comment / vote / status",
            icon = "add_comment_outlined",
            onClick = { env.launch { showIssueActions(repo, issue) } },
        )
        ThreadSubscriptionButton(env, repo, source = "issue", number = issue.number, subscribed = subscribed)
        FmInboxPendingNote()
    }
}

@Composable
internal fun ThreadSubscriptionButton(
    env: CollabEnv,
    repo: Repository,
    source: String,
    number: Int,
    subscribed: MutableState<Boolean>,
) {
    val session = rememberSession()
    val guest = rememberGuestMode()
    val identity = env.identity
    val node = ThreadSubscription.signedAccountNode(session?.nodeName, session?.pubkey, identity?.publicKeyB64url)
    val canSign = node.isNotEmpty()
    var busy by remember { mutableStateOf(false) }
    val isSubscribed = subscribed.value
    CollabOutlinedButton(
        label = ThreadSubscription.label(guest, canSign, isSubscribed),
        icon = ThreadSubscription.icon(guest, canSign, isSubscribed),
        busy = busy,
        onClick = when {
            guest -> {
                { env.launch { requireAccount(ThreadSubscription.GUEST_ACTION) } }
            }
            busy || !canSign -> null
            else -> {
                {
                    if (!busy) {
                        val next = !subscribed.value
                        busy = true
                        env.launch {
                            try {
                                withContext(Dispatchers.IO) {
                                    inbox.setThreadSubscription(repo.owner, repo.name, node, source, number, next)
                                }
                                subscribed.value = next
                                status.show(ThreadSubscription.toggled(next))
                            } catch (error: CancellationException) {
                                throw error
                            } catch (error: Exception) {
                                status.error(runFailureText(error))
                            } finally {
                                busy = false
                            }
                        }
                    }
                }
            }
        },
    )
}
