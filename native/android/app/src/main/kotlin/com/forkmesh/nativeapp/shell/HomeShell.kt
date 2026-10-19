package com.forkmesh.nativeapp.shell

import androidx.compose.foundation.border
import androidx.compose.animation.AnimatedContent
import androidx.compose.animation.core.Animatable
import androidx.compose.animation.core.Spring
import androidx.compose.animation.core.spring
import androidx.compose.animation.slideInVertically
import androidx.compose.animation.slideOutVertically
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.layout.boundsInRoot
import androidx.compose.ui.layout.onGloballyPositioned
import androidx.compose.animation.core.tween
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.slideInHorizontally
import androidx.compose.animation.slideOutHorizontally
import androidx.compose.animation.togetherWith
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.consumeWindowInsets
import androidx.compose.foundation.layout.windowInsetsTopHeight
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.ime
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.navigationBars
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.statusBars
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Badge
import androidx.compose.material3.BadgedBox
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.NavigationRail
import androidx.compose.material3.NavigationRailItem
import androidx.compose.material3.NavigationRailItemDefaults
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.saveable.SaveableStateHolder
import androidx.compose.runtime.saveable.rememberSaveableStateHolder
import androidx.compose.runtime.setValue
import androidx.compose.runtime.snapshotFlow
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.scale
import androidx.compose.ui.input.key.Key
import androidx.compose.ui.input.key.KeyEventType
import androidx.compose.ui.input.key.isCtrlPressed
import androidx.compose.ui.input.key.isMetaPressed
import androidx.compose.ui.input.key.key
import androidx.compose.ui.input.key.onPreviewKeyEvent
import androidx.compose.ui.input.key.type
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.LocalUriHandler
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.role
import androidx.compose.ui.semantics.selected
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.feature.navigation.ShellMenuGrid
import com.forkmesh.nativeapp.ui.kit.fmGlass
import com.forkmesh.nativeapp.ui.kit.FmBackdropHost
import com.forkmesh.nativeapp.ui.kit.LocalShellClearance
import androidx.compose.ui.unit.Dp
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.interaction.collectIsPressedAsState
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.ui.unit.sp
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.account.AccountFeature
import com.forkmesh.nativeapp.feature.account.AuthService
import com.forkmesh.nativeapp.feature.account.projects.NoInstanceBanner
import com.forkmesh.nativeapp.feature.account.projects.ProjectsService
import com.forkmesh.nativeapp.feature.account.settings.SettingsArea
import com.forkmesh.nativeapp.feature.account.settings.rememberNowMillis
import com.forkmesh.nativeapp.feature.agents.AgentsFeature
import com.forkmesh.nativeapp.feature.agents.RunnerAttentionChip
import com.forkmesh.nativeapp.feature.agents.RunnerAttentionFeature
import com.forkmesh.nativeapp.feature.chat.ChatFeature
import com.forkmesh.nativeapp.feature.chat.RelayConnection
import com.forkmesh.nativeapp.feature.code.CodeFeature
import com.forkmesh.nativeapp.feature.composer.PromptButton
import com.forkmesh.nativeapp.feature.composer.ShellPrompt
import com.forkmesh.nativeapp.feature.glasses.GlassesRadarChip
import com.forkmesh.nativeapp.feature.inbox.InboxFeature
import com.forkmesh.nativeapp.feature.insights.StatusPageFeature
import com.forkmesh.nativeapp.feature.market.PackageGateFeature
import com.forkmesh.nativeapp.market.PackageSurfaces
import com.forkmesh.nativeapp.feature.navigation.CommandPaletteSheet
import com.forkmesh.nativeapp.feature.navigation.DesktopOfflineDot
import com.forkmesh.nativeapp.feature.navigation.NavigationFeature
import com.forkmesh.nativeapp.feature.navigation.ShellFirstRun
import com.forkmesh.nativeapp.feature.navigation.ShellMenuBadge
import com.forkmesh.nativeapp.feature.navigation.ShellMenuEntry
import com.forkmesh.nativeapp.feature.navigation.ShellMenuSheet
import com.forkmesh.nativeapp.feature.navigation.rememberDesktopOffline
import com.forkmesh.nativeapp.feature.navigation.UpdateRequiredView
import com.forkmesh.nativeapp.feature.navigation.YouHubSheet
import com.forkmesh.nativeapp.feature.planning.PlanningFeature
import com.forkmesh.nativeapp.feature.push.PushFeature
import com.forkmesh.nativeapp.feature.push.PushScreens
import com.forkmesh.nativeapp.push.PushScreen
import com.forkmesh.nativeapp.feature.surfaces.RunningAgentAccessory
import com.forkmesh.nativeapp.feature.surfaces.SurfacesFeature
import com.forkmesh.nativeapp.feature.tools.ToolsFeature
import com.forkmesh.nativeapp.navigation.AppTab
import com.forkmesh.nativeapp.navigation.CodeBarItem
import com.forkmesh.nativeapp.navigation.CodeButton
import com.forkmesh.nativeapp.navigation.CodeButtonPlan
import com.forkmesh.nativeapp.navigation.RepoRef
import com.forkmesh.nativeapp.navigation.ComposeTarget
import com.forkmesh.nativeapp.navigation.Route
import com.forkmesh.nativeapp.navigation.SearchScope
import com.forkmesh.nativeapp.navigation.barOrder
import com.forkmesh.nativeapp.navigation.icon
import com.forkmesh.nativeapp.navigation.title
import com.forkmesh.nativeapp.ui.SoundCue
import com.forkmesh.nativeapp.ui.kit.FmInlineStatus
import com.forkmesh.nativeapp.ui.kit.FmInlineStatusLine
import com.forkmesh.nativeapp.ui.kit.FmStatus
import com.forkmesh.nativeapp.ui.kit.FmSoundCues
import com.forkmesh.nativeapp.ui.navigation.LocalNavigator
import com.forkmesh.nativeapp.ui.navigation.LocalScreen
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmMotion
import com.forkmesh.nativeapp.ui.theme.FmRadius
import com.forkmesh.nativeapp.ui.theme.FmSpace
import com.forkmesh.nativeapp.ui.theme.FmType
import java.util.WeakHashMap

