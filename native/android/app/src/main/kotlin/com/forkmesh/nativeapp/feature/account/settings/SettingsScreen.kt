package com.forkmesh.nativeapp.feature.account.settings

import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.defaultMinSize
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.LocalTextStyle
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.SideEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.account.AccountCard
import com.forkmesh.nativeapp.account.AuthFlowStart
import com.forkmesh.nativeapp.account.DeviceTone
import com.forkmesh.nativeapp.account.PING_SETTINGS_SUBTITLE
import com.forkmesh.nativeapp.account.PING_SETTINGS_TITLE
import com.forkmesh.nativeapp.account.ThisDeviceCard
import com.forkmesh.nativeapp.account.accountCard
import com.forkmesh.nativeapp.account.adoptedDisplayName
import com.forkmesh.nativeapp.account.deleteAccountConfirmError
import com.forkmesh.nativeapp.account.deleteAccountFailure
import com.forkmesh.nativeapp.account.deviceRowDetail
import com.forkmesh.nativeapp.account.deviceRowTimeline
import com.forkmesh.nativeapp.account.deviceRowTitle
import com.forkmesh.nativeapp.account.deviceTone
import com.forkmesh.nativeapp.account.fundingStatus
import com.forkmesh.nativeapp.account.hasRealSession
import com.forkmesh.nativeapp.account.isCurrentDevice
import com.forkmesh.nativeapp.account.nodesEmptyText
import com.forkmesh.nativeapp.account.nodesSummary
import com.forkmesh.nativeapp.account.profileCardText
import com.forkmesh.nativeapp.account.pushCanEnable
import com.forkmesh.nativeapp.account.pushPermissionLabel
import com.forkmesh.nativeapp.account.pushRelayLabel
import com.forkmesh.nativeapp.account.pushSendLabel
import com.forkmesh.nativeapp.account.pushTokenLabel
import com.forkmesh.nativeapp.account.shortDeviceKey
import com.forkmesh.nativeapp.account.showsDeleteAccount
import com.forkmesh.nativeapp.account.showsPingSettings
import com.forkmesh.nativeapp.account.showsPushCard
import com.forkmesh.nativeapp.account.showsUserStatus
import com.forkmesh.nativeapp.account.signedInAsName
import com.forkmesh.nativeapp.account.thisDeviceCard
import com.forkmesh.nativeapp.account.webSettingsUrl
import com.forkmesh.nativeapp.account.serveAsNodeNote
import com.forkmesh.nativeapp.account.projects.RelayFeature
import com.forkmesh.nativeapp.feature.account.projects.ProjectsService
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.settings.AccountSignIns
import com.forkmesh.nativeapp.feature.account.AccountFeature
import com.forkmesh.nativeapp.feature.account.AccountFilledButton
import com.forkmesh.nativeapp.feature.account.AccountLabeledField
import com.forkmesh.nativeapp.feature.account.AccountOutlinedButton
import com.forkmesh.nativeapp.feature.account.AccountScreen
import com.forkmesh.nativeapp.feature.account.AuthService
import com.forkmesh.nativeapp.feature.account.HGap
import com.forkmesh.nativeapp.feature.account.PortableIdentityActions
import com.forkmesh.nativeapp.feature.account.PortableIdentityResult
import com.forkmesh.nativeapp.feature.account.PushNotificationService
import com.forkmesh.nativeapp.feature.account.SettingsService
import com.forkmesh.nativeapp.feature.account.VGap
import com.forkmesh.nativeapp.feature.account.openExternalUrl
import com.forkmesh.nativeapp.feature.account.orgs.OrgsScreen
import com.forkmesh.nativeapp.feature.chat.ChatFeature
import com.forkmesh.nativeapp.feature.chat.ChatLinkPreviewSettingRow
import com.forkmesh.nativeapp.feature.chat.RelayConnection
import com.forkmesh.nativeapp.feature.chat.showChatRetentionSheet
import com.forkmesh.nativeapp.model.ChatRetention
import com.forkmesh.nativeapp.model.ChatRetentionException
import com.forkmesh.nativeapp.model.UserStatus
import com.forkmesh.nativeapp.model.auth.AccountDevice
import com.forkmesh.nativeapp.model.auth.AuthSession
import com.forkmesh.nativeapp.security.DeviceIdentity
import com.forkmesh.nativeapp.ui.kit.FmAlertDialog
import com.forkmesh.nativeapp.ui.kit.FmCard
import com.forkmesh.nativeapp.ui.kit.FmCircularProgressIndicator
import com.forkmesh.nativeapp.ui.kit.FmConfirmDialog
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmInlineStatus
import com.forkmesh.nativeapp.ui.kit.FmKeyValueRow
import com.forkmesh.nativeapp.ui.kit.FmListRow
import com.forkmesh.nativeapp.ui.kit.FmMessenger
import com.forkmesh.nativeapp.ui.kit.FmSectionHeader
import com.forkmesh.nativeapp.ui.kit.FmStatus
import com.forkmesh.nativeapp.ui.kit.FmStatusBadge
import com.forkmesh.nativeapp.ui.kit.FmSwitch
import com.forkmesh.nativeapp.ui.kit.FmTextButton
import com.forkmesh.nativeapp.ui.kit.FmTextField
import com.forkmesh.nativeapp.ui.kit.GUEST_CHAT_CHANNEL
import com.forkmesh.nativeapp.ui.kit.LocalFmMessenger
import com.forkmesh.nativeapp.ui.kit.rememberFmStatus
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmColors
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmRadius
import com.forkmesh.nativeapp.ui.theme.FmSpace
import com.forkmesh.nativeapp.ui.theme.FmType
import kotlin.coroutines.cancellation.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext

private fun disconnectRelay() = com.forkmesh.nativeapp.feature.chat.ChatFeature.disconnect()

class SettingsScreen : AccountScreen() {
    private val initial = Services.get<SettingsService>()
    private var name by mutableStateOf(initial?.displayName?.value.orEmpty())
    private var solana by mutableStateOf(initial?.solanaAddress?.value.orEmpty())
    private var room by mutableStateOf(initial?.room?.value.orEmpty())
    private var passphrase by mutableStateOf(initial?.passphrase?.value.orEmpty())
    private var github by mutableStateOf(initial?.githubToken?.value.orEmpty())
    private var gitlab by mutableStateOf(initial?.gitlabToken?.value.orEmpty())
    private var devicesLoadedFor: String? = null

