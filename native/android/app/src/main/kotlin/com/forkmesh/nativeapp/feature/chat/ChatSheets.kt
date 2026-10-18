package com.forkmesh.nativeapp.feature.chat

import com.forkmesh.nativeapp.navigation.Route
import com.forkmesh.nativeapp.feature.navigation.NavigationFeature
import com.forkmesh.nativeapp.chat.ChatProfileLink
import android.content.Context
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.HorizontalDivider
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.input.KeyboardCapitalization
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.api.InboxService
import com.forkmesh.nativeapp.chat.ChatComposerTools
import com.forkmesh.nativeapp.chat.ChatExtras
import com.forkmesh.nativeapp.chat.ChatHub
import com.forkmesh.nativeapp.chat.ChatIssueDraft
import com.forkmesh.nativeapp.chat.ChatRow
import com.forkmesh.nativeapp.chat.ChatVisitorPresence
import com.forkmesh.nativeapp.chat.MESH_CONVERSATION
import com.forkmesh.nativeapp.chat.OfficeFrames
import com.forkmesh.nativeapp.chat.RepoSlug
import com.forkmesh.nativeapp.chat.messageCopyText
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.agents.AgentsFeature
import com.forkmesh.nativeapp.feature.composer.ComposerFeature
import com.forkmesh.nativeapp.ui.kit.FmAlertDialog
import com.forkmesh.nativeapp.ui.kit.FmDialogActions
import com.forkmesh.nativeapp.ui.kit.FmIconTile
import com.forkmesh.nativeapp.ui.kit.FmListRow
import com.forkmesh.nativeapp.ui.kit.FmMessenger
import com.forkmesh.nativeapp.ui.kit.FmRowDivider
import com.forkmesh.nativeapp.ui.kit.FmSegmentTab
import com.forkmesh.nativeapp.ui.kit.FmSegmentTabBar
import com.forkmesh.nativeapp.ui.kit.FmStatus
import com.forkmesh.nativeapp.ui.kit.FmTextField
import com.forkmesh.nativeapp.ui.kit.copyMessageText
import com.forkmesh.nativeapp.ui.kit.showSelectableText
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmSpace
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

suspend fun newChannelFlow(messenger: FmMessenger, hub: ChatHub?): String? {
    val chat = hub ?: return null
    val managed = ChatDepth.office != null
    return messenger.showDialog<String> { done ->
        var name by remember { mutableStateOf("") }
        var kind by remember { mutableStateOf(0) }
        var members by remember { mutableStateOf("") }
        var error by remember { mutableStateOf<String?>(null) }
        var working by remember { mutableStateOf(false) }
        val scope = rememberCoroutineScope()
        val tabs = buildList {
            add(FmSegmentTab("Public"))
            if (managed) add(FmSegmentTab("Private"))
        }
        val invited = members.split(",").map { it.trim() }.filter { it.isNotEmpty() }
        val footer = when {
            kind == 1 && invited.isEmpty() ->
                "Only you can see this room until you invite someone. Nobody else, including administrators, is added."
            kind == 1 ->
                "Only you and " + invited.joinToString(", ") { "@$it" } +
                    " can see, join or read this room. Nobody else, including administrators, is added."
            managed -> "Anyone in this project's chat can read it. ForkBot and integrations can take part."
            else -> "Anyone in this project's chat can read it. Sign in to create a private room that only invited people can see."
        }
        FmAlertDialog(
            onDismissRequest = { done(null) },
            title = "New channel",
            actions = {
                FmDialogActions(
                    onCancel = { done(null) },
                    onConfirm = if (name.isBlank() || working) null else ({
                        working = true
                        scope.launch {
                            val result = withContext(Dispatchers.IO) {
                                when (kind) {
                                    1 -> runCatching {
                                        ChatDepth.office?.create(name, isPrivate = true, members = invited)?.conversation ?: ""
                                    }
                                    else -> chat.extras.addPublicChannel(name)?.let { Result.success(it) }
                                        ?: Result.failure(IllegalArgumentException("Use letters, digits, dashes or underscores."))
                                }
                            }
                            working = false
                            result.onSuccess { done(it.ifEmpty { null }) }.onFailure { error = it.message }
                        }
                    }),
                    confirmLabel = "Create",
                    busy = working,
                )
            },
        ) {
            FmTextField(
                value = name,
                onValueChange = { name = it },
                hint = "channel-name",
                autofocus = true,
                keyboardOptions = KeyboardOptions(capitalization = KeyboardCapitalization.None, autoCorrectEnabled = false),
                modifier = Modifier.fillMaxWidth(),
            )
            Spacer(Modifier.height(FmSpace.x3))
            FmSegmentTabBar(tabs = tabs, index = kind, onChanged = { kind = it })
            Spacer(Modifier.height(FmSpace.x2))
            Text(footer, style = Fm.text.bodySmall.copy(color = Fm.colors.textTertiary))
            if (kind == 1) {
                Spacer(Modifier.height(FmSpace.x3))
                FmTextField(
                    value = members,
                    onValueChange = { members = it },
                    hint = "usernames, separated by commas",
                    helperText = "Invite people",
                    keyboardOptions = KeyboardOptions(autoCorrectEnabled = false),
                    modifier = Modifier.fillMaxWidth(),
                )
            }
            error?.let {
                Spacer(Modifier.height(FmSpace.x2))
                Text(it, style = Fm.text.bodySmall.copy(color = Fm.colors.danger))
            }
        }
    }
}