class HomeShell : Screen() {
    private val roots: Map<AppTab, Screen> by lazy {
        mapOf(
            AppTab.Agents to AgentsFeature.tab(),
            AppTab.Inbox to InboxFeature.tab(),
            AppTab.Code to CodeFeature.tab(),
            AppTab.Chat to ChatFeature.tab(),
        )
    }
    private val navigators: Map<AppTab, Navigator> by lazy { roots.mapValues { Navigator(it.value) } }
    private val routeOf = WeakHashMap<Screen, Route>()
    private var selectedTab by mutableStateOf(homeTab(PackageGateFeature.surfaces.value))
    private var homeSelected by mutableStateOf(true)
    private var promptOpen by mutableStateOf(false)
    private var hubOpen by mutableStateOf(false)
    private var paletteOpen by mutableStateOf(false)
    private var pendingPrompt: ComposeTarget? = null

    val selected: AppTab get() = selectedTab

    override val title: String get() = "ForkMesh"

    fun navigator(tab: AppTab): Navigator = navigators.getValue(tab)

    fun routes(tab: AppTab): List<Route> = navigator(tab).stack.drop(1).map { routeOf[it] ?: return@map null }.takeWhile { it != null }.filterNotNull()

    private fun pushScreen(root: Navigator): PushScreen {
        val nav = navigator(selectedTab)
        val routes = routes(selectedTab)
        val covered = homeSelected || promptOpen || hubOpen || paletteOpen || root.top !== this || routes.size < nav.depth - 1
        return PushScreen(selectedTab, routes.lastOrNull(), covered)
    }

    fun push(tab: AppTab, route: Route, screen: Screen) {
        routeOf[screen] = route
        homeSelected = false
        navigator(tab).push(screen)
    }

    fun selectHome() {
        if (!homeSelected) FmSoundCues.play(SoundCue.NAVIGATE)
        homeSelected = true
    }

    private fun goHome() {
        paletteOpen = false
        hubOpen = false
        selectHome()
    }

    fun select(tab: AppTab, reselect: Boolean = true) {
        val wasHome = homeSelected
        homeSelected = false
        if (reselect) {
            val nav = navigator(tab)
            if (tab == selectedTab && !wasHome) {
                if (nav.depth > 1) FmSoundCues.play(SoundCue.CLOSE)
                nav.popToRoot()
                return
            }
            nav.popToRoot()
        }
        if (tab == AppTab.Chat) ChatFeature.markBadgeSeen()
        if (tab != selectedTab || wasHome) FmSoundCues.play(SoundCue.NAVIGATE)
        selectedTab = tab
    }

    fun openCurrentRepo(): Boolean {
        val nav = navigator(AppTab.Code)
        val repo = CodeFeature.currentRepository()
        val plan = CodeButton.plan(repo?.let { RepoRef(it.owner, it.name) }, nav.stack.drop(1).map(CodeFeature::repoOf))
        when (plan) {
            CodeButtonPlan.NoRepo -> {
                select(AppTab.Code)
                return false
            }
            CodeButtonPlan.PopToRepo -> nav.popTo(nav.stack[1])
            CodeButtonPlan.Reset -> {
                nav.popToRoot()
                repo?.let { nav.push(CodeFeature.repoDetailScreen(it)) }
            }
        }
        select(AppTab.Code, reselect = false)
        return true
    }