    private val secretWrites = Mutex()

    override val title: String get() = "Settings"

    private fun saveSecret(write: suspend () -> Unit) {
        scope.launch { secretWrites.withLock { write() } }
    }

    @OptIn(ExperimentalLayoutApi::class)
    @Composable
    override fun Content(navigator: Navigator) {
        val settings = Services.require<SettingsService>()
        val auth = Services.require<AuthService>()
        val push = Services.get<PushNotificationService>()
        val identity = Services.get<DeviceIdentity>()
        val messenger = LocalFmMessenger.current
        val context = LocalContext.current
        val c = Fm.colors
        val chatStatus = rememberFmStatus()
        val accountStatus = rememberFmStatus()
        val session by auth.session.collectAsState()
        val guest by auth.isGuest.collectAsState()
        val devices by auth.devices.collectAsState()
        val devicesLoading by auth.devicesLoading.collectAsState()
        val devicesError by auth.devicesError.collectAsState()
        val relay by ChatFeature.shellState.collectAsState()
        val serverUrl by settings.serverUrl.collectAsState()
        val userStatus by auth.userStatus.collectAsState()
        val chatRetention by auth.chatRetention.collectAsState()
        val activeProject = Services.get<ProjectsService>()?.active?.collectAsState()?.value
        fun supports(feature: RelayFeature): Boolean = activeProject?.features?.has(feature) ?: true

        SideEffect {
            adoptedDisplayName(name, session)?.let { if (it != name) name = it }
        }
        val token = session?.sessionToken ?: ""
        LaunchedEffect(token) {
            if (devicesLoadedFor != token) {
                devicesLoadedFor = token
                scope.launch { auth.refreshDevices() }
                scope.launch { auth.refreshUserStatus() }
                scope.launch { auth.refreshChatRetention() }
            }
        }

        LazyColumn(Modifier.fillMaxSize(), contentPadding = PaddingValues(FmSpace.x4)) {
            item {
                Section("Profile") {
                    FmCard(modifier = Modifier.fillMaxWidth(), radius = FmRadius.lg) {
                        Column(Modifier.fillMaxWidth()) {
                            ProfileAccountCard(session, guest, userStatus)
                            if (showsUserStatus(session, guest)) {
                                VGap(FmSpace.x4)
                                UserStatusControls(auth)
                            }
                            VGap(FmSpace.x4)
                            AccountLabeledField(
                                label = "Display name",
                                value = name,
                                onValueChange = {
                                    name = it
                                    settings.setDisplayName(it.trim())
                                },
                            )
                            Text(
                                "Node id: ${identity?.publicKeyB64url.orEmpty()}",
                                overflow = TextOverflow.Ellipsis,
                                style = Fm.text.bodySmall.copy(color = c.textTertiary),
                            )
                        }
                    }
                }
            }
            item {
                Section("Membership") {
                    FmCard(modifier = Modifier.fillMaxWidth(), radius = FmRadius.lg, padding = PaddingValues(0.dp)) {
                        Column(Modifier.fillMaxWidth()) {
                            if (token.isNotEmpty() && !guest) {
                                MembershipStatusRow(token)
                                HorizontalDivider(thickness = 1.dp, color = c.border)
                            }
                            Text(
                                "ForkMesh Core is free. Paid memberships fund public relays, signed releases, security " +
                                    "work and maintenance.",
                                modifier = Modifier
                                    .padding(start = FmSpace.x4, top = FmSpace.x4, end = FmSpace.x4, bottom = FmSpace.x3),
                                style = noteStyle(),
                            )
                            HorizontalDivider(thickness = 1.dp, color = c.border)
                            FmListRow(
                                title = "Become a member",
                                modifier = Modifier.testTag("settings-membership-row"),
                                subtitle = "See plans on forkmesh.com",
                                leading = { FmIcon("favorite_outline_baseline", tint = c.textSecondary) },
                                trailing = { FmIcon("open_in_new", size = 16.dp, tint = c.textTertiary) },
                                showChevron = false,
                                onTap = { openExternalUrl(context, com.forkmesh.nativeapp.feature.account.projects.ProjectActions.membershipUrl()) },
                            )
                        }
                    }
                }
            }
            if (showsUserStatus(session, guest)) {
                item {
                    Section("Chat") {
                        FmCard(modifier = Modifier.fillMaxWidth(), radius = FmRadius.lg, padding = PaddingValues(0.dp)) {
                            Column(Modifier.fillMaxWidth()) {
                                FmListRow(
                                    title = "Chat messages disappear after",
                                    modifier = Modifier.testTag("settings-chat-retention-row"),
                                    subtitle = "${chatRetention.defaultLabel}. Long-press a channel or tap the timer in a chat to change one chat.",
                                    leading = { FmIcon("timer_outlined", tint = c.textSecondary) },
                                    onTap = {
                                        scope.launch {
                                            val choice = showChatRetentionSheet(
                                                messenger,
                                                "Chat messages disappear after",
                                                ChatRetention.clamp(chatRetention.defaultMs),
                                            ) ?: return@launch
                                            chatStatus.clear()
                                            try {
                                                auth.updateChatRetention(ChatRetention.defaultBody(choice.ms))
                                            } catch (refused: ChatRetentionException) {
                                                chatStatus.error(refused.message ?: "Could not save when messages disappear.")
                                            }
                                        }
                                    },
                                )
                                ChatLinkPreviewSettingRow()
                            }
                        }
                        FmInlineStatus(chatStatus, Modifier.padding(top = FmSpace.x2))
                    }
                }
            }
            item {
                Section("Appearance") {
                    FmCard(modifier = Modifier.fillMaxWidth(), radius = FmRadius.lg) { AppearanceCard(navigator) }
                }
            }
            item {
                Section("Funding and payouts") {
                    FmCard(modifier = Modifier.fillMaxWidth(), radius = FmRadius.lg) {
                        Column(Modifier.fillMaxWidth()) {
                            FundingPayoutStatus(session)
                            VGap(FmSpace.x3)
                            AccountLabeledField(
                                label = "Public Solana address (payouts/donations, optional)",
                                value = solana,
                                onValueChange = {
                                    solana = it
                                    settings.setSolanaAddress(it.trim())
                                },
                            )
                            VGap(FmSpace.x3)
                            ChatTipsRow(settings)
                            VGap(FmSpace.x3)
                            Text(
                                "Non-custodial: ForkMesh saves only this public payout address. Wallet keys and recovery " +
                                    "phrases remain on your device. User-owned funds, the public community pool, pending " +
                                    "allocations, and finalized transfers are separate states. Legacy Worker bounty wallets " +
                                    "are frozen for offline migration; never fund one or paste a private key or seed phrase.",
                                style = noteStyle(),
                            )
                        }
                    }
                }
            }
            if (supports(RelayFeature.orgs)) item {
                Section("Organizations") {
                    FmCard(modifier = Modifier.fillMaxWidth(), radius = FmRadius.lg, padding = PaddingValues(0.dp)) {
                        FmListRow(
                            title = "Manage organizations",
                            modifier = Modifier.testTag("settings-organizations-row"),
                            subtitle = "Orgs, members, teams, and linked repos",
                            leading = { FmIcon("apartment_outlined", tint = c.textSecondary) },
                            onTap = { navigator.push(OrgsScreen()) },
                        )
                    }
                }
            }
            if (!guest && hasRealSession(session)) item {
                Section("Apps") {
                    FmCard(modifier = Modifier.fillMaxWidth(), radius = FmRadius.lg, padding = PaddingValues(0.dp)) {
                        FmListRow(
                            title = "Installed apps",
                            modifier = Modifier.testTag("settings-installed-apps-row"),
                            subtitle = "Apps that act for you or your organizations",
                            leading = { FmIcon("extension", tint = c.textSecondary) },
                            onTap = { navigator.push(AccountFeature.installedAppsScreen()) },
                        )
                    }
                }
            }
            if (!guest && hasRealSession(session)) item {
                Section("Apps") {
                    FmCard(modifier = Modifier.fillMaxWidth(), radius = FmRadius.lg, padding = PaddingValues(0.dp)) {
                        FmListRow(
                            title = "Installed apps",
                            modifier = Modifier.testTag("settings-installed-apps-row"),
                            subtitle = "Apps, bots and slash commands you or your orgs installed",
                            leading = { FmIcon("extension_outlined", tint = c.textSecondary) },
                            onTap = { navigator.push(AccountFeature.installedAppsScreen()) },
                        )
                    }
                }
            }
            item {
                Section("This device") {
                    FmCard(modifier = Modifier.fillMaxWidth(), radius = FmRadius.lg) {
                        ThisDeviceCardView(session, identity, settings)
                    }
                    VGap(FmSpace.x3)
                    FmCard(modifier = Modifier.fillMaxWidth(), radius = FmRadius.lg) {
                        IdentityKeyCard(identity)
                    }
                }
            }
            if (showsPingSettings(session, guest)) {
                item {
                    Section("Pings") {
                        FmCard(modifier = Modifier.fillMaxWidth(), radius = FmRadius.lg, padding = PaddingValues(0.dp)) {
                            FmListRow(
                                title = PING_SETTINGS_TITLE,
                                modifier = Modifier.testTag("settings-pings-row"),
                                subtitle = PING_SETTINGS_SUBTITLE,
                                leading = { FmIcon("tune_rounded", tint = c.textSecondary) },
                                onTap = { navigator.push(AccountFeature.pingSettingsScreen()) },
                            )
                        }
                    }
                }
            }
            if (push != null && showsPushCard(push.supported, guest)) {
                item {
                    Section("Phone banners") {
                        FmCard(modifier = Modifier.fillMaxWidth(), radius = FmRadius.lg) { PushNotificationsCard(push) }
                    }
                }
            }
            if (supports(RelayFeature.devices)) item {
                Section("Nodes on this account") {
                    FmCard(modifier = Modifier.fillMaxWidth(), radius = FmRadius.lg) {
                        AccountNodesCard(
                            session = session,
                            refreshed = devices,
                            loading = devicesLoading,
                            error = devicesError,
                            onRefresh = { scope.launch { auth.refreshDevices() } },
                            webUrl = webSettingsUrl(serverUrl),
                        )
                    }
                }
            }
            val signedIn = session
            if (signedIn != null && hasRealSession(signedIn) && signedIn.sessionKind != "preview") {
                item {
                    Section(AccountSignIns.TITLE) {
                        FmCard(modifier = Modifier.fillMaxWidth(), radius = FmRadius.lg) {
                            AccountSessionsCard(signedIn.sessionToken, auth, accountStatus)
                        }
                    }
                }
            }
            item {
                Section("Projects") {
                    FmCard(modifier = Modifier.fillMaxWidth(), radius = FmRadius.lg) {
                        com.forkmesh.nativeapp.feature.account.projects.ProjectsCard(navigator)
                    }
                }
            }
            item {
                Section("Relay") {
                    FmCard(modifier = Modifier.fillMaxWidth(), radius = FmRadius.lg) {
                        Column(Modifier.fillMaxWidth()) {
                            Text(
                                "Environment: ${settings.core.env.describe(serverUrl)}",
                                modifier = Modifier.padding(bottom = FmSpace.x3),
                                style = Fm.text.bodySmall.copy(color = c.textSecondary),
                            )
                            AccountLabeledField(
                                label = "Room",
                                value = room,
                                onValueChange = {
                                    room = it
                                    settings.setRoom(it.trim())
                                },
                            )
                            AccountLabeledField(
                                label = "Passphrase",
                                value = passphrase,
                                onValueChange = {
                                    passphrase = it
                                    val value = it.trim()
                                    saveSecret { settings.setPassphrase(value) }
                                },
                                obscure = true,
                            )
                            VGap(FmSpace.x2)
                            Text(
                                "Public #channels are controller-readable so ForkBot, MCP, and webhooks can participate. " +
                                    "Direct messages are automatically encrypted to known participant devices beyond the " +
                                    "controller room key. A brand-new device cannot open protected messages sent before " +
                                    "its first visit.",
                                style = noteStyle(),
                            )
                            VGap(FmSpace.x2)
                            FlowRow(
                                horizontalArrangement = Arrangement.spacedBy(FmSpace.x3),
                                verticalArrangement = Arrangement.spacedBy(FmSpace.x2),
                            ) {
                                AccountFilledButton(label = "Reconnect", icon = "refresh", onClick = { ChatFeature.reconnect() })
                                AccountOutlinedButton(
                                    label = "Disconnect",
                                    onClick = if (relay.connection == RelayConnection.Offline) null else ::disconnectRelay,
                                    icon = "logout",
                                )
                                Box(Modifier.align(Alignment.CenterVertically)) { RelayStateBadge(relay.connection) }
                            }
                        }
                    }
                }
            }
            if (supports(RelayFeature.imports)) item {
                Section("Import tokens") {
                    FmCard(modifier = Modifier.fillMaxWidth(), radius = FmRadius.lg) {
                        Column(Modifier.fillMaxWidth()) {
                            Text(
                                "Personal access tokens used to authenticate clones when importing a repo from " +
                                    "GitHub/GitLab (raises rate limits). Stored locally only.",
                                style = noteStyle(),
                            )
                            VGap(FmSpace.x3)
                            AccountLabeledField(
                                label = "GitHub token",
                                value = github,
                                onValueChange = {
                                    github = it
                                    val value = it.trim()
                                    saveSecret { settings.setGithubToken(value) }
                                },
                                obscure = true,
                            )
                            AccountLabeledField(
                                label = "GitLab token",
                                value = gitlab,
                                onValueChange = {
                                    gitlab = it
                                    val value = it.trim()
                                    saveSecret { settings.setGitlabToken(value) }
                                },
                                obscure = true,
                            )
                        }
                    }
                }
            }
            item {
                Section("Account") {
                    FmCard(modifier = Modifier.fillMaxWidth(), radius = FmRadius.lg) {
                        when (accountCard(guest)) {
                            AccountCard.Guest -> GuestAccountCard(onLeave = { auth.exitGuestMode() })
                            AccountCard.SignOut -> SignOutCard(
                                signedInAs = signedInAsName(session),
                                onSignOut = { confirmSignOut(messenger, auth, accountStatus) },
                            )
                        }
                    }
                    if (showsDeleteAccount(session)) {
                        VGap(FmSpace.x3)
                        FmCard(modifier = Modifier.fillMaxWidth(), radius = FmRadius.lg) {
                            DeleteAccountCard(onDelete = { confirmDeleteAccount(messenger, auth, accountStatus) })
                        }
                    }
                    FmInlineStatus(accountStatus, Modifier.padding(top = FmSpace.x2))
                }
            }
            if (supports(RelayFeature.ads)) item {
                Section("Sponsored messages") {
                    FmCard(modifier = Modifier.fillMaxWidth(), radius = FmRadius.lg) {
                        SponsoredMessagesRow(settings)
                    }
                }
            }
            item {
                Section("Legal", bottomGap = false) {
                    FmCard(modifier = Modifier.fillMaxWidth(), radius = FmRadius.lg) {
                        Column(Modifier.fillMaxWidth()) {
                            LegalLinkRow("Terms of Service") { openExternalUrl(context, com.forkmesh.nativeapp.feature.account.projects.ProjectActions.termsUrl()) }
                            HorizontalDivider(thickness = 1.dp, color = c.border)
                            LegalLinkRow("Privacy Policy") { openExternalUrl(context, com.forkmesh.nativeapp.feature.account.projects.ProjectActions.privacyUrl()) }
                        }
                    }
                }
            }
            if (supports(RelayFeature.ads)) item { SponsoredAdCard(topGap = true) }
        }
    }

