package com.forkmesh.nativeapp.feature.account.settings

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalUriHandler
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.account.AuthService
import com.forkmesh.nativeapp.feature.agents.AgentsFeature
import com.forkmesh.nativeapp.feature.people.ProfileTipContent
import com.forkmesh.nativeapp.feature.tools.copyToClipboard
import com.forkmesh.nativeapp.model.OrgSummary
import com.forkmesh.nativeapp.settings.AccountDataModel
import com.forkmesh.nativeapp.settings.AccountProfileModel
import com.forkmesh.nativeapp.settings.AccountTime
import com.forkmesh.nativeapp.settings.DesktopNicknames
import com.forkmesh.nativeapp.settings.McpConnector
import com.forkmesh.nativeapp.settings.McpConnectorModel
import com.forkmesh.nativeapp.settings.RecentAccountsStore
import com.forkmesh.nativeapp.settings.SecurityReportDraft
import com.forkmesh.nativeapp.settings.SshKeyCheck
import com.forkmesh.nativeapp.settings.SshKeys
import com.forkmesh.nativeapp.settings.SshKeysModel
import com.forkmesh.nativeapp.ui.kit.FmAvatar
import com.forkmesh.nativeapp.ui.kit.FmCircularProgressIndicator
import com.forkmesh.nativeapp.ui.kit.FmInlineStatus
import com.forkmesh.nativeapp.ui.kit.FmListRow
import com.forkmesh.nativeapp.ui.kit.FmTextButton
import com.forkmesh.nativeapp.ui.kit.FmTextField
import com.forkmesh.nativeapp.ui.kit.LocalFmMessenger
import com.forkmesh.nativeapp.ui.kit.QrImage
import com.forkmesh.nativeapp.ui.kit.rememberFmStatus
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmSpace
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

class SshKeysScreen : Screen() {
    override val title: String get() = "SSH keys"

    private val model: SshKeysModel? by lazy { SettingsArea.endpoints?.let(::SshKeysModel) }
    private var revision by mutableIntStateOf(0)

    @Composable
    override fun Content(navigator: Navigator) {
        val scope = rememberCoroutineScope()
        val messenger = LocalFmMessenger.current
        val addStatus = rememberFmStatus()
        val model = model
        var key by remember { mutableStateOf("") }
        var label by remember { mutableStateOf("") }
        revision
        if (model != null) {
            LaunchedEffect(model) {
                withContext(Dispatchers.IO) { model.load() }
                revision++
            }
        }
        SettingsPage("SSH keys", navigator) {
            if (model == null) {
                item { SettingsNote("Sign in to manage SSH keys.") }
                return@SettingsPage
            }
            val page = model.page
            item {
                SettingsCard("Git over SSH") {
                    SettingsNote(page?.gatewayLine ?: "Loading…")
                    if (page != null && page.notice.isNotEmpty()) SettingsNote(page.notice)
                }
            }
            item {
                SettingsCard("Add a key") {
                    FmTextField(key, {
                        key = it
                        model.draftKey = it
                    }, Modifier.fillMaxWidth(), hint = "ssh-ed25519 AAAA…", minLines = 2, maxLines = 4)
                    FmTextField(label, {
                        label = it
                        model.draftLabel = it
                    }, Modifier.fillMaxWidth(), hint = "Label (optional)")
                    val check = model.draftCheck
                    if (key.isNotBlank() && check !is SshKeyCheck.Ok) SettingsNote(SshKeys.message(check), Fm.colors.danger)
                    if (page?.atLimit == true) SettingsNote("You have the most keys allowed (${page.maximumKeys}). Remove one first.")
                    FmTextButton(onClick = {
                        addStatus.clear()
                        scope.launch {
                            val added = withContext(Dispatchers.IO) { model.add() }
                            if (added) {
                                key = ""
                                label = ""
                                addStatus.show("Key added.")
                            }
                            revision++
                        }
                    }, enabled = model.canAdd) { Text("Add key") }
                    FmInlineStatus(addStatus)
                }
            }
            if (model.error.isNotEmpty()) item { SettingsNote(model.error, Fm.colors.danger) }
            if (model.loading && page == null) item { Box(Modifier.fillMaxWidth(), contentAlignment = Alignment.Center) { FmCircularProgressIndicator() } }
            items(page?.keys.orEmpty(), key = { it.id }) { row ->
                FmListRow(
                    row.title,
                    subtitle = "${row.fingerprint} · last used ${AccountTime.label(row.lastUsedAt, System.currentTimeMillis())}",
                    trailing = {
                        FmTextButton(onClick = {
                            scope.launch {
                                val sure = messenger.showConfirmDialog("Remove ${row.title}?", "Git over SSH stops accepting it at once.", confirmLabel = "Remove", destructive = true)
                                if (!sure) return@launch
                                withContext(Dispatchers.IO) { model.remove(row.id) }
                                revision++
                            }
                        }) { Text("Remove") }
                    },
                    showChevron = false,
                )
            }
        }
    }
}