    fun showTab(label: String) {
        AppTab.entries.firstOrNull { it.title.equals(label, ignoreCase = true) }?.let { select(it, reselect = false) }
    }

    fun openPrompt(target: ComposeTarget? = null) {
        pendingPrompt = target
        target?.repo?.let { com.forkmesh.nativeapp.feature.composer.ComposerFeature.followRepository(it.slug) }
        target?.text?.let { text -> com.forkmesh.nativeapp.feature.composer.ComposerFeature.drafts?.save(com.forkmesh.nativeapp.composer.PromptDraftStore.SHELL, text) }
        promptOpen = true
    }

    fun openHub() {
        hubOpen = true
    }

    fun openPalette() {
        paletteOpen = true
    }

    override fun onBack(): Boolean {
        if (paletteOpen) {
            paletteOpen = false
            return true
        }
        if (hubOpen) {
            hubOpen = false
            return true
        }
        if (homeSelected) return false
        val nav = navigator(selectedTab)
        if (nav.depth > 1) return nav.handleBack()
        if (nav.top.onBack()) return true
        homeSelected = true
        return true
    }

    override fun onDisposed() {
        navigators.values.forEach { nav -> nav.stack.toList().asReversed().forEach { it.onDisposed() } }
    }

    private fun homeTab(surfaces: PackageSurfaces): AppTab = if (surfaces.showsTab(AppTab.Agents)) AppTab.Agents else AppTab.Code

    private fun selectShown(tab: AppTab, surfaces: PackageSurfaces): Boolean {
        if (!surfaces.showsTab(tab)) return false
        select(tab, false)
        return true
    }

    private fun barIcon(tab: AppTab, selected: Boolean): String = when (tab) {
        AppTab.Agents -> "auto_awesome"
        AppTab.Inbox -> if (selected) "inbox" else "inbox_outlined"
        AppTab.Code -> "code"
        AppTab.Chat -> if (selected) "forum" else "forum_outlined"
    }

    @Composable
    private fun hubBadges(activeAgents: Int): Map<Route.Kind, ShellMenuBadge> {
        val timers by PlanningFeature.timers.collectAsState()
        val runnerAttention by RunnerAttentionFeature.items.collectAsState()
        return mapOf(
            Route.Kind.AllAgents to ShellMenuBadge(activeAgents, running = true),
            Route.Kind.Timers to ShellMenuBadge(timers.count { it.isRunning }, running = true),
            Route.Kind.Hosts to ShellMenuBadge(runnerAttention.size),
        )
    }

    private fun homeEntries(surfaces: PackageSurfaces, agentsOn: Boolean, hideBarIcons: Boolean): List<ShellMenuEntry> {
        val tabs = if (hideBarIcons) emptyList() else AppTab.barOrder.filter(surfaces::showsTab).flatMap { tab ->
            if (tab == AppTab.Code) {
                listOfNotNull(
                    ShellMenuEntry("repos", CodeButton.REPOS_LABEL, CodeButton.REPOS_ICON) { select(AppTab.Code) },
                    if (surfaces.code) ShellMenuEntry("code", CodeButton.CODE_LABEL, "code") { openCurrentRepo() } else null,
                )
            } else {
                listOf(ShellMenuEntry(tab.raw, tab.title, barIcon(tab, false)) { select(tab) })
            }
        }
        return tabs + listOfNotNull(
            ShellMenuEntry("search", "Search", "search") { NavigationFeature.open(Route.Search("", SearchScope.Everything)) },
            ShellMenuEntry("palette", "Commands", "keyboard_command_key") { paletteOpen = true },
            if (agentsOn) ShellMenuEntry("prompt", "Prompt", "auto_awesome") { openPrompt() } else null,
        )
    }

    private fun promptLabel(): String = when (selectedTab) {
        AppTab.Chat -> "Chat"
        AppTab.Agents -> "Agents"
        AppTab.Inbox -> "Inbox"
        AppTab.Code -> "Code"
    }