    private fun confirmSignOut(messenger: FmMessenger, auth: AuthService, status: FmStatus) {
        scope.launch {
            status.clear()
            val confirmed = messenger.showDialog<Boolean> { done ->
                FmAlertDialog(
                    onDismissRequest = { done(null) },
                    title = "Sign out?",
                    containerColor = Fm.colors.bgOverlay,
                    shape = RoundedCornerShape(FmRadius.lg),
                    actions = {
                        FmTextButton(onClick = { done(false) }) { Text("Cancel") }
                        Button(onClick = { done(true) }) { Text("Sign out") }
                    },
                    content = {
                        Text(
                            "This clears the saved mobile session on this device. Your identity key, relay settings, " +
                                "and tokens stay local.",
                        )
                    },
                )
            }
            if (confirmed != true) return@launch
            auth.logout()
            if (auth.hasAppAccess.value) status.show("Signed out. Sign in with another account.")
        }
    }

    private fun confirmDeleteAccount(messenger: FmMessenger, auth: AuthService, status: FmStatus) {
        scope.launch {
            val deleted = messenger.showDialog<Boolean> { done -> DeleteAccountDialog(auth, done) }
            if (deleted == true && auth.hasAppAccess.value) status.show("Account deleted.")
        }
    }
}

@Composable
private fun Section(title: String, bottomGap: Boolean = true, content: @Composable ColumnScope.() -> Unit) {
    Column(Modifier.fillMaxWidth()) {
        FmSectionHeader(title = title)
        VGap(FmSpace.x2)
        content()
        if (bottomGap) VGap(FmSpace.x5)
    }
}

