@file:OptIn(ExperimentalLayoutApi::class)

package com.forkmesh.nativeapp.feature.code.collab

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.material3.LocalTextStyle
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontStyle
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.ci.RepoRef
import com.forkmesh.nativeapp.collab.DiscussionCard
import com.forkmesh.nativeapp.collab.DiscussionDetail
import com.forkmesh.nativeapp.collab.FederatedThread
import com.forkmesh.nativeapp.collab.PENDING_NOTE
import com.forkmesh.nativeapp.collab.dartErrorText
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.model.FederatedReply
import com.forkmesh.nativeapp.model.RepoDiscussion
import com.forkmesh.nativeapp.model.Repository
import com.forkmesh.nativeapp.ui.kit.FmInboxPendingNote
import com.forkmesh.nativeapp.ui.kit.FmInlineStatus
import com.forkmesh.nativeapp.ui.kit.FmLinearProgressIndicator
import com.forkmesh.nativeapp.ui.kit.FmTextButton
import com.forkmesh.nativeapp.ui.kit.FmTextField
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmType
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

@Composable
internal fun DiscussionsTabContent(repo: Repository, navigator: Navigator) {
    val state = rememberLoad(repo.owner, repo.name) {
        Services.require<ApiService>().publishedDiscussions(repo.owner, repo.name)
    }
    val accent = Fm.colors.accent
    AsyncListBody(state, empty = "No discussions have been published yet.") { discussion ->
        MobileCard(
            icon = "forum_outlined",
            iconColor = accent,
            title = discussion.title,
            subtitle = DiscussionCard.subtitle(discussion),
            badge = discussion.category,
            onTap = { navigator.push(DiscussionDetailScreen(repo, discussion.number, discussion)) },
        )
    }
}

internal class DiscussionDetailScreen(
    private val repo: Repository,
    private val number: Int,
    initial: RepoDiscussion?,
) : Screen() {
    private var discussion by mutableStateOf(initial)
    private var loadError by mutableStateOf<String?>(null)
    private var loaded by mutableStateOf(initial != null)
    private var loadStarted = initial != null
    private var reply by mutableStateOf("")

    override val title: String get() = "Discussion #${discussion?.number ?: number}"

    @Composable
    override fun Content(navigator: Navigator) {
        val env = rememberCollabEnv(navigator)
        LaunchedEffect(Unit) {
            if (!loadStarted) {
                loadStarted = true
                env.launch { load() }
            }
        }
        CollabScaffold(
            topBar = { CollabTopBar(navigator, title = title) },
            modifier = Modifier.imePadding(),
        ) {
            val current = discussion
            val error = loadError
            when {
                current != null -> Detail(env, current)
                error != null -> MessageList(padding = 20.dp) { EmptyCard(error) }
                !loaded -> MessageList(padding = 20.dp) { LoadingCard("Loading…") }
                else -> MessageList(padding = 20.dp) { EmptyCard("Discussion #$number is not published.") }
            }
        }
    }

    private suspend fun CollabEnv.load() {
        try {
            val discussions = withContext(Dispatchers.IO) { api.publishedDiscussions(repo.owner, repo.name) }
            discussion = discussions.firstOrNull { it.number == number }
        } catch (error: CancellationException) {
            throw error
        } catch (error: Exception) {
            loadError = dartErrorText(error)
        } finally {
            loaded = true
        }
    }

    @Composable
    private fun Detail(env: CollabEnv, discussion: RepoDiscussion) {
        LazyColumn(Modifier.fillMaxSize(), contentPadding = PaddingValues(20.dp)) {
            item {
                InfoCard(discussion.title) {
                    FlowRow(
                        horizontalArrangement = Arrangement.spacedBy(8.dp),
                        verticalArrangement = Arrangement.spacedBy(8.dp),
                    ) {
                        for (chip in DiscussionDetail.chips(discussion, repo.fullName)) ChipView(chip)
                    }
                    Spacer(Modifier.height(16.dp))
                    SelectableBody(DiscussionDetail.body(discussion), style = LocalTextStyle.current.withHeight(1.45f), repo = RepoRef(repo.owner, repo.name))
                }
            }
            item {
                Spacer(Modifier.height(12.dp))
                InfoCard("Timeline") { DiscussionTimelineView(discussion, RepoRef(repo.owner, repo.name)) }
            }
            if (!repo.isPrivate) {
                item {
                    Spacer(Modifier.height(12.dp))
                    FederatedRepliesCard(repo, kind = "discussion", number = discussion.number)
                }
            }
            item {
                Spacer(Modifier.height(12.dp))
                InfoCard("Signed discussion reply") {
                    Text(DiscussionDetail.REPLY_COPY)
                    Spacer(Modifier.height(12.dp))
                    SessionIdentityBar("Replying")
                    Spacer(Modifier.height(8.dp))
                    FmTextField(
                        value = reply,
                        onValueChange = { reply = it },
                        hint = DiscussionDetail.REPLY_HINT,
                        minLines = 3,
                        maxLines = 8,
                    )
                    Spacer(Modifier.height(12.dp))
                    FmInboxPendingNote()
                    Spacer(Modifier.height(12.dp))
                    CollabFilledButton(
                        label = "Submit reply",
                        icon = "reply_outlined",
                        onClick = {
                            val text = reply.trim()
                            if (text.isNotEmpty()) {
                                env.launch {
                                    if (!requireAccount("reply to discussions")) return@launch
                                    run(PENDING_NOTE) {
                                        inbox.commentOnDiscussion(repo.owner, repo.name, discussion.number, text)
                                    }
                                }
                            }
                        },
                    )
                    FmInlineStatus(env.status, Modifier.padding(top = 8.dp))
                }
            }
            item { NavigationBarSpacer() }
        }
    }
}

