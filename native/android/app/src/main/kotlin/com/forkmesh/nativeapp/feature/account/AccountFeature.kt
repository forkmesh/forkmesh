package com.forkmesh.nativeapp.feature.account

import androidx.compose.ui.Modifier
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import com.forkmesh.nativeapp.App
import com.forkmesh.nativeapp.account.AuthFlowStart
import com.forkmesh.nativeapp.account.gateCreatesPreviewSession
import com.forkmesh.nativeapp.account.guestChatName
import com.forkmesh.nativeapp.account.requireAccount
import com.forkmesh.nativeapp.account.showsUserStatus
import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.api.InboxService
import com.forkmesh.nativeapp.api.PrefStore
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.account.auth.AuthFlow
import com.forkmesh.nativeapp.feature.account.auth.AuthFlowScreen
import com.forkmesh.nativeapp.feature.account.notifications.NotificationsScreen
import com.forkmesh.nativeapp.feature.account.notifications.PingSettingsScreen
import com.forkmesh.nativeapp.feature.account.apps.InstalledAppsScreen
import com.forkmesh.nativeapp.feature.account.orgs.OrgsScreen
import com.forkmesh.nativeapp.feature.account.settings.SettingsScreen
import com.forkmesh.nativeapp.model.UserStatus
import com.forkmesh.nativeapp.platform.AppLifecycle
import com.forkmesh.nativeapp.platform.ProjectLifecycle
import com.forkmesh.nativeapp.account.projects.NextProjectFold
import com.forkmesh.nativeapp.account.projects.ProjectDirectory
import com.forkmesh.nativeapp.api.AccountSettings
import com.forkmesh.nativeapp.api.AuthController
import com.forkmesh.nativeapp.feature.account.projects.ChooseInstanceSheet
import com.forkmesh.nativeapp.feature.account.projects.FlutterDataMigration
import com.forkmesh.nativeapp.feature.account.projects.ProjectsService
import com.forkmesh.nativeapp.feature.push.PingCache
import com.forkmesh.nativeapp.platform.Prefs
import com.forkmesh.nativeapp.platform.SecureStore
import com.forkmesh.nativeapp.security.DeviceIdentity
import com.forkmesh.nativeapp.security.OwnerKeys
import com.forkmesh.nativeapp.security.SecretStore
import com.forkmesh.nativeapp.ui.kit.GuestModeBanner
import com.forkmesh.nativeapp.ui.kit.GuestSignInChoice
import com.forkmesh.nativeapp.ui.kit.LocalFmMessenger
import com.forkmesh.nativeapp.ui.kit.showGuestSignInSheet
import com.forkmesh.nativeapp.ui.navigation.AppNavigation
import com.forkmesh.nativeapp.ui.navigation.Screen
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

data class AccountShellState(
    val label: String = "",
    val unreadNotifications: Int = 0,
    val isGuest: Boolean = false,
    val userStatus: UserStatus = UserStatus(),
)

object AccountFeature {
    private val state = MutableStateFlow(AccountShellState(label = "Guest"))

    val shellState: StateFlow<AccountShellState> get() = state

    private class Globals(val secureStore: SecureStore, val rawPrefs: Prefs, val prefs: PrefsStore, val projects: ProjectsService)

    @Volatile
    private var globals: Globals? = null

    private fun globals(app: App): Globals {
        globals?.let { return it }
        val secureStore = SecureStore(app)
        val rawPrefs = Prefs(app)
        FlutterDataMigration.runBeforeLaunch(app, secureStore, rawPrefs)
        val prefs = PrefsStore(rawPrefs)
        AccountSettings.prepareForProjects(prefs, secureStore)
        runCatching { NextProjectFold.run(FoldPrefsStore(rawPrefs), secureStore) { secureStore.keys() } }
        val legacyInstall = FlutterDataMigration.isUpgraded(rawPrefs) ||
            secureStore.contains(AuthController.SESSION_KEY) ||
            secureStore.contains(AuthController.IDENTITY_SEED_KEY)
        val directory = ProjectDirectory.load(prefs, legacyInstall = legacyInstall)
        return Globals(secureStore, rawPrefs, prefs, ProjectsService(app, directory, secureStore, rawPrefs)).also { globals = it }
    }