suspend fun inviteFlow(messenger: FmMessenger, channel: String) {
    val chat = ChatDepth.hub ?: return
    val people = withContext(Dispatchers.IO) {
        chat.relay.roster().filter { !it.self && it.online && !ChatVisitorPresence.isTransientVisitor(name = it.name) }
    }
    messenger.showModalBottomSheet<Unit>(isScrollControlled = true) { close ->
        var invited by remember { mutableStateOf(setOf<String>()) }
        val scope = rememberCoroutineScope()
        Text("Invite to $channel", modifier = Modifier.padding(FmSpace.x4), style = Fm.text.titleMedium)
        if (people.isEmpty()) {
            Text(
                "Nobody else is online right now. Invitations go to devices that are connected.",
                modifier = Modifier.padding(horizontal = FmSpace.x4, vertical = FmSpace.x2),
                style = Fm.text.bodyMedium.copy(color = Fm.colors.textSecondary),
            )
        }
        LazyColumn {
            for (member in people) {
                item(key = member.id) {
                    FmListRow(
                        title = member.owner.ifEmpty { member.name },
                        subtitle = member.nodeName.ifEmpty { null },
                        trailing = if (member.id in invited) ({ FmIcon("check", size = 18.dp, tint = Fm.colors.success) }) else null,
                        showChevron = false,
                        onTap = if (member.id in invited) null else ({
                            scope.launch {
                                val ok = withContext(Dispatchers.IO) { chat.extras.invite(member.id, channel) }
                                if (ok) invited = invited + member.id
                            }
                        }),
                    )
                }
            }
        }
        FmListRow(title = "Done", onTap = { close(Unit) }, showChevron = false)
        Spacer(Modifier.height(FmSpace.x2))
    }
}

suspend fun pickReaction(messenger: FmMessenger): String? = messenger.showModalBottomSheet<String> { close ->
    Text("React", modifier = Modifier.padding(start = FmSpace.x4, end = FmSpace.x4, top = FmSpace.x4, bottom = FmSpace.x1), style = Fm.text.titleMedium)
    ChatReactionPicker(Modifier.weight(1f, fill = false)) { close(it) }
}

suspend fun pickEmoji(messenger: FmMessenger): String? = messenger.showModalBottomSheet<String> { close ->
    Text("Emoji", modifier = Modifier.padding(FmSpace.x4), style = Fm.text.titleMedium)
    ChatEmojiGrid(ChatComposerTools.emojiPicker) { close(ChatComposerTools.emojiPicker[it]) }
}

suspend fun fileIssueFlow(messenger: FmMessenger, status: FmStatus, draft: ChatIssueDraft) {
    val filed = messenger.showDialog<String> { done ->
        var repo by remember { mutableStateOf(draft.repo.slug) }
        var title by remember { mutableStateOf(draft.title) }
        var body by remember { mutableStateOf(draft.body) }
        var working by remember { mutableStateOf(false) }
        var error by remember { mutableStateOf<String?>(null) }
        val scope = rememberCoroutineScope()
        val target = RepoSlug.parse(repo)
        FmAlertDialog(
            onDismissRequest = { done(null) },
            title = "New issue",
            actions = {
                FmDialogActions(
                    onCancel = { done(null) },
                    onConfirm = if (working || target == null || title.isBlank()) null else ({
                        val chosen = target
                        working = true
                        scope.launch {
                            val inbox = Services.get<InboxService>()
                            if (inbox == null) {
                                working = false
                                error = "Sign in to file issues."
                                return@launch
                            }
                            val result = withContext(Dispatchers.IO) { runCatching { inbox.submitNewIssue(chosen.owner, chosen.name, title, body) } }
                            working = false
                            result.onSuccess { done("Issue \"$title\" was signed and sent to ${chosen.slug}.") }
                                .onFailure { error = it.message ?: it.toString() }
                        }
                    }),
                    confirmLabel = "File",
                    busy = working,
                )
            },
        ) {
            FmTextField(value = repo, onValueChange = { repo = it }, hint = "owner/name", helperText = "Repository", keyboardOptions = KeyboardOptions(autoCorrectEnabled = false), modifier = Modifier.fillMaxWidth())
            Spacer(Modifier.height(FmSpace.x2))
            FmTextField(value = title, onValueChange = { title = it }, hint = "Title", modifier = Modifier.fillMaxWidth())
            Spacer(Modifier.height(FmSpace.x2))
            FmTextField(value = body, onValueChange = { body = it }, hint = "Description", minLines = 4, maxLines = 10, modifier = Modifier.fillMaxWidth())
            error?.let {
                Spacer(Modifier.height(FmSpace.x2))
                Text(it, style = Fm.text.bodySmall.copy(color = Fm.colors.danger))
            }
        }
    }
    if (filed != null) status.show(filed)
}