    @Composable
    override fun Content(navigator: Navigator) {
        val relay by ChatFeature.shellState.collectAsState()
        val account by AccountFeature.shellState.collectAsState()
        val noInstance = Services.get<ProjectsService>()?.active?.collectAsState()?.value?.hasInstance == false
        val inboxUnread by InboxFeature.unread.collectAsState()
        val needsYou by SurfacesFeature.needsYou.collectAsState()
        val agentsSurface by SurfacesFeature.agents.collectAsState()
        val update by NavigationFeature.updateRequired.collectAsState()
        val shellStatus = remember { FmStatus() }
        val linkProblem by NavigationFeature.linkProblem.collectAsState()
        LaunchedEffect(selectedTab) { shellStatus.clear() }
        val wide = LocalConfiguration.current.screenWidthDp >= 760
        val keyboardUp = WindowInsets.ime.getBottom(LocalDensity.current) > 0
        val holder = rememberSaveableStateHolder()
        val surfaces by PackageGateFeature.surfaces.collectAsState()
        var lastSurfaces by remember { mutableStateOf(surfaces) }
        LaunchedEffect(surfaces) {
            if (lastSurfaces.showsTab(selectedTab) && !surfaces.showsTab(selectedTab)) selectedTab = homeTab(surfaces)
            lastSurfaces = surfaces
        }
        val agentsOn = surfaces.agents
        val hideBarIcons by SettingsArea.homeHidesBarIcons.collectAsState()
        val guest = Services.get<AuthService>()?.isGuest?.collectAsState()?.value == true
        val shellScope = rememberCoroutineScope()
        LaunchedEffect(Unit) {
            AccountFeature.startNotifications()
            Services.get<AuthService>()?.refreshAvatar()
        }
        LaunchedEffect(navigator) {
            snapshotFlow { pushScreen(navigator) }.collect { PushScreens.publish(it) }
        }
        DisposableEffect(Unit) { onDispose { PushScreens.publish(null) } }
        PushFeature.PermissionPrompt()
        RunnerAttentionFeature.Watcher()
        val uriHandler = LocalUriHandler.current
        ShellFirstRun(navigator)

        fun badge(tab: AppTab): Int = when (tab) {
            AppTab.Agents -> if (needsYou > 0) needsYou else LaunchFlight.badge(agentsSurface.active.size)
            AppTab.Inbox -> if (inboxUnread >= 0) inboxUnread else account.unreadNotifications
            AppTab.Code -> 0
            AppTab.Chat -> relay.unreadCount
        }

        val tabNav = navigator(selectedTab)
        val atRoot = homeSelected || tabNav.depth <= 1
        val shellChromeHidden = !homeSelected && tabNav.stack.any { it.hidesShellChrome }
        val codeItem = CodeButton.selected(selectedTab == AppTab.Code, navigator(AppTab.Code).depth)
        val onHome = homeSelected
        val homeItem = BarItem("Home", "home", 0, onHome, "new") { selectHome() }
        val barItems = listOf(homeItem) + AppTab.barOrder.filter(surfaces::showsTab).flatMap { tab ->
            if (tab != AppTab.Code) {
                listOf(BarItem(tab.title, barIcon(tab, tab == selectedTab && !onHome), badge(tab), tab == selectedTab && !onHome, "new", agents = tab == AppTab.Agents) { select(tab) })
            } else {
                listOfNotNull(
                    BarItem(CodeButton.REPOS_LABEL, CodeButton.REPOS_ICON, 0, !onHome && codeItem == CodeBarItem.Repos, "repositories") { select(AppTab.Code) },
                    if (!surfaces.code) {
                        null
                    } else {
                        BarItem(CodeButton.CODE_LABEL, "code", 0, !onHome && codeItem == CodeBarItem.Code, "new") {
                            if (openCurrentRepo()) shellStatus.clear() else shellStatus.show(CodeButton.NO_REPO_NOTICE)
                        }
                    },
                )
            }
        }
        val body: @Composable () -> Unit = {
            Column(Modifier.fillMaxSize()) {
                val chatRoot = !homeSelected && selectedTab == AppTab.Chat && tabNav.depth <= 1
                if (!shellChromeHidden && atRoot && !chatRoot) {
                    TopBar(
                        navigator = tabNav,
                        connection = relay.connection,
                        name = account.label,
                        statusEmoji = account.userStatus.displayEmoji(rememberNowMillis()),
                        away = account.userStatus.away,
                        guest = guest,
                        showsSignIn = surfaces.signIn,
                        onAvatar = { hubOpen = true },
                        onSignIn = { AccountFeature.openSignIn() },
                        onHome = { goHome() },
                        prompt = {
                            if (agentsOn) PromptButton(promptLabel(), onClick = { openPrompt() }, onLongClick = { paletteOpen = true }, running = agentsSurface.activeCount)
                        },
                    )
                } else if (!shellChromeHidden) {
                    Spacer(Modifier.fillMaxWidth().windowInsetsTopHeight(WindowInsets.statusBars))
                }
                if (!shellChromeHidden && noInstance) NoInstanceBanner() else if (!shellChromeHidden) ConnectionBanner(relay.connection)
                if (!shellChromeHidden) FmInlineStatusLine(
                    linkProblem,
                    isError = true,
                    modifier = Modifier.background(Fm.colors.bgRaised).padding(horizontal = FmSpace.x4),
                    onDismiss = NavigationFeature::clearLinkProblem,
                )
                if (!shellChromeHidden && wide) FmInlineStatus(shellStatus, Modifier.background(Fm.colors.bgRaised).padding(horizontal = FmSpace.x4))
                if (!shellChromeHidden && atRoot && (homeSelected || selectedTab != AppTab.Agents) && agentsOn) {
                    RunnerAttentionChip { items ->
                        if (items.size == 1) {
                            RunnerAttentionFeature.open(tabNav, items[0]) { url -> runCatching { uriHandler.openUri(url) } }
                        } else {
                            select(AppTab.Agents, reselect = false)
                        }
                    }
                }
                Box(Modifier.weight(1f).fillMaxWidth().consumeWindowInsets(WindowInsets.statusBars)) {
                    ToolsFeature.BugReportOverlay(if (homeSelected) "home" else selectedTab.raw) {
                        if (homeSelected) {
                            HomePage(homeEntries(surfaces, agentsOn, hideBarIcons), hubBadges(agentsSurface.activeCount), shellScope)
                        } else {
                            TabStack(tabNav, holder, if (wide || keyboardUp || shellChromeHidden) 0.dp else ShellBar.clearance())
                        }
                    }
                    ShellPrompt(
                        tabNav,
                        promptOpen,
                        onStarted = {
                            navigator(AppTab.Agents).popToRoot()
                            select(AppTab.Agents, reselect = false)
                        },
                        openRun = { navigator(AppTab.Agents).push(it) },
                        notice = shellStatus,
                    ) { open ->
                        promptOpen = open
                        if (!open) pendingPrompt = null
                    }
                }
            }
        }

        Surface(
            color = Fm.colors.bgBase,
            modifier = Modifier.fillMaxSize().onPreviewKeyEvent { event ->
                if (event.type != KeyEventType.KeyDown || !(event.isCtrlPressed || event.isMetaPressed)) return@onPreviewKeyEvent false
                when (event.key) {
                    Key.K -> { paletteOpen = true; true }
                    Key.F -> { NavigationFeature.open(Route.Search("", SearchScope.Everything)); true }
                    Key.N -> { if (agentsOn) openPrompt(); agentsOn }
                    Key.One -> selectShown(AppTab.Agents, surfaces)
                    Key.Two -> selectShown(AppTab.Inbox, surfaces)
                    Key.Three -> selectShown(AppTab.Code, surfaces)
                    Key.Four -> selectShown(AppTab.Chat, surfaces)
                    else -> false
                }
            },
        ) {
            val desktopOffline = rememberDesktopOffline()
            if (wide) {
                Row(Modifier.fillMaxSize()) {
                    NavigationRail(
                        containerColor = Fm.colors.bgRaised,
                        modifier = Modifier.windowInsetsPadding(WindowInsets.statusBars),
                        header = { if (agentsOn) PromptButton(promptLabel(), onClick = { openPrompt() }) },
                    ) {
                        barItems.forEach { item ->
                            val isSelected = item.selected
                            NavigationRailItem(
                                selected = isSelected,
                                onClick = item.onClick,
                                icon = {
                                    BadgeIcon(
                                        item.icon, item.badge, if (isSelected) Fm.colors.textPrimary else Fm.colors.textTertiary, 24,
                                        badgeColor = if (item.agents && needsYou == 0) Fm.colors.accent else Fm.colors.danger,
                                        pulse = if (item.agents) LaunchFlight.landings else 0,
                                        modifier = if (item.agents) Modifier.launchTarget() else Modifier,
                                        desktopOffline = item.agents && desktopOffline,
                                    )
                                },
                                label = { Text(item.title) },
                                colors = NavigationRailItemDefaults.colors(
                                    indicatorColor = Fm.colors.bgRaised,
                                    selectedTextColor = Fm.colors.textPrimary,
                                    unselectedTextColor = Fm.colors.textTertiary,
                                ),
                            )
                        }
                        Spacer(Modifier.weight(1f))
                        StatusPageFeature.Dots(
                            onOpen = { NavigationFeature.openFromHub(Route.Status) },
                            modifier = Modifier.width(80.dp).windowInsetsPadding(WindowInsets.navigationBars),
                        )
                    }
                    Box(Modifier.weight(1f)) { body() }
                }
            } else {
                FmBackdropHost(
                    modifier = Modifier.fillMaxSize(),
                    overlay = {
                        if (!keyboardUp && !shellChromeHidden) {
                            Column(Modifier.align(Alignment.BottomCenter).fillMaxWidth()) {
                                FmInlineStatus(shellStatus, Modifier.background(Fm.colors.bgRaised).padding(horizontal = FmSpace.x4))
                                StatusPageFeature.Dots(onOpen = { NavigationFeature.openFromHub(Route.Status) })
                                if (agentsOn) RunningAgentAccessory()
                                GlassTabBar(barItems, agentsRunning = needsYou == 0)
                            }
                        }
                    },
                ) {
                    Box(
                        Modifier
                            .fillMaxSize()
                            .then(if (keyboardUp) Modifier else Modifier.consumeWindowInsets(WindowInsets.navigationBars)),
                    ) {
                        body()
                    }
                }
            }
            LaunchSparkles()
            update?.let { UpdateRequiredView(it) }
        }

        if (hubOpen) YouHubSheet(onClose = { hubOpen = false }, scope = shellScope)
        if (paletteOpen) {
            CommandPaletteSheet(
                tab = selectedTab,
                path = routes(selectedTab),
                onClose = { paletteOpen = false },
                onCompose = { openPrompt(it) },
                onSelectTab = { select(it, reselect = false) },
            )
        }
    }
}