@Composable
private fun emphasisStyle(): TextStyle = Fm.text.bodyMedium.copy(fontWeight = FmType.emphasis)

@Composable
private fun noteStyle(): TextStyle =
    Fm.text.bodySmall.copy(color = Fm.colors.textSecondary, lineHeight = FmType.meta * FmType.leadingSnug)

@Composable
private fun microStyle(): TextStyle = Fm.text.bodySmall.copy(
    fontSize = FmType.micro,
    lineHeight = FmType.micro * FmType.leadingSnug,
    color = Fm.colors.textTertiary,
)

@Composable
private fun ProfileAccountCard(session: AuthSession?, guest: Boolean, status: UserStatus) {
    val c = Fm.colors
    val text = profileCardText(session, guest)
    val now = rememberNowMillis()
    val showsStatus = showsUserStatus(session, guest)
    val emoji = if (showsStatus) status.displayEmoji(now) else ""
    Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
        Box(
            Modifier
                .size(42.dp)
                .background(c.textPrimary, CircleShape),
            contentAlignment = Alignment.Center,
        ) {
            FmIcon("person_outline", tint = if (c.dark) FmColors.darkBgBase else FmColors.bgRaised)
        }
        HGap(FmSpace.x3)
        Column(Modifier.weight(1f)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(
                    text.title,
                    modifier = Modifier.weight(1f, fill = false),
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                    style = emphasisStyle(),
                )
                if (emoji.isNotEmpty()) {
                    HGap(FmSpace.x1)
                    Text(emoji, modifier = Modifier.testTag("settings-profile-status-emoji"), style = emphasisStyle())
                }
            }
            VGap(FmSpace.x1)
            Text(
                text.subtitle,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
                style = Fm.text.bodySmall.copy(color = c.textSecondary),
            )
        }
        HGap(FmSpace.x2)
        if (guest) {
            FmStatusBadge(label = "Guest", color = c.textSecondary)
        } else if (showsStatus && status.away) {
            FmStatusBadge(label = "Away", color = c.warning)
        } else {
            FmStatusBadge(label = "Active", color = c.success)
        }
    }
}