fun defaultIssueRepo(): RepoSlug =
    ComposerFeature.repoContext.value?.let(RepoSlug::parse) ?: RepoSlug("forkmesh", "forkmesh")

suspend fun issueFromCommand(messenger: FmMessenger, status: FmStatus, argument: String, channel: String, author: String) {
    val first = argument.trim().takeWhile { !it.isWhitespace() }
    val repos = ArrayList<RepoSlug>()
    RepoSlug.parse(first)?.let { repos.add(it) }
    val current = ComposerFeature.repoContext.value?.let(RepoSlug::parse)
    current?.let { if (it !in repos) repos.add(it) }
    repos.add(RepoSlug("forkmesh", "forkmesh"))
    when (val result = ChatIssueDraft.parse(argument, repos, current, channel, author, System.currentTimeMillis())) {
        is ChatIssueDraft.Result.Ok -> fileIssueFlow(messenger, status, result.draft)
        is ChatIssueDraft.Result.Error -> status.error(
            when (val failure = result.failure) {
                ChatIssueDraft.Failure.Usage -> "Usage: /issue [owner/repo] <title>"
                ChatIssueDraft.Failure.NoRepositories -> "No repository to file the issue in."
                is ChatIssueDraft.Failure.UnknownRepository -> "Unknown repository ${failure.slug}."
            },
        )
    }
}