class McpConnectorScreen : Screen() {
    override val title: String get() = "MCP connector"

    private var orgs by mutableStateOf<List<OrgSummary>>(emptyList())
    private var chosen by mutableStateOf<String?>(null)
    private var model by mutableStateOf<McpConnectorModel?>(null)
    private var revision by mutableIntStateOf(0)

    @Composable
    override fun Content(navigator: Navigator) {
        val scope = rememberCoroutineScope()
        val context = LocalContext.current
        val messenger = LocalFmMessenger.current
        val copyStatus = rememberFmStatus()
        LaunchedEffect(Unit) {
            orgs = withContext(Dispatchers.IO) { runCatching { SettingsArea.api?.myOrgs().orEmpty() }.getOrDefault(emptyList()) }.filter { it.canManage }
            if (chosen == null) orgs.firstOrNull()?.let { chosen = it.name }
        }
        LaunchedEffect(chosen) {
            val org = chosen ?: return@LaunchedEffect
            val endpoints = SettingsArea.endpoints ?: return@LaunchedEffect
            val next = McpConnectorModel(org, endpoints)
            model = next
            withContext(Dispatchers.IO) { next.load() }
            revision++
        }
        revision
        val current = model
        SettingsPage("MCP connector", navigator) {
            item {
                SettingsCard {
                    SettingsNote("Let Claude Code or Codex read and update your organization's tasks. Tokens can only touch tasks and expire on their own.")
                }
            }
            if (orgs.isEmpty()) {
                item { SettingsNote("You need to own or administer an organization to create a connector token.") }
                return@SettingsPage
            }
            item {
                SettingsCard("Organization") {
                    for (org in orgs) {
                        FmListRow(org.name, onTap = { chosen = org.name }, showChevron = false,
                            trailing = if (chosen == org.name) ({ Text("✓", style = Fm.text.bodyLarge.copy(color = Fm.colors.accent)) }) else null)
                    }
                }
            }
            if (current == null) return@SettingsPage
            current.minted?.let { minted ->
                item {
                    SettingsCard("New token") {
                        SettingsNote("Copy it now. It is shown once.")
                        Text(McpConnector.mask(minted.token), style = Fm.mono())
                        Row {
                            FmTextButton(onClick = {
                                copyToClipboard(context, McpConnector.claudeCommand(current.serverUrl, minted.token))
                                copyStatus.show("Command copied.")
                            }) { Text("Copy Claude Code command") }
                            FmTextButton(onClick = {
                                copyToClipboard(context, McpConnector.configJson(current.serverUrl, minted.token))
                                copyStatus.show("Config copied.")
                            }) { Text("Copy JSON config") }
                        }
                        FmInlineStatus(copyStatus)
                        FmTextButton(onClick = {
                            copyStatus.clear()
                            current.dismissMinted()
                            revision++
                        }) { Text("Done") }
                    }
                }
            }
            item {
                var label by remember(chosen) { mutableStateOf("") }
                var provider by remember(chosen) { mutableStateOf(current.provider) }
                SettingsCard("Create a token") {
                    Row {
                        for (p in McpConnector.providers) {
                            FmTextButton(onClick = { provider = p }, enabled = provider != p) { Text(McpConnector.providerLabel(p)) }
                        }
                    }
                    FmTextField(label, { label = it }, Modifier.fillMaxWidth(), hint = "Label (optional)")
                    SettingsNote("Server: ${current.serverUrl} · expires in ${McpConnector.DEFAULT_DAYS} days")
                    FmTextButton(onClick = {
                        current.provider = provider
                        current.label = label
                        scope.launch {
                            withContext(Dispatchers.IO) { current.mint() }
                            revision++
                        }
                    }, enabled = !current.busy) { Text("Create token") }
                }
            }
            if (current.error.isNotEmpty()) item { SettingsNote(current.error, Fm.colors.danger) }
            items(current.tokens, key = { it.id }) { row ->
                val now = System.currentTimeMillis()
                FmListRow(
                    row.title,
                    subtitle = "${row.status(now)} · last used ${AccountTime.label(row.lastUsedAt, now)}",
                    trailing = if (!row.isActive(now)) null else ({
                        Row {
                            FmTextButton(onClick = {
                                scope.launch {
                                    withContext(Dispatchers.IO) { current.rotate(row) }
                                    revision++
                                }
                            }) { Text("Rotate") }
                            FmTextButton(onClick = {
                                scope.launch {
                                    if (!messenger.showConfirmDialog("Revoke ${row.title}?", confirmLabel = "Revoke", destructive = true)) return@launch
                                    withContext(Dispatchers.IO) { current.revoke(row.id) }
                                    revision++
                                }
                            }) { Text("Revoke") }
                        }
                    }),
                    showChevron = false,
                )
            }
        }
    }
}