@Composable
private fun TabStack(navigator: Navigator, holder: SaveableStateHolder, clearance: Dp) {
    DisposableEffect(navigator) {
        navigator.onRemoved = { holder.removeState(it.key) }
        onDispose { navigator.onRemoved = {} }
    }
    var lastDepth by remember(navigator) { mutableStateOf(navigator.depth) }
    AnimatedContent(
        targetState = navigator.top,
        transitionSpec = {
            val forward = navigator.depth >= lastDepth
            lastDepth = navigator.depth
            val duration = FmMotion.normal
            if (forward) {
                (slideInHorizontally(tween(duration)) { it / 4 } + fadeIn(tween(duration))) togetherWith fadeOut(tween(duration))
            } else {
                fadeIn(tween(duration)) togetherWith (slideOutHorizontally(tween(duration)) { it / 4 } + fadeOut(tween(duration)))
            }
        },
        label = "tab-stack",
    ) { screen ->
        holder.SaveableStateProvider(screen.key) {
            CompositionLocalProvider(LocalNavigator provides navigator, LocalScreen provides screen, LocalShellClearance provides clearance) {
                Box(Modifier.fillMaxSize().padding(bottom = if (screen.extendsUnderTabBar) 0.dp else clearance)) {
                    screen.Content(navigator)
                }
            }
        }
    }
}