@Composable
private fun DiscussionTimelineView(discussion: RepoDiscussion, repo: RepoRef) {
    if (discussion.events.isEmpty()) {
        Text(DiscussionDetail.TIMELINE_EMPTY)
        return
    }
    val tint = Fm.colors.textTertiary
    Column(Modifier.fillMaxWidth()) {
        for (event in discussion.events) {
            TimelineTile(
                icon = "forum_outlined",
                iconTint = tint,
                title = "Reply",
                author = event.displayAuthor,
                body = event.body,
                repo = repo,
            )
        }
    }
}

@Composable
internal fun FederatedRepliesCard(repo: Repository, kind: String, number: Int) {
    val colors = Fm.colors
    val state = rememberLoad(repo.owner, repo.name, kind, number) {
        Services.require<ApiService>().federatedReplies(repo.owner, repo.name, kind, number)
    }
    InfoCard(FederatedThread.TITLE) {
        Text(FederatedThread.INTRO, style = Fm.text.bodySmall.copy(color = colors.textSecondary).withHeight(1.35f))
        Spacer(Modifier.height(12.dp))
        when (state) {
            Loadable.Loading -> FmLinearProgressIndicator()
            is Loadable.Failed -> Text(FederatedThread.UNAVAILABLE, color = colors.textTertiary)
            is Loadable.Ready -> if (state.value.isEmpty()) {
                Text(FederatedThread.EMPTY, color = colors.textTertiary)
            } else {
                Column(Modifier.fillMaxWidth()) {
                    for (reply in state.value) FederatedReplyTile(reply, RepoRef(repo.owner, repo.name))
                }
            }
        }
    }
}

@Composable
private fun FederatedReplyTile(reply: FederatedReply, repo: RepoRef) {
    val colors = Fm.colors
    val context = LocalContext.current
    val provenance = FederatedThread.provenance(reply)
    val unavailable = FederatedThread.unavailable(reply)
    val canOpen = FederatedThread.canOpenBacklink(reply.backlink)
    val micro = Fm.text.bodySmall.copy(fontSize = FmType.micro, color = colors.textTertiary)
    Column(
        Modifier
            .padding(start = (reply.depth * 14).dp, top = 8.dp, bottom = 8.dp)
            .fillMaxWidth()
            .drawBehind { drawRect(colors.border, size = Size(2.dp.toPx(), size.height)) }
            .padding(start = 12.dp),
    ) {
        FlowRow(
            horizontalArrangement = Arrangement.spacedBy(8.dp),
            verticalArrangement = Arrangement.spacedBy(4.dp),
            itemVerticalAlignment = Alignment.CenterVertically,
        ) {
            Text(FederatedThread.author(reply), fontWeight = FmType.emphasis)
            if (reply.edited) Text("(edited)", style = micro)
            if (provenance.isNotEmpty()) Text(provenance, style = micro)
        }
        Spacer(Modifier.height(6.dp))
        SelectableBody(
            reply.displayBody,
            style = LocalTextStyle.current.copy(
                color = if (unavailable) colors.textTertiary else colors.textPrimary,
                fontStyle = if (unavailable) FontStyle.Italic else FontStyle.Normal,
            ).withHeight(1.4f),
            repo = repo,
        )
        Spacer(Modifier.height(6.dp))
        FlowRow(
            horizontalArrangement = Arrangement.spacedBy(8.dp),
            itemVerticalAlignment = Alignment.CenterVertically,
        ) {
            Text(FederatedThread.NOT_NATIVE, style = micro)
            if (canOpen) {
                FmTextButton(onClick = {
                    if (FederatedThread.canOpenBacklink(reply.backlink)) openExternalUrl(context, reply.backlink)
                }) {
                    FmIcon("open_in_new", size = 14.dp)
                    Spacer(Modifier.width(8.dp))
                    Text("Original")
                }
            }
        }
    }
}