class SecurityReportScreen : Screen() {
    override val title: String get() = "Report a vulnerability"

    private val model: AccountDataModel? by lazy { SettingsArea.endpoints?.let(::AccountDataModel) }
    private var revision by mutableIntStateOf(0)

    @Composable
    override fun Content(navigator: Navigator) {
        val scope = rememberCoroutineScope()
        val model = model
        revision
        var draft by remember { mutableStateOf(SecurityReportDraft()) }
        SettingsPage("Report a vulnerability", navigator) {
            if (model == null) {
                item { SettingsNote("Sign in to report, or email security@forkmesh.com.") }
                return@SettingsPage
            }
            item {
                SettingsCard {
                    SettingsNote("Reports go straight to the security team and are not public. Please do not test against other people's data.")
                    FmTextField(draft.title, { draft = draft.copy(title = it) }, Modifier.fillMaxWidth(), hint = "Title")
                    Text("Component", style = Fm.text.labelMedium)
                    for ((id, label) in SecurityReportDraft.components) {
                        FmListRow(label, onTap = { draft = draft.copy(component = id) }, showChevron = false, minHeight = 40.dp,
                            trailing = if (draft.component == id) ({ Text("✓", style = Fm.text.bodyLarge.copy(color = Fm.colors.accent)) }) else null)
                    }
                    FmTextField(draft.body, { draft = draft.copy(body = it) }, Modifier.fillMaxWidth(), hint = "What happens and how to reproduce it", minLines = 5, maxLines = 12)
                    FmTextField(draft.contact, { draft = draft.copy(contact = it) }, Modifier.fillMaxWidth(), hint = "How to reach you (optional)")
                    FmTextButton(onClick = {
                        model.report = draft
                        scope.launch {
                            val sent = withContext(Dispatchers.IO) { model.sendReport() }
                            if (sent) draft = SecurityReportDraft()
                            revision++
                        }
                    }, enabled = !model.busy) { Text("Send report") }
                    if (model.reportSent) SettingsNote("Thank you. The security team will reply by email if you left a contact.")
                    if (model.error.isNotEmpty()) SettingsNote(model.error, Fm.colors.danger)
                }
            }
        }
    }
}

class AccountProfileScreen(private val name: String) : Screen() {
    override val title: String get() = "@$name"