    fun install(app: App) {
        val scope = ProjectLifecycle.scope
        val g = globals(app)
        val projects = Services.put(g.projects)
        val project = projects.directory.active
        val projectScope = project.scope
        Services.put(projectScope)
        val secureStore = g.secureStore
        val projectSecrets = projectScope.secrets(secureStore)
        Services.put<SecretStore>(projectSecrets)
        val prefs = g.prefs
        Services.put<PrefStore>(prefs)
        val settings = Services.put(
            SettingsService.forProject(
                prefs,
                projectScope,
                secureStore,
                serverUrl = { projects.directory.byId(project.id)?.serverUrl ?: project.serverUrl },
                useServerUrl = { url -> projects.useUrl(url) },
            ),
        )

        SecurityBootstrap.install(app, projectSecrets)

        val api = ApiService(settings.core)
        api.ownerKeysProvider = { Services.get<OwnerKeys>() }
        Services.put(api)

        val auth = Services.put(AuthService.create(settings, prefs, projectSecrets, project.id, secureStore))
        api.onRouteMissing = { path ->
            com.forkmesh.nativeapp.account.projects.RelayFeature.learnableFromPath(path)?.let { projects.directory.markFeatureMissing(project.id, it) }
        }
        AppLifecycle.scope.launch {
            withContext(Dispatchers.IO) {
                FlutterDataMigration.retireWhenVerified(app, secureStore, g.rawPrefs, api, auth.isAuthenticated)
            }
        }

        api.sessionTokenProvider = { auth.core.session?.sessionToken ?: "" }
        api.onSessionInvalid = { code -> scope.launch { auth.handleUnauthorized(code) } }
        scope.launch {
            auth.session.map { it?.sessionToken ?: "" }.distinctUntilChanged().collect {
                withContext(Dispatchers.IO) { api.syncOwnerEnrollment() }
            }
        }

        Services.put(InboxService(settings.core, identity = { Services.get<DeviceIdentity>() }))

        val notifications = Services.put(
            NotificationService(
                api,
                nodeName = { auth.core.session?.nodeName ?: "" },
                scope = scope,
                snapshots = PingCache.get(app),
                project = project.id,
            ),
        )

        val accountEvents = Services.put(
            AccountEventChannel(api, sessionToken = { auth.core.session?.sessionToken ?: "" }, scope = scope),
        )
        accountEvents.onTopic = { topic ->
            if (topic == "pings" || topic == "issues") notifications.refreshInBackground(silent = true)
        }
        accountEvents.onConnected = { notifications.refreshInBackground(silent = true) }

        ProjectLifecycle.onClose {
            withContext(Dispatchers.IO) {
                accountEvents.stop()
                accountEvents.dispose()
            }
        }

        val push = Services.put(PushNotificationService(api))
        push.accountKeyProvider = { auth.core.session?.nodeName ?: "" }
        push.onOpened = { notifications.refreshInBackground(silent = true) }

        scope.launch {
            var lastToken = ""
            auth.session.collect { session ->
                val token = session?.sessionToken ?: ""
                if (token == lastToken) return@collect
                lastToken = token
                withContext(Dispatchers.IO) {
                    if (token.isEmpty()) {
                        accountEvents.stop()
                        push.binder.stop()
                    } else {
                        accountEvents.restart()
                        push.binder.rebind(session?.nodeName ?: "")
                        auth.refreshUserStatus(api)
                        auth.refreshChatRetention(api)
                    }
                }
            }
        }
        scope.launch {
            AppLifecycle.foreground.collect { foreground ->
                val account = auth.core.session?.nodeName ?: ""
                if (foreground && account.isNotEmpty()) {
                    push.rebind(account)
                    auth.refreshChatRetention(api)
                }
            }
        }

        scope.launch { auth.session.collect { notifications.publish() } }

        scope.launch {
            combine(auth.session, auth.isGuest, notifications.unreadCount, auth.userStatus) { session, guest, unread, status ->
                val accountName = if (session?.nodeName?.isNotEmpty() == true) session.nodeName else session?.email ?: ""
                val label = accountName.ifEmpty {
                    guestChatName(Services.get<DeviceIdentity>()?.publicKeyB64url ?: "")
                }
                AccountShellState(
                    label = label,
                    unreadNotifications = unread,
                    isGuest = guest,
                    userStatus = if (showsUserStatus(session, guest)) status else UserStatus(),
                )
            }.collect { state.value = it }
        }
    }