@Composable
private fun TopBar(
    navigator: Navigator,
    connection: RelayConnection,
    name: String,
    statusEmoji: String,
    away: Boolean,
    guest: Boolean,
    showsSignIn: Boolean,
    onAvatar: () -> Unit,
    onSignIn: () -> Unit,
    onHome: () -> Unit,
    prompt: @Composable () -> Unit,
) {
    Box(
        Modifier
            .fillMaxWidth()
            .windowInsetsPadding(WindowInsets.statusBars)
            .padding(horizontal = FmSpace.x4, vertical = FmSpace.x1),
    ) {
        ToolsFeature.TelemetryStripCompact(navigator, Modifier.align(Alignment.Center).padding(horizontal = 64.dp))
        Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
            Box(Modifier.size(44.dp), contentAlignment = Alignment.Center) { AccountFeature.ProjectSlot(onLongPress = onHome) }
            Spacer(Modifier.width(FmSpace.x3))
            if (PackageGateFeature.surfaces.collectAsState().value.shows("glasses-alert")) GlassesRadarChip()
            Spacer(Modifier.weight(1f))
            prompt()
            if (guest) {
                Box(
                    Modifier
                        .size(44.dp)
                        .clip(CircleShape)
                        .clickable(onClick = onAvatar)
                        .semantics {
                            role = Role.Button
                            contentDescription = "You: projects, tools and settings"
                        },
                    contentAlignment = Alignment.Center,
                ) {
                    FmIcon("account_circle", size = 28.dp, tint = Fm.colors.textPrimary)
                }
                if (showsSignIn) SignInChip(onSignIn)
            } else {
                Spacer(Modifier.width(FmSpace.x2))
                AvatarWithDot(name, connection, statusEmoji, away, onAvatar)
            }
        }
    }
}