@Composable
private fun FundingPayoutStatus(session: AuthSession?) {
    val c = Fm.colors
    val status = fundingStatus(session)
    Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.Top) {
        FmIcon(
            if (status.connected) "verified_outlined" else "info_outline",
            size = 22.dp,
            tint = if (status.connected) c.success else c.textTertiary,
        )
        HGap(FmSpace.x3)
        Column(Modifier.weight(1f)) {
            Text(status.title, style = emphasisStyle())
            VGap(FmSpace.x1)
            Text(status.detail, overflow = TextOverflow.Ellipsis, style = Fm.text.bodySmall.copy(color = c.textSecondary))
        }
    }
}

@Composable
private fun CardEmptyState(title: String, body: String) {
    Column(Modifier.fillMaxWidth()) {
        Text(title, style = emphasisStyle())
        VGap(FmSpace.x2)
        Text(body, style = noteStyle())
    }
}

@Composable
private fun TintedTile(icon: String, color: Color) {
    Box(
        Modifier
            .size(36.dp)
            .background(color.copy(alpha = 28f / 255f), RoundedCornerShape(FmRadius.md)),
        contentAlignment = Alignment.Center,
    ) {
        FmIcon(icon, size = 20.dp, tint = color)
    }
}

@Composable
private fun ThisDeviceCardView(session: AuthSession?, identity: DeviceIdentity?, settings: SettingsService) {
    when (val card = thisDeviceCard(session)) {
        ThisDeviceCard.SignInToRegister -> CardEmptyState(
            title = "Sign in to register this phone",
            body = "Logging in binds this phone's key to your account as a mobile node so it can publish " +
                "repositories and sign as you.",
        )
        ThisDeviceCard.NotRegistered -> Column(Modifier.fillMaxWidth()) {
            CardEmptyState(
                title = "Not registered as a mobile node",
                body = "This session was created without binding this phone's key. Sign out and sign in again to " +
                    "register it as a mobile node that can publish and sign as you.",
            )
            VGap(FmSpace.x2)
            Text("Key: ${identity?.shortKey.orEmpty()}", style = microStyle())
        }
        is ThisDeviceCard.MobileNode -> {
            val color = Fm.colors.success
            Column(Modifier.fillMaxWidth()) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    TintedTile("phone_iphone", color)
                    HGap(FmSpace.x3)
                    Column(Modifier.weight(1f)) {
                        Text(card.label, maxLines = 1, overflow = TextOverflow.Ellipsis, style = emphasisStyle())
                        VGap(FmSpace.x1)
                        FmStatusBadge(label = "Mobile node", color = color)
                    }
                }
                VGap(FmSpace.x3)
                FmKeyValueRow(label = "Device", value = card.deviceId)
                FmKeyValueRow(label = "Can", value = card.capabilities)
                FmKeyValueRow(label = "Last seen", value = card.lastSeen)
                FmKeyValueRow(label = "Key", value = identity?.shortKey.orEmpty())
                VGap(FmSpace.x2)
                Text(
                    "This phone publishes and signs as you. It never hosts or mirrors repositories.",
                    style = noteStyle(),
                )
                if (card.canServeAsNode) {
                    VGap(FmSpace.x3)
                    ServeAsNodeRow(settings)
                }
            }
        }
    }
}

@Composable
private fun ServeAsNodeRow(settings: SettingsService) {
    val enabled by settings.serveAsNode.collectAsState()
    val project = Services.get<ProjectsService>()?.active?.collectAsState()?.value
    val supported = project?.features?.has(RelayFeature.nodeSync) ?: true
    Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.Top) {
        Column(Modifier.weight(1f)) {
            Text("Serve as a node", style = emphasisStyle())
            VGap(FmSpace.x1)
            Text(
                serveAsNodeNote(supported, project?.label.orEmpty().ifEmpty { project?.host.orEmpty() }),
                style = if (supported) noteStyle() else noteStyle().copy(color = Fm.colors.warning),
            )
        }
        HGap(FmSpace.x3)
        FmSwitch(
            checked = enabled && supported,
            enabled = supported,
            onCheckedChange = { settings.setServeAsNode(it) },
            modifier = Modifier.testTag("serve-as-node"),
        )
    }
}

@Composable
private fun ChatTipsRow(settings: SettingsService) {
    val enabled by settings.chatTipsEnabled.collectAsState()
    Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.Top) {
        Column(Modifier.weight(1f)) {
            Text("Accept tips in chat", style = emphasisStyle())
            VGap(FmSpace.x1)
            Text(
                "Shows this Solana address to everyone in shared chat rooms, guests included, so they can tip you. " +
                    "Off by default.",
                style = noteStyle(),
            )
        }
        HGap(FmSpace.x3)
        FmSwitch(
            checked = enabled,
            onCheckedChange = { settings.setChatTipsEnabled(it) },
            modifier = Modifier.testTag("chat-tips"),
        )
    }
}

@Composable
private fun SponsoredMessagesRow(settings: SettingsService) {
    val enabled by settings.showSponsoredMessages.collectAsState()
    Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.Top) {
        Column(Modifier.weight(1f)) {
            Text("Show sponsored messages", style = emphasisStyle())
            VGap(FmSpace.x1)
            Text(
                "One text-only, human-reviewed message from a ForkMesh member at the bottom of Settings. Chosen by " +
                    "placement alone: nothing about you or your account is sent.",
                style = noteStyle(),
            )
        }
        HGap(FmSpace.x3)
        FmSwitch(
            checked = enabled,
            onCheckedChange = { settings.setShowSponsoredMessages(it) },
            modifier = Modifier.testTag("show-sponsored-messages"),
        )
    }
}