    private val model: AccountProfileModel? by lazy { SettingsArea.endpoints?.let { AccountProfileModel(name, it) } }
    private var revision by mutableIntStateOf(0)

    @Composable
    override fun Content(navigator: Navigator) {
        val model = model
        if (model != null) {
            LaunchedEffect(model) {
                withContext(Dispatchers.IO) { model.load() }
                revision++
            }
        }
        revision
        val uri = LocalUriHandler.current
        SettingsPage("@${model?.name ?: name}", navigator) {
            val profile = model?.profile
            when {
                model == null -> item { SettingsNote("Profiles are unavailable on this server.") }
                model.error.isNotEmpty() -> item { SettingsNote(model.error, Fm.colors.danger) }
                profile == null -> item { Box(Modifier.fillMaxWidth(), contentAlignment = Alignment.Center) { FmCircularProgressIndicator() } }
                !profile.exists -> item { SettingsNote("There is no account called @${model.name}.") }
                profile.isPrivate -> item { SettingsNote("@${profile.name} keeps their profile private.") }
                else -> {
                    item {
                        SettingsCard {
                            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(FmSpace.x3)) {
                                FmAvatar(profile.name, 56.dp, avatarPng = profile.avatar)
                                Column {
                                    Text("@${profile.name}", style = Fm.text.titleMedium)
                                    SettingsNote("${profile.kindLabel} · ${profile.accountStatus}")
                                }
                            }
                            if (profile.bio.isNotEmpty()) Text(profile.bio, style = Fm.text.bodyMedium)
                            if (profile.location.isNotEmpty()) SettingsNote(profile.location)
                            SettingsNote("${profile.followers} followers · ${profile.following} following · ${profile.mirrorCount} mirrors")
                            if (profile.createdAt > 0) SettingsNote("Joined ${AccountTime.label(profile.createdAt, System.currentTimeMillis())}")
                            SettingsNote(profile.linkedNodesLine)
                        }
                    }
                    if (profile.links.isNotEmpty() || profile.mastodonUrl.isNotEmpty()) {
                        item {
                            SettingsCard("Links") {
                                for (link in profile.links) {
                                    FmListRow(
                                        link.label,
                                        subtitle = if (link.verified) "Verified · ${link.url}" else link.url,
                                        onTap = { runCatching { uri.openUri(link.url) } },
                                        minHeight = 44.dp,
                                    )
                                }
                                if (profile.mastodonUrl.isNotEmpty()) {
                                    FmListRow(
                                        profile.mastodon.ifEmpty { "Mastodon" },
                                        subtitle = "Mastodon",
                                        onTap = { runCatching { uri.openUri(profile.mastodonUrl) } },
                                        minHeight = 44.dp,
                                    )
                                }
                            }
                        }
                    }
                    if (!profile.isNode) {
                        item {
                            SettingsCard("Tips") { ProfileTipContent(profile.name, profile.tipAddress) }
                        }
                    }
                    if (profile.nodes.isNotEmpty()) {
                        item {
                            SettingsCard("Nodes") {
                                for (node in profile.nodes) {
                                    FmListRow(node, onTap = { navigator.push(AccountProfileScreen(node)) }, minHeight = 44.dp)
                                }
                            }
                        }
                    }
                    if (profile.isNode && profile.owner.isNotEmpty()) {
                        item {
                            SettingsCard("Owner") {
                                FmListRow("@${profile.owner}", subtitle = "The user this node belongs to", onTap = { navigator.push(AccountProfileScreen(profile.owner)) }, minHeight = 44.dp)
                            }
                        }
                    }
                    if (profile.identityQrText.isNotEmpty()) {
                        item {
                            SettingsCard("Keys") {
                                Column(Modifier.fillMaxWidth(), horizontalAlignment = Alignment.CenterHorizontally) {
                                    QrImage(profile.identityQrText, size = 140.dp, description = "Identity QR code")
                                    SettingsNote("Identity")
                                }
                                SelectionContainer { Text(profile.pubkey, style = Fm.mono()) }
                            }
                        }
                    }
                    if (profile.canLinkNode(SettingsArea.account)) {
                        item { FmTextButton(onClick = { navigator.push(LinkNodeScreen()) }) { Text("Link a node") } }
                    }
                }
            }
        }
    }
}