@Composable
private fun SignInChip(onClick: () -> Unit) {
    val c = Fm.colors
    Row(
        Modifier
            .height(36.dp)
            .clip(RoundedCornerShape(18.dp))
            .border(1.dp, c.border, RoundedCornerShape(18.dp))
            .clickable(onClick = onClick)
            .semantics { role = Role.Button }
            .padding(horizontal = FmSpace.x3),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        FmIcon("person_outline", size = 18.dp, tint = c.textPrimary)
        Spacer(Modifier.width(FmSpace.x2))
        Text("Sign in", style = Fm.text.labelLarge, color = c.textPrimary)
    }
}

@Composable
private fun AvatarWithDot(name: String, connection: RelayConnection, statusEmoji: String, away: Boolean, onClick: () -> Unit) {
    val dot = when (connection) {
        RelayConnection.Connected -> if (away) Fm.colors.warning else Fm.colors.success
        RelayConnection.Connecting -> Fm.colors.warning
        RelayConnection.Offline -> Fm.colors.offline
    }
    if (statusEmoji.isNotEmpty()) {
        Text(statusEmoji, fontSize = 17.sp, lineHeight = 17.sp, maxLines = 1)
        Spacer(Modifier.width(FmSpace.x1))
    }
    Box {
        Box(
            Modifier
                .size(36.dp)
                .clip(RoundedCornerShape(FmRadius.md))
                .clickable(onClick = onClick)
                .semantics {
                    role = Role.Button
                    contentDescription = "You, settings and more"
                }
                .background(Fm.colors.bgOverlay),
            contentAlignment = Alignment.Center,
        ) {
            Text(name.firstOrNull()?.uppercase() ?: "?", style = MaterialTheme.typography.titleSmall, color = Fm.colors.textPrimary)
        }
        Box(
            Modifier
                .align(Alignment.BottomEnd)
                .size(11.dp)
                .clip(CircleShape)
                .background(dot),
        )
    }
}

@Composable
private fun ConnectionBanner(connection: RelayConnection) {
    if (connection != RelayConnection.Offline) return
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .background(Fm.colors.bgOverlay)
            .clickable { ChatFeature.reconnect() }
            .padding(horizontal = FmSpace.x4, vertical = FmSpace.x2),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        FmIcon("cloud_off_outlined", size = 16.dp, tint = Fm.colors.textSecondary)
        Spacer(Modifier.width(FmSpace.x2))
        Text(
            "Offline - tap to retry",
            style = MaterialTheme.typography.bodyMedium,
            color = Fm.colors.textSecondary,
        )
    }
}

private fun Modifier.launchTarget(): Modifier = onGloballyPositioned { LaunchFlight.target = it.boundsInRoot() }

@Composable
private fun BadgeIcon(
    icon: String,
    count: Int,
    tint: androidx.compose.ui.graphics.Color,
    size: Int,
    description: String? = null,
    badgeColor: androidx.compose.ui.graphics.Color = Fm.colors.danger,
    pulse: Int = 0,
    modifier: Modifier = Modifier,
    desktopOffline: Boolean = false,
) {
    Box {
        BadgeIconBody(icon, count, tint, size, description, badgeColor, pulse, modifier)
        if (desktopOffline) {
            DesktopOfflineDot(Modifier.align(Alignment.BottomStart).offset(x = (-4).dp, y = 4.dp))
        }
    }
}

@Composable
private fun BadgeIconBody(
    icon: String,
    count: Int,
    tint: androidx.compose.ui.graphics.Color,
    size: Int,
    description: String?,
    badgeColor: androidx.compose.ui.graphics.Color,
    pulse: Int,
    modifier: Modifier,
) {
    val reduced = Fm.reducedMotion
    val pop = remember { Animatable(1f) }
    LaunchedEffect(pulse) {
        if (pulse == 0 || reduced) return@LaunchedEffect
        pop.animateTo(1.6f, spring(dampingRatio = 0.5f, stiffness = Spring.StiffnessMedium))
        pop.animateTo(1f, spring(dampingRatio = 0.35f, stiffness = Spring.StiffnessMediumLow))
    }
    val iconScale = 1f + (pop.value - 1f) * 0.25f
    val iconModifier = modifier.graphicsLayer { scaleX = iconScale; scaleY = iconScale }
    if (count <= 0) {
        FmIcon(icon, size = size.dp, tint = tint, contentDescription = description, modifier = iconModifier)
        return
    }
    BadgedBox(
        badge = {
            Badge(containerColor = badgeColor, modifier = Modifier.graphicsLayer { scaleX = pop.value; scaleY = pop.value }) {
                AnimatedContent(
                    targetState = count,
                    transitionSpec = {
                        (slideInVertically { it } + fadeIn()) togetherWith (slideOutVertically { -it } + fadeOut())
                    },
                    label = "badge",
                ) { shown -> Text(if (shown > 999) "999+" else "$shown") }
            }
        },
    ) {
        FmIcon(icon, size = size.dp, tint = tint, contentDescription = description, modifier = iconModifier)
    }
}