@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun IdentityKeyCard(identity: DeviceIdentity?) {
    val context = LocalContext.current
    val messenger = LocalFmMessenger.current
    val scope = rememberCoroutineScope()
    val status = rememberFmStatus()
    var pendingExport by rememberSaveable { mutableStateOf<String?>(null) }
    val saveLauncher = rememberLauncherForActivityResult(ActivityResultContracts.CreateDocument("application/json")) { uri ->
        val document = pendingExport
        pendingExport = null
        if (uri != null && document != null) {
            scope.launch {
                val written = withContext(Dispatchers.IO) {
                    runCatching {
                        context.contentResolver.openOutputStream(uri, "wt")?.use { it.write(document.toByteArray(Charsets.UTF_8)) } != null
                    }.getOrDefault(false)
                }
                status.report(
                    if (written) PortableIdentityResult.Exported(document).message else "Could not write the recovery kit.",
                    !written,
                )
            }
        }
    }
    val openLauncher = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri == null) return@rememberLauncherForActivityResult
        scope.launch {
            val document = withContext(Dispatchers.IO) {
                runCatching {
                    context.contentResolver.openInputStream(uri)?.use { String(it.readBytes(), Charsets.UTF_8) }
                }.getOrNull()
            }
            if (document == null) {
                status.error("Could not read the recovery kit.")
                return@launch
            }
            val passphrase = messenger.showDialog<String> { done ->
                PassphraseDialog(title = "Decrypt recovery kit", body = "Enter the recovery-kit passphrase.", confirmLabel = "Decrypt", done = done)
            }
            if (passphrase.isNullOrEmpty()) return@launch
            val replace = messenger.showConfirmDialog(
                title = "Replace identity?",
                body = "Importing a recovery kit replaces this device's current identity key. Continue?",
                confirmLabel = "Replace",
                destructive = true,
            )
            if (!replace) return@launch
            val result = PortableIdentityActions.importIdentity(document, passphrase)
            status.report(result.message, result !is PortableIdentityResult.Imported)
        }
    }
    val export: () -> Unit = {
        if (!PortableIdentityActions.available) {
            status.error(PortableIdentityActions.NOT_AVAILABLE_MESSAGE)
        } else {
            scope.launch {
                val passphrase = messenger.showDialog<String> { done ->
                    PassphraseDialog(
                        title = "Encrypt recovery kit",
                        body = "Choose a passphrase of at least 12 characters. You'll need it to restore the key — " +
                            "it cannot be recovered.",
                        confirmLabel = "Encrypt",
                        done = done,
                    )
                }
                if (passphrase.isNullOrEmpty()) return@launch
                when (val result = PortableIdentityActions.exportIdentity(passphrase)) {
                    is PortableIdentityResult.Exported -> {
                        pendingExport = result.document
                        saveLauncher.launch(PortableIdentityActions.FILE_NAME)
                    }
                    else -> status.error(result.message)
                }
            }
        }
    }
    val import: () -> Unit = {
        if (!PortableIdentityActions.available) {
            status.error(PortableIdentityActions.NOT_AVAILABLE_MESSAGE)
        } else {
            openLauncher.launch(arrayOf("application/json", "*/*"))
        }
    }
    Column(Modifier.fillMaxWidth()) {
        Text("Identity key", style = emphasisStyle())
        VGap(FmSpace.x2)
        Text(
            "Your Ed25519 identity key signs everything you publish. Export it to a portable passphrase-encrypted " +
                "recovery kit and keep it somewhere safe. On a new device, import it to keep your name and signatures.",
            style = noteStyle(),
        )
        VGap(FmSpace.x2)
        FmKeyValueRow(label = "Key", value = identity?.shortKey ?: "—")
        VGap(FmSpace.x3)
        FlowRow(
            horizontalArrangement = Arrangement.spacedBy(FmSpace.x3),
            verticalArrangement = Arrangement.spacedBy(FmSpace.x2),
        ) {
            AccountOutlinedButton(label = "Save recovery kit…", onClick = export, icon = "save_alt")
            FmTextButton(onClick = import, modifier = Modifier.align(Alignment.CenterVertically)) {
                Text("Import keyfile…")
            }
        }
        FmInlineStatus(status, Modifier.padding(top = FmSpace.x2))
    }
}

@Composable
private fun PassphraseDialog(title: String, body: String, confirmLabel: String, done: (String?) -> Unit) {
    var text by remember { mutableStateOf("") }
    FmConfirmDialog(
        title = title,
        onConfirm = { done(text) },
        onDismissRequest = { done(null) },
        body = body,
        confirmLabel = confirmLabel,
    ) {
        FmTextField(
            value = text,
            onValueChange = { text = it },
            autofocus = true,
            keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Password),
            visualTransformation = PasswordVisualTransformation(),
        )
    }
}

@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun PushNotificationsCard(push: PushNotificationService) {
    val c = Fm.colors
    val scope = rememberCoroutineScope()
    val testStatus = rememberFmStatus()
    val permission by push.permission.collectAsState()
    val environment by push.environment.collectAsState()
    val registered by push.registered.collectAsState()
    val relayConfigured by push.relayConfigured.collectAsState()
    val lastError by push.lastError.collectAsState()
    val token = push.binder.token
    var sending by remember { mutableStateOf(false) }
    Column(Modifier.fillMaxWidth()) {
        Text(
            "While ForkMesh is closed, iOS wakes it with a payload-free alert for pings, issue activity and " +
                "direct messages — Apple is told an account had activity and nothing else. The phone then fills the " +
                "banner in over your signed session, so you see who acted, on which repository, and what they wrote.",
            style = noteStyle(),
        )
        VGap(FmSpace.x2)
        FmKeyValueRow(label = "Permission", value = pushPermissionLabel(permission))
        FmKeyValueRow(label = "Token", value = pushTokenLabel(token, permission, environment, registered))
        FmKeyValueRow(label = "Relay", value = pushRelayLabel(relayConfigured))
        if (lastError.isNotEmpty()) {
            VGap(FmSpace.x1)
            Text(lastError, style = Fm.text.bodySmall.copy(color = c.danger, lineHeight = FmType.meta * FmType.leadingSnug))
        }
        if (permission == "denied") {
            VGap(FmSpace.x1)
            Text("Turn notifications back on for ForkMesh in iOS Settings, then reopen the app.", style = noteStyle())
        }
        VGap(FmSpace.x3)
        FlowRow(
            horizontalArrangement = Arrangement.spacedBy(FmSpace.x3),
            verticalArrangement = Arrangement.spacedBy(FmSpace.x2),
        ) {
            if (pushCanEnable(token, permission)) {
                AccountFilledButton(
                    label = "Enable notifications",
                    icon = "notifications_active_outlined",
                    onClick = { scope.launch { push.enable() } },
                )
            }
            AccountOutlinedButton(
                label = pushSendLabel(sending),
                onClick = if (sending || token.isEmpty()) {
                    null
                } else {
                    {
                        scope.launch {
                            sending = true
                            testStatus.clear()
                            val result = push.sendTest()
                            sending = false
                            testStatus.report(result.summary, result.delivered == 0)
                        }
                    }
                },
                icon = "send_outlined",
            )
        }
        FmInlineStatus(testStatus, Modifier.padding(top = FmSpace.x2))
    }
}