class DesktopsSettingsScreen : Screen() {
    override val title: String get() = "Desktops"

    private var revision by mutableIntStateOf(0)

    @Composable
    override fun Content(navigator: Navigator) {
        val scope = rememberCoroutineScope()
        val messenger = LocalFmMessenger.current
        revision
        val local = runCatching { AgentsFeature.localControl }.getOrNull()
        val paired = local?.paired.orEmpty()
        val names = SettingsArea.prefs?.let(::DesktopNicknames)
        SettingsPage("Desktops", navigator) {
            item {
                SettingsCard {
                    SettingsNote("Desktops that approved this phone. Rename one to tell them apart; forgetting one means pairing again.")
                    FmTextButton(onClick = { navigator.push(AgentsFeature.localDesktopScreen()) }) { Text("Pair a desktop") }
                }
            }
            if (paired.isEmpty()) item { SettingsNote("No desktops paired yet.") }
            items(paired, key = { it.key }) { desktop ->
                val shown = names?.name(desktop.key, desktop.name) ?: desktop.name.ifEmpty { desktop.key.take(12) }
                FmListRow(
                    shown,
                    subtitle = listOf(desktop.user.takeIf { it.isNotEmpty() }?.let { "@$it" }, "paired ${AccountTime.label(desktop.pairedAtMs, System.currentTimeMillis())}")
                        .filterNotNull().joinToString(" · "),
                    trailing = {
                        Row {
                            FmTextButton(onClick = {
                                scope.launch {
                                    val nick = messenger.showPromptDialog("Rename", initialValue = shown, confirmLabel = "Rename") ?: return@launch
                                    names?.rename(desktop.key, nick)
                                    revision++
                                }
                            }) { Text("Rename") }
                            FmTextButton(onClick = {
                                scope.launch {
                                    if (!messenger.showConfirmDialog("Forget $shown?", "You will need to pair again to control it.", confirmLabel = "Forget", destructive = true)) return@launch
                                    local?.forget(desktop.key)
                                    names?.forget(desktop.key)
                                    revision++
                                }
                            }) { Text("Forget") }
                        }
                    },
                    showChevron = false,
                )
            }
        }
    }
}

class AccountSwitchScreen : Screen() {
    override val title: String get() = "Account"

    private var revision by mutableIntStateOf(0)

    @Composable
    override fun Content(navigator: Navigator) {
        val scope = rememberCoroutineScope()
        val messenger = LocalFmMessenger.current
        val auth = Services.get<AuthService>()
        val store = SettingsArea.prefs?.let(::RecentAccountsStore)
        revision
        val current = SettingsArea.account
        val others = store?.others(current).orEmpty()
        SettingsPage("Account", navigator) {
            item {
                SettingsCard("Signed in") {
                    Text(if (SettingsArea.signedIn) "@$current" else "Not signed in", style = Fm.text.bodyLarge)
                    FmTextButton(onClick = { navigator.push(SettingsScreen()) }) { Text("Sign out or delete the account") }
                }
            }
            if (others.isNotEmpty()) {
                item {
                    SettingsCard("Recent accounts on this phone") {
                        SettingsNote("Switching signs this account out. Sign in again as the other account; your keys stay on the phone.")
                        for (recent in others) {
                            FmListRow(
                                recent.name,
                                subtitle = recent.subtitle,
                                trailing = {
                                    Row {
                                        FmTextButton(onClick = {
                                            scope.launch {
                                                if (!messenger.showConfirmDialog("Switch to @${recent.name}?", "You will be signed out of @$current first.", confirmLabel = "Switch")) return@launch
                                                auth?.logout()
                                            }
                                        }) { Text("Switch") }
                                        FmTextButton(onClick = {
                                            store?.forget(recent.name)
                                            revision++
                                        }) { Text("Forget") }
                                    }
                                },
                                showChevron = false,
                            )
                        }
                    }
                }
            }
        }
    }
}