private class BarItem(
    val title: String,
    val icon: String,
    val badge: Int,
    val selected: Boolean,
    val badgeNoun: String,
    val agents: Boolean = false,
    val onClick: () -> Unit,
)

object ShellBar {
    val height = 60.dp
    val cell = 52.dp
    val margin = 12.dp
    val gap = 5.dp
    val homeIndicator = 34.dp
    val cornerOuter = 26.dp
    val cornerInner = 10.dp

    @Composable
    fun bottomInset(): Dp {
        val nav = with(LocalDensity.current) { WindowInsets.navigationBars.getBottom(this).toDp() }
        return maxOf(nav, homeIndicator) + gap
    }

    @Composable
    fun clearance(): Dp = bottomInset() + height + FmSpace.x2
}

@Composable
private fun HomePage(entries: List<ShellMenuEntry>, badges: Map<Route.Kind, ShellMenuBadge>, scope: kotlinx.coroutines.CoroutineScope) {
    ShellMenuGrid(
        entries,
        badges,
        onClose = {},
        scope = scope,
        modifier = Modifier.fillMaxSize().background(Fm.colors.bgBase),
        contentPadding = PaddingValues(start = FmSpace.x1, end = FmSpace.x1, bottom = ShellBar.clearance()),
        columns = 6,
        plain = true,
    )
}

@Composable
private fun GlassTabBar(items: List<BarItem>, agentsRunning: Boolean) {
    val c = Fm.colors
    val desktopOffline = rememberDesktopOffline()
    Row(
        modifier = Modifier
            .padding(start = ShellBar.margin, end = ShellBar.margin, top = FmSpace.x1, bottom = ShellBar.bottomInset())
            .fillMaxWidth()
            .height(ShellBar.height)
            .fmGlass(RoundedCornerShape(ShellBar.height / 2))
            .padding(FmSpace.x1),
        horizontalArrangement = Arrangement.spacedBy(2.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        items.forEachIndexed { index, item ->
            val isSelected = item.selected
            val count = item.badge
            val interaction = remember { MutableInteractionSource() }
            val pressed by interaction.collectIsPressedAsState()
            val start = if (index == 0) ShellBar.cornerOuter else ShellBar.cornerInner
            val end = if (index == items.lastIndex) ShellBar.cornerOuter else ShellBar.cornerInner
            val shape = RoundedCornerShape(topStart = start, bottomStart = start, topEnd = end, bottomEnd = end)
            Column(
                modifier = Modifier
                    .weight(1f)
                    .height(ShellBar.cell)
                    .graphicsLayer {
                        val k = if (pressed) 0.94f else 1f
                        scaleX = k
                        scaleY = k
                        alpha = if (pressed) 0.7f else 1f
                    }
                    .clip(shape)
                    .background(if (isSelected) c.accent.copy(alpha = 0.14f) else androidx.compose.ui.graphics.Color.Transparent)
                    .clickable(interactionSource = interaction, indication = null, onClick = item.onClick)
                    .semantics(mergeDescendants = true) {
                        role = Role.Tab
                        this.selected = isSelected
                        contentDescription = buildString {
                            append("${item.title}, tab ${index + 1} of ${items.size}")
                            if (count > 0) append(if (item.agents && agentsRunning) ", $count running" else ", $count ${item.badgeNoun}")
                        }
                    },
                horizontalAlignment = Alignment.CenterHorizontally,
                verticalArrangement = Arrangement.Center,
            ) {
                val iconColor = if (isSelected) c.accent else c.textPrimary
                if (item.agents) {
                    BadgeIcon(
                        item.icon, count, iconColor, 24,
                        badgeColor = c.accent,
                        pulse = LaunchFlight.landings,
                        modifier = Modifier.launchTarget(),
                        desktopOffline = desktopOffline,
                    )
                } else {
                    BadgeIcon(item.icon, count, iconColor, 24, badgeColor = c.accent)
                }
                Spacer(Modifier.height(3.dp))
                Text(
                    item.title,
                    style = Fm.text.labelSmall.copy(
                        fontSize = 10.sp,
                        lineHeight = 12.sp,
                        letterSpacing = 0.sp,
                        fontWeight = if (isSelected) FmType.emphasis else androidx.compose.ui.text.font.FontWeight.Medium,
                        color = if (isSelected) c.accent else c.textSecondary,
                    ),
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
            }
        }
    }
}