suspend fun messageActionsFlow(
    messenger: FmMessenger,
    context: Context,
    navigator: Navigator,
    row: ChatRow,
    inThread: Boolean,
    status: FmStatus,
) {
    val chat = ChatDepth.hub
    val message = row.message
    val conversation = message.conversation
    val relayRoom = chat != null && conversation != MESH_CONVERSATION && !OfficeFrames.isOffice(conversation)
    val copyText = row.text.ifEmpty { messageCopyText(message) }
    val profileName = ChatProfileLink.senderProfile(row)
    val deletable = row.deletable || (message.self && conversation != MESH_CONVERSATION && !OfficeFrames.isOffice(conversation))
    val hideable = chat != null
    if (!relayRoom && copyText.isEmpty() && profileName == null && !deletable && !hideable) return
    val sections = listOf(
        listOfNotNull(
            if (relayRoom && !inThread && chat.canReplyInThread(conversation)) MessageAction("thread", "Reply in thread", "forum_outlined") else null,
            if (relayRoom && row.editable) MessageAction("edit", "Edit message", "edit_outlined") else null,
            if (copyText.isNotEmpty()) MessageAction("copy", "Copy text", "copy_outlined") else null,
            if (copyText.isNotEmpty()) MessageAction("select", "Select text", "select_all_outlined") else null,
        ),
        listOfNotNull(
            if (row.text.isNotEmpty()) MessageAction("prompt", "Send to prompt", "text_snippet_outlined") else null,
            if (row.text.isNotEmpty()) MessageAction("issue", "Create issue", "bug_report_outlined") else null,
            profileName?.let { MessageAction("profile", "View $it", "account_circle") },
        ),
        listOfNotNull(
            if (deletable || hideable) MessageAction("delete", "Delete message", "delete_outline", danger = true) else null,
        ),
    ).filter { it.isNotEmpty() }
    val choice = messenger.showModalBottomSheet<String> { close ->
        if (relayRoom) {
            Spacer(Modifier.height(FmSpace.x1))
            ChatQuickReactions(onPick = { close("react:$it") }, onMore = { close("more") })
            if (sections.isNotEmpty()) HorizontalDivider(thickness = 1.dp, color = Fm.colors.border)
        }
        Column(Modifier.weight(1f, fill = false).verticalScroll(rememberScrollState()).padding(vertical = FmSpace.x1)) {
            sections.forEachIndexed { index, section ->
                if (index > 0) HorizontalDivider(Modifier.padding(horizontal = FmSpace.x4, vertical = FmSpace.x1), thickness = 1.dp, color = Fm.colors.border)
                for (action in section) MessageActionRow(action) { close(action.key) }
            }
        }
        Spacer(Modifier.height(FmSpace.x1))
    } ?: return
    when {
        choice.startsWith("react:") -> withContext(Dispatchers.IO) { chat?.toggleReaction(message, choice.removePrefix("react:")) }
        choice == "more" -> {
            val value = pickReaction(messenger) ?: return
            withContext(Dispatchers.IO) { chat?.toggleReaction(message, value) }
        }
        choice == "thread" -> navigator.push(ChatThreadScreen(conversation, message.id))
        choice == "copy" -> copyMessageText(context, copyText, status)
        choice == "select" -> showSelectableText(messenger, copyText)
        choice == "edit" -> {
            val next = messenger.showPromptDialog(title = "Edit message", initialValue = row.text, confirmLabel = "Save") ?: return
            if (next.isNotBlank() && next != row.text) withContext(Dispatchers.IO) { chat?.edit(message, next) }
        }
        choice == "profile" -> profileName?.let { NavigationFeature.open(Route.Profile(it)) }
        choice == "prompt" -> navigator.push(AgentsFeature.startSheetScreen(row.text))
        choice == "issue" -> fileIssueFlow(messenger, status, ChatIssueDraft.fromMessage(message, row.text, defaultIssueRepo(), conversation))
        choice == "delete" -> {
            val forEveryone = deleteScopeFlow(messenger, forEveryone = deletable, forMe = hideable) ?: return
            withContext(Dispatchers.IO) {
                when {
                    !forEveryone -> chat?.hideMessage(message)
                    chat != null -> chat.deleteMessage(message)
                    else -> ChatFeature.relay?.deleteMessage(message)
                }
            }
        }
    }
}

private data class MessageAction(val key: String, val title: String, val icon: String, val danger: Boolean = false)

@Composable
private fun MessageActionRow(action: MessageAction, onTap: () -> Unit) {
    val colors = Fm.colors
    FmListRow(
        title = action.title,
        leading = { FmIcon(action.icon, size = 20.dp, tint = if (action.danger) colors.danger else colors.textSecondary) },
        onTap = onTap,
        showChevron = false,
        titleColor = if (action.danger) colors.danger else null,
        minHeight = 44.dp,
        padding = PaddingValues(horizontal = FmSpace.x4, vertical = 0.dp),
    )
}

private suspend fun deleteScopeFlow(messenger: FmMessenger, forEveryone: Boolean, forMe: Boolean): Boolean? {
    if (forEveryone && forMe) {
        return messenger.showModalBottomSheet<Boolean> { close ->
            Spacer(Modifier.height(FmSpace.x2))
            Column {
                FmListRow(
                    title = "Delete for everyone",
                    subtitle = "Removes it for anyone whose device syncs this room.",
                    leading = { FmIconTile(icon = "delete_outline", tint = Fm.colors.danger) },
                    onTap = { close(true) },
                )
                FmRowDivider()
                FmListRow(
                    title = "Delete for me",
                    subtitle = "Removes it from this phone only.",
                    leading = { FmIconTile(icon = "visibility_off_outlined") },
                    onTap = { close(false) },
                )
            }
            Spacer(Modifier.height(FmSpace.x2))
        }
    }
    val ok = messenger.showConfirmDialog(
        title = "Delete message?",
        body = if (forEveryone) "This removes it for anyone whose device syncs this room." else "It is removed from this phone only. Others still see it.",
        confirmLabel = "Delete",
        destructive = true,
    )
    return if (ok) forEveryone else null
}

@Composable
fun ChatTypingLine(text: String) {
    if (text.isEmpty()) return
    Text(
        text,
        modifier = Modifier.fillMaxWidth().padding(horizontal = FmSpace.x4, vertical = 2.dp),
        style = Fm.text.bodySmall.copy(color = Fm.colors.textTertiary),
    )
}

fun typingLabelFor(conversation: String): String {
    val chat = ChatDepth.hub ?: return ""
    if (conversation == MESH_CONVERSATION || OfficeFrames.isOffice(conversation)) return ""
    return ChatExtras.typingLabel(chat.extras.typingNames(conversation))
}