@Composable
private fun AccountNodesCard(
    session: AuthSession?,
    refreshed: List<AccountDevice>?,
    loading: Boolean,
    error: String,
    onRefresh: () -> Unit,
    webUrl: String,
) {
    val c = Fm.colors
    if (session == null || !hasRealSession(session)) {
        CardEmptyState(
            title = "Sign in to see your nodes",
            body = "Every desktop and mobile node bound to your account is listed here. Add or revoke nodes from the web app.",
        )
        return
    }
    val summary = nodesSummary(refreshed, session)
    Column(Modifier.fillMaxWidth()) {
        Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
            Text(summary.headline, modifier = Modifier.weight(1f), style = emphasisStyle())
            if (loading) {
                FmCircularProgressIndicator(Modifier.size(16.dp), strokeWidth = 2.dp)
            } else {
                FmIconButton(icon = "refresh", label = "Refresh nodes", onPressed = onRefresh, iconSize = 18.dp)
            }
        }
        if (error.isNotEmpty()) {
            VGap(FmSpace.x2)
            Text(error, style = Fm.text.bodySmall.copy(color = c.warning))
        }
        VGap(FmSpace.x2)
        if (summary.devices.isEmpty()) {
            Text(nodesEmptyText(loading), style = noteStyle())
        } else {
            if (summary.showsDesktopNote) {
                Text(
                    "Remote commands stay locked until a desktop approval flow is added. This inventory only shows " +
                        "which nodes could later run trusted work.",
                    style = noteStyle(),
                )
            }
            VGap(FmSpace.x3)
            summary.devices.forEachIndexed { index, device ->
                DeviceRow(device, isCurrentDevice(device, session))
                if (index != summary.devices.lastIndex) VGap(FmSpace.x2)
            }
        }
        VGap(FmSpace.x3)
        WebSettingsLink(webUrl)
    }
}

@Composable
private fun WebSettingsLink(url: String) {
    val c = Fm.colors
    val context = LocalContext.current
    Row(
        Modifier
            .fillMaxWidth()
            .background(Color.Transparent, RoundedCornerShape(FmRadius.sm))
            .clickable(onClickLabel = "Manage nodes on the web", role = Role.Button) { openExternalUrl(context, url) }
            .padding(vertical = FmSpace.x1),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        FmIcon("open_in_new", size = 14.dp, tint = c.textSecondary)
        HGap(FmSpace.x2)
        Text(
            "Manage and revoke nodes from the web app → Settings",
            modifier = Modifier.weight(1f),
            style = Fm.text.bodySmall.copy(fontWeight = FmType.emphasis, color = c.accent),
        )
    }
}

@Composable
private fun DeviceRow(device: AccountDevice, isCurrent: Boolean) {
    val c = Fm.colors
    val color = when (deviceTone(device)) {
        DeviceTone.Offline -> c.offline
        DeviceTone.Success -> c.success
        DeviceTone.Warning -> c.warning
    }
    Row(
        Modifier
            .fillMaxWidth()
            .background(c.bgBase, RoundedCornerShape(FmRadius.md))
            .padding(FmSpace.x3),
        verticalAlignment = Alignment.Top,
    ) {
        TintedTile(if (device.kind == "desktop_node") "desktop_mac_outlined" else "phone_iphone", color)
        HGap(FmSpace.x3)
        Column(Modifier.weight(1f)) {
            Text(deviceRowTitle(device, isCurrent), maxLines = 1, overflow = TextOverflow.Ellipsis, style = emphasisStyle())
            VGap(FmSpace.x1)
            FmStatusBadge(label = device.statusLabel, color = color)
            VGap(FmSpace.x1)
            Text(
                deviceRowDetail(device),
                maxLines = 2,
                overflow = TextOverflow.Ellipsis,
                style = Fm.text.bodySmall.copy(color = c.textSecondary),
            )
            deviceRowTimeline(device)?.let { timeline ->
                VGap(FmSpace.x1)
                Text(timeline, style = microStyle())
            }
            if (device.pubkey.isNotEmpty()) {
                VGap(FmSpace.x1)
                Text("Key: ${shortDeviceKey(device.pubkey)}", style = microStyle())
            }
        }
    }
}

@Composable
private fun RelayStateBadge(state: RelayConnection) {
    val c = Fm.colors
    when (state) {
        RelayConnection.Connected -> FmStatusBadge(label = "Connected", color = c.success)
        RelayConnection.Connecting -> FmStatusBadge(label = "Connecting...", color = c.warning)
        RelayConnection.Offline -> FmStatusBadge(label = "Offline", color = c.offline)
    }
}

@Composable
private fun SignOutCard(signedInAs: String, onSignOut: () -> Unit) {
    val c = Fm.colors
    Column(Modifier.fillMaxWidth()) {
        Text("Signed in as $signedInAs", maxLines = 1, overflow = TextOverflow.Ellipsis, style = emphasisStyle())
        VGap(FmSpace.x2)
        Text(
            "Sign out clears the saved session on this device. Your local identity key and relay settings stay " +
                "until you remove them.",
            style = noteStyle(),
        )
        VGap(FmSpace.x3)
        TextButton(
            onClick = onSignOut,
            modifier = Modifier.defaultMinSize(minHeight = 44.dp),
            shape = RoundedCornerShape(FmRadius.md),
            colors = ButtonDefaults.textButtonColors(
                containerColor = c.danger.copy(alpha = 22f / 255f),
                contentColor = c.danger,
            ),
            contentPadding = PaddingValues(horizontal = FmSpace.x4, vertical = FmSpace.x3),
        ) {
            FmIcon("logout", size = 18.dp)
            HGap(8.dp)
            Text("Sign out")
        }
    }
}