    @Composable
    fun rememberRequireAccount(): suspend (action: String) -> Boolean {
        val messenger = LocalFmMessenger.current
        return remember<suspend (String) -> Boolean>(messenger) {
            { action: String ->
                val auth = Services.get<AuthService>()
                if (ChooseInstanceSheet.needsInstance()) {
                    ChooseInstanceSheet.open()
                    false
                } else {
                    requireAccount(
                        guest = com.forkmesh.nativeapp.account.isGuestMode(
                            authAvailable = auth != null,
                            isGuest = auth?.isGuest?.value == true,
                        ),
                        showSheet = {
                            when (messenger.showGuestSignInSheet(action)) {
                                GuestSignInChoice.LogIn -> AuthFlowStart.Login
                                GuestSignInChoice.SignUp -> AuthFlowStart.Signup
                                null -> null
                            }
                        },
                        openSignIn = { start -> signIn(start) },
                        isAuthenticated = { auth?.isAuthenticated == true },
                    )
                }
            }
        }
    }

    @Composable
    fun isGuestMode(): Boolean {
        val auth = Services.get<AuthService>() ?: return false
        val guest by auth.isGuest.collectAsState()
        return com.forkmesh.nativeapp.account.isGuestMode(authAvailable = true, isGuest = guest)
    }

    fun openSignIn(start: AuthFlowStart = AuthFlowStart.Login, onClosed: () -> Unit = {}) {
        if (ChooseInstanceSheet.needsInstance()) {
            ChooseInstanceSheet.open()
            onClosed()
            return
        }
        AppNavigation.navigator.push(AuthFlowScreen(start, onClosed))
    }

    suspend fun signIn(start: AuthFlowStart = AuthFlowStart.Login) {
        val closed = CompletableDeferred<Unit>()
        openSignIn(start) { closed.complete(Unit) }
        closed.await()
    }

    @Composable
    fun GuestBanner() {
        if (!isGuestMode()) return
        GuestModeBanner(onSignIn = { openSignIn() })
    }

    @Composable
    fun Gate(content: @Composable () -> Unit) {
        val generation = Services.get<ProjectsService>()?.generation?.collectAsState()?.value ?: 0
        androidx.compose.foundation.layout.Box(androidx.compose.ui.Modifier.fillMaxSize()) {
            androidx.compose.runtime.key(generation) { GateBody(content) }
            com.forkmesh.nativeapp.feature.account.projects.ProjectDrawerHost()
            com.forkmesh.nativeapp.feature.account.projects.ChooseInstanceHost()
            com.forkmesh.nativeapp.feature.account.projects.ProjectSwitchOverlay()
        }
    }

    @Composable
    fun ProjectTitle(icon: Boolean = false, subtitle: String? = null, onOpen: () -> Unit = {}) {
        com.forkmesh.nativeapp.feature.account.projects.ProjectSwitcherTitle(icon = icon, subtitle = subtitle, onOpen = onOpen)
    }

    @Composable
    fun ProjectSlot(modifier: Modifier = Modifier) {
        com.forkmesh.nativeapp.feature.account.projects.ProjectSwitcherTitle(modifier = modifier, iconOnly = true)
    }

    @Composable
    private fun GateBody(content: @Composable () -> Unit) {
        val projects = Services.get<ProjectsService>()
        val active = projects?.active?.collectAsState()?.value
        if (projects != null && active != null && !active.hasInstance) {
            content()
            return
        }
        val auth = Services.get<AuthService>()
        if (auth == null) {
            content()
            return
        }
        val hasAccess by auth.hasAppAccess.collectAsState()
        if (hasAccess) {
            content()
        } else {
            AuthFlow(
                onContinueAsGuest = { auth.enterGuestMode() },
                onAuthenticated = {
                    if (gateCreatesPreviewSession(auth.isAuthenticated)) {
                        AppLifecycle.scope.launch { auth.authenticatePreview() }
                    }
                },
            )
        }
    }

    fun settingsTab(): Screen = SettingsScreen()

    fun notificationsScreen(): Screen = NotificationsScreen()

    fun pingSettingsScreen(initialKind: String? = null): Screen = PingSettingsScreen(initialKind)

    fun orgsScreen(): Screen = OrgsScreen()

    fun installedAppsScreen(): Screen = InstalledAppsScreen()

    fun startNotifications() {
        Services.get<NotificationService>()?.start()
    }
}