@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun GuestAccountCard(onLeave: () -> Unit) {
    val c = Fm.colors
    Column(
        Modifier
            .fillMaxWidth()
            .testTag("settings-guest-account"),
    ) {
        Text("You are exploring as a guest", style = emphasisStyle())
        VGap(FmSpace.x2)
        Text(
            "Browse repositories, issues, pull requests and chat in #$GUEST_CHAT_CHANNEL. Sign in to open issues, " +
                "comment, review, publish and manage organizations.",
            style = noteStyle(),
        )
        VGap(FmSpace.x3)
        FlowRow(
            horizontalArrangement = Arrangement.spacedBy(FmSpace.x3),
            verticalArrangement = Arrangement.spacedBy(FmSpace.x2),
        ) {
            AccountFilledButton(
                label = "Log in",
                icon = "login",
                onClick = { AccountFeature.openSignIn() },
                modifier = Modifier.testTag("settings-guest-log-in"),
            )
            AccountOutlinedButton(label = "Create an account", onClick = { AccountFeature.openSignIn(AuthFlowStart.Signup) })
            FmTextButton(onClick = onLeave, contentColor = c.textSecondary) { Text("Leave guest mode") }
        }
    }
}

@Composable
private fun DeleteAccountCard(onDelete: () -> Unit) {
    val c = Fm.colors
    Column(Modifier.fillMaxWidth()) {
        Text("Delete account", style = emphasisStyle())
        VGap(FmSpace.x2)
        Text(
            "Permanently delete your ForkMesh account and associated server data. You will need your password.",
            style = noteStyle(),
        )
        VGap(FmSpace.x3)
        AccountOutlinedButton(
            label = "Delete account",
            onClick = onDelete,
            modifier = Modifier.testTag("settings-delete-account"),
            icon = "delete_forever_outlined",
            contentColor = c.danger,
            borderColor = c.danger.copy(alpha = 120f / 255f),
            minHeight = 44.dp,
        )
    }
}

@Composable
private fun DeleteAccountDialog(auth: AuthService, done: (Boolean?) -> Unit) {
    val c = Fm.colors
    val scope = rememberCoroutineScope()
    var password by remember { mutableStateOf("") }
    var confirm by remember { mutableStateOf("") }
    var error by remember { mutableStateOf<String?>(null) }

    fun submit() {
        val refused = deleteAccountConfirmError(confirm)
        if (refused != null) {
            error = refused
            return
        }
        scope.launch {
            try {
                auth.deleteAccount(password)
                done(true)
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (failure: Exception) {
                error = deleteAccountFailure(failure)
            }
        }
    }

    FmAlertDialog(
        onDismissRequest = {},
        title = "Delete account?",
        containerColor = c.bgOverlay,
        shape = RoundedCornerShape(FmRadius.lg),
        actions = {
            FmTextButton(onClick = { done(false) }) { Text("Cancel") }
            Button(
                onClick = { submit() },
                colors = ButtonDefaults.buttonColors(containerColor = c.danger),
            ) { Text("Delete account") }
        },
        content = {
            val style = LocalTextStyle.current
            Text(
                "This permanently deletes your ForkMesh account on the server. This cannot be undone.",
                style = style.copy(color = c.textSecondary, lineHeight = style.fontSize * 1.4f),
            )
            VGap(FmSpace.x4)
            FmTextField(
                value = password,
                onValueChange = { password = it },
                hint = "Current password",
                keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Password, imeAction = ImeAction.Next),
                visualTransformation = PasswordVisualTransformation(),
            )
            VGap(FmSpace.x3)
            FmTextField(
                value = confirm,
                onValueChange = { confirm = it },
                hint = "Type DELETE to confirm",
                keyboardOptions = KeyboardOptions(imeAction = ImeAction.Done),
                keyboardActions = KeyboardActions(onDone = { submit() }),
            )
            error?.let { message ->
                VGap(FmSpace.x3)
                Text(message, style = Fm.text.bodyMedium.copy(color = c.danger))
            }
        },
    )
}

@Composable
private fun LegalLinkRow(label: String, onTap: () -> Unit) {
    Row(
        Modifier
            .fillMaxWidth()
            .clickable(onClickLabel = label, role = Role.Button, onClick = onTap)
            .padding(FmSpace.x4),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(label, modifier = Modifier.weight(1f), style = emphasisStyle())
        FmIcon("open_in_new", size = 16.dp, tint = Fm.colors.textTertiary)
    }
}

@Composable
private fun MembershipStatusRow(token: String) {
    val c = Fm.colors
    var status by remember(token) { mutableStateOf<com.forkmesh.nativeapp.settings.MembershipStatus?>(null) }
    LaunchedEffect(token) {
        val endpoints = SettingsArea.endpoints ?: return@LaunchedEffect
        status = kotlinx.coroutines.withContext(kotlinx.coroutines.Dispatchers.IO) {
            com.forkmesh.nativeapp.settings.AccountDataModel(endpoints).also { it.loadMembership() }.membership
        }
    }
    val current = status ?: return
    Row(
        Modifier.fillMaxWidth().padding(start = FmSpace.x4, top = FmSpace.x4, end = FmSpace.x4, bottom = FmSpace.x3).testTag("settings-membership-status"),
        horizontalArrangement = Arrangement.spacedBy(FmSpace.x3),
    ) {
        FmIcon(
            if (current.connected) "verified_outlined" else "favorite_outline_baseline",
            size = 22.dp,
            tint = if (current.connected) c.success else c.textTertiary,
        )
        Column(Modifier.fillMaxWidth()) {
            Text(current.title, style = Fm.text.titleSmall.copy(color = c.textPrimary))
            Text(current.detail, style = Fm.text.bodySmall.copy(color = c.textSecondary))
        }
    }
}
