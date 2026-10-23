package com.forkmesh.nativeapp.feature.navigation

import com.forkmesh.nativeapp.ui.kit.FmSafeInput
import com.forkmesh.nativeapp.ui.kit.confirmAction
import com.forkmesh.nativeapp.ui.kit.fmScrollEdge
import com.forkmesh.nativeapp.ui.kit.FmSheet
import androidx.compose.foundation.ExperimentalFoundationApi
import com.forkmesh.nativeapp.ui.theme.FmType
import androidx.compose.foundation.combinedClickable
import androidx.compose.ui.draw.drawBehind
import com.forkmesh.nativeapp.shell.ShellHomeIconStyle
import androidx.compose.ui.unit.Dp
import com.forkmesh.nativeapp.feature.account.settings.rememberNowMillis
import androidx.activity.compose.BackHandler
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.scaleIn
import androidx.compose.animation.scaleOut
import androidx.compose.animation.expandVertically
import androidx.compose.animation.shrinkVertically
import androidx.compose.animation.slideInVertically
import androidx.compose.animation.core.spring
import androidx.compose.ui.graphics.TransformOrigin
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.statusBars
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.verticalScroll
import androidx.compose.ui.draw.shadow
import androidx.compose.foundation.interaction.MutableInteractionSource
import com.forkmesh.nativeapp.ui.kit.FmAvatar
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.graphics.CompositingStrategy
import androidx.compose.ui.draw.drawWithContent
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.BlendMode
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.border
import androidx.compose.foundation.shape.CircleShape
import com.forkmesh.nativeapp.feature.market.PluginMenuButton
import android.content.ActivityNotFoundException
import android.content.Intent
import android.net.Uri
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxScope
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.RowScope
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.grid.GridCells
import androidx.compose.foundation.lazy.grid.GridItemSpan
import androidx.compose.foundation.lazy.grid.LazyGridScope
import androidx.compose.foundation.lazy.grid.LazyVerticalGrid
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.grid.items as gridItems
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.Badge
import androidx.compose.material3.BadgedBox
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.FilterChip
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.Text
import androidx.compose.material3.rememberModalBottomSheetState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.role
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.graphics.Color
import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.api.PrefStore
import com.forkmesh.nativeapp.code.CommitFields
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.account.AccountFeature
import com.forkmesh.nativeapp.feature.account.AuthService
import com.forkmesh.nativeapp.feature.account.SettingsService
import com.forkmesh.nativeapp.feature.account.projects.ProjectsService
import com.forkmesh.nativeapp.feature.account.settings.showUserStatusSheet
import com.forkmesh.nativeapp.feature.chat.ChatDepth
import com.forkmesh.nativeapp.feature.chat.ChatFeature
import com.forkmesh.nativeapp.feature.composer.ComposerFeature
import com.forkmesh.nativeapp.feature.inbox.InboxFeature
import com.forkmesh.nativeapp.feature.tools.DiagnosticsSendPage
import com.forkmesh.nativeapp.feature.tools.HubVersionRow
import com.forkmesh.nativeapp.feature.tools.ToolsFeature
import com.forkmesh.nativeapp.feature.tools.WhatsNewPage
import com.forkmesh.nativeapp.account.projects.LegalLinks
import com.forkmesh.nativeapp.account.projects.ProjectDefaults
import com.forkmesh.nativeapp.feature.market.PackageGateFeature
import com.forkmesh.nativeapp.market.PackageGate
import com.forkmesh.nativeapp.localrepos.LocalRepoStore
import com.forkmesh.nativeapp.navigation.AppTab
import com.forkmesh.nativeapp.navigation.ChatSearchSource
import com.forkmesh.nativeapp.navigation.CommandPalette
import com.forkmesh.nativeapp.navigation.ComposeContext
import com.forkmesh.nativeapp.navigation.CommitItem
import com.forkmesh.nativeapp.navigation.CommitsSearchSource
import com.forkmesh.nativeapp.navigation.ComposeTarget
import com.forkmesh.nativeapp.navigation.DestinationsSearchSource
import com.forkmesh.nativeapp.navigation.GlobalSearch
import com.forkmesh.nativeapp.navigation.GlobalSearchScope
import com.forkmesh.nativeapp.navigation.GlobalSearchSource
import com.forkmesh.nativeapp.navigation.HubCatalog
import com.forkmesh.nativeapp.navigation.HubItem
import com.forkmesh.nativeapp.navigation.IssuesSearchSource
import com.forkmesh.nativeapp.navigation.LocalRepositoryItem
import com.forkmesh.nativeapp.navigation.PaletteCommand
import com.forkmesh.nativeapp.navigation.PaletteContext
import com.forkmesh.nativeapp.navigation.PeopleSearchSource
import com.forkmesh.nativeapp.navigation.RepoRef
import com.forkmesh.nativeapp.navigation.RepositoriesSearchSource
import com.forkmesh.nativeapp.navigation.Route
import com.forkmesh.nativeapp.navigation.RouteCatalog
import com.forkmesh.nativeapp.navigation.SavedSearchStore
import com.forkmesh.nativeapp.navigation.SearchBucket
import com.forkmesh.nativeapp.navigation.SearchBucketKind
import com.forkmesh.nativeapp.navigation.SearchScope
import com.forkmesh.nativeapp.navigation.SearchableItem
import com.forkmesh.nativeapp.navigation.UpdateRequirement
import com.forkmesh.nativeapp.navigation.title
import com.forkmesh.nativeapp.ui.kit.FmEmptyState
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmInlineStatus
import com.forkmesh.nativeapp.ui.kit.FmInlineStatusLine
import com.forkmesh.nativeapp.ui.kit.FmStatus
import com.forkmesh.nativeapp.ui.kit.FmSectionHeader
import com.forkmesh.nativeapp.ui.kit.FmSoundOnShow
import com.forkmesh.nativeapp.ui.kit.FmSwitch
import com.forkmesh.nativeapp.ui.kit.FmTextButton
import com.forkmesh.nativeapp.ui.kit.LocalFmMessenger
import com.forkmesh.nativeapp.ui.kit.fmSafeAreaInsets
import com.forkmesh.nativeapp.ui.kit.rememberFmStatus
import com.forkmesh.nativeapp.ui.kit.fmTapSounds
import com.forkmesh.nativeapp.ui.navigation.AppNavigation
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmRadius
import com.forkmesh.nativeapp.ui.theme.FmSpace
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

@Composable
fun RoutePage(
    title: String,
    navigator: Navigator,
    actions: @Composable RowScope.() -> Unit = {},
    largeTitle: Boolean = false,
    content: @Composable BoxScope.() -> Unit,
) {
    val c = Fm.colors
    val large = com.forkmesh.nativeapp.ui.kit.rememberFmLargeTitle()
    Column(Modifier.fillMaxSize().background(c.bgBase)) {
        com.forkmesh.nativeapp.ui.kit.FmNavBar(title, onBack = { navigator.pop() }, titleAlpha = if (largeTitle) large.barTitleAlpha else 1f) {
            actions()
            PluginMenuButton()
        }
        if (largeTitle) com.forkmesh.nativeapp.ui.kit.FmLargeTitleBox(title, large, Modifier.weight(1f).fillMaxWidth(), content)
        else Box(Modifier.weight(1f).fillMaxWidth().fmScrollEdge(), content = content)
    }
}

class GuestGateScreen(private val route: Route, private val action: String) : Screen() {
    override val title: String get() = route.title

    @Composable
    override fun Content(navigator: Navigator) {
        val requireAccount = com.forkmesh.nativeapp.feature.account.AccountFeature.rememberRequireAccount()
        val screen = this
        RoutePage(title, navigator) {
            FmEmptyState(RouteCatalog.icon(route.kind), "Sign in to $action", message = "${RouteCatalog.title(route.kind)} needs a ForkMesh account.")
        }
        androidx.compose.runtime.LaunchedEffect(Unit) {
            val ok = requireAccount(action)
            if (navigator.stack.lastOrNull() !== screen) return@LaunchedEffect
            navigator.pop()
            if (ok) NavigationFeature.open(route)
        }
    }
}

class RoutePlaceholderScreen(private val route: Route) : Screen() {
    override val title: String get() = route.title

    @Composable
    override fun Content(navigator: Navigator) {
        RoutePage(title, navigator) {
            FmEmptyState(
                RouteCatalog.icon(route.kind),
                "Coming soon",
                message = "${RouteCatalog.title(route.kind)} is not in this version of the app yet.",
            )
        }
    }
}

class ApprovalRouteScreen(private val id: String) : Screen() {
    override val title: String get() = "Approval"

    @Composable
    override fun Content(navigator: Navigator) {
        RoutePage(title, navigator) {
            val parts = com.forkmesh.nativeapp.inbox.InboxRoutes.approvalParts(id)
            Column(Modifier.fillMaxSize().padding(FmSpace.x4), verticalArrangement = Arrangement.spacedBy(FmSpace.x3)) {
                Text("An agent is waiting for your answer.", style = Fm.text.titleMedium)
                if (parts != null) Text("Session ${parts.first}", style = Fm.text.bodyMedium.copy(color = Fm.colors.textSecondary))
                val handler = InboxFeature.approvals
                val scope = rememberCoroutineScope()
                val status = rememberFmStatus()
                var answered by remember { mutableStateOf(false) }
                if (handler != null && parts != null) {
                    if (!answered) {
                        Row(horizontalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                            TextAction("Approve", onClick = {
                                scope.launch {
                                    val answer = InboxFeature.decide(parts.first, parts.second, true)
                                    answered = answer.ok
                                    status.report(answer.message, !answer.ok)
                                }
                            })
                            TextAction("Deny", onClick = {
                                scope.launch {
                                    val answer = InboxFeature.decide(parts.first, parts.second, false)
                                    answered = answer.ok
                                    status.report(answer.message, !answer.ok)
                                }
                            })
                        }
                    }
                    FmInlineStatus(status, dismissible = !answered)
                } else {
                    Text(
                        "Open the agent on your desktop to answer it.",
                        style = Fm.text.bodyMedium.copy(color = Fm.colors.textSecondary),
                    )
                }
                if (parts != null) {
                    TextAction("Open the agent", onClick = { NavigationFeature.open(Route.Agent(parts.first)) })
                }
            }
        }
    }
}

class TourScreen : Screen() {
    override val title: String get() = "Tour"
    private var page by mutableStateOf(0)

    private val pages = listOf(
        Triple("auto_awesome", "Agents first", "Check on your agents, answer what needs you and start new runs from the Agents tab."),
        Triple("inbox", "One Inbox", "Pings, review requests, assigned tasks, mentions and approvals land in one list."),
        Triple("integration_instructions", "Code", "Browse, review and merge. Links from anywhere open the right repository, issue or pull request."),
        Triple("forum", "Chat", "Channels, direct messages and the local mesh room stay one tap away."),
        Triple("edit", "The prompt button", "It is always next to the tabs. It opens the composer aimed at whatever is on screen."),
        Triple("account_circle", "You and search", "Everything else is in the You hub behind your avatar, in search, or in the command palette."),
    )

    @Composable
    override fun Content(navigator: Navigator) {
        val c = Fm.colors
        val (icon, heading, body) = pages[page]
        Column(
            Modifier.fillMaxSize().background(c.bgBase).windowInsetsPadding(fmSafeAreaInsets()).padding(FmSpace.x5),
            verticalArrangement = Arrangement.SpaceBetween,
        ) {
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.End) {
                TextAction("Skip", onClick = { finish(navigator) })
            }
            Column(horizontalAlignment = Alignment.CenterHorizontally, modifier = Modifier.fillMaxWidth()) {
                FmIcon(icon, size = 56.dp, tint = c.accent)
                Spacer(Modifier.height(FmSpace.x4))
                Text(heading, style = Fm.text.headlineSmall, modifier = Modifier.semantics { heading() })
                Spacer(Modifier.height(FmSpace.x3))
                Text(body, style = Fm.text.bodyLarge.copy(color = c.textSecondary))
            }
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween, verticalAlignment = Alignment.CenterVertically) {
                Text("${page + 1} of ${pages.size}", style = Fm.text.bodySmall.copy(color = c.textTertiary))
                Row {
                    if (page > 0) TextAction("Back", onClick = { page-- })
                    TextAction(if (page == pages.lastIndex) "Done" else "Next", onClick = {
                        if (page == pages.lastIndex) finish(navigator) else page++
                    })
                }
            }
        }
    }

    private fun finish(navigator: Navigator) {
        Services.get<PrefStore>()?.putBool(TOUR_KEY, true)
        navigator.pop()
    }

    companion object {
        const val TOUR_KEY = "nav/tour.v1"
    }
}

@Composable
fun ShellFirstRun(navigator: Navigator) {
    LaunchedEffect(Unit) {
        val prefs = Services.get<PrefStore>() ?: return@LaunchedEffect
        if (prefs.getBool(TourScreen.TOUR_KEY) == true) return@LaunchedEffect
        if (!PackageGateFeature.isInstalled(PackageGate.TOUR)) return@LaunchedEffect
        prefs.putBool(TourScreen.TOUR_KEY, true)
        navigator.push(TourScreen())
    }
}

class UpdateRequiredScreen : Screen() {
    override val title: String get() = "Update required"

    @Composable
    override fun Content(navigator: Navigator) {
        val requirement by NavigationFeature.updateRequired.collectAsState()
        RoutePage(title, navigator) {
            UpdateRequiredView(requirement ?: UpdateRequirement("", "a newer version", null))
        }
    }
}

@Composable
fun UpdateRequiredView(requirement: UpdateRequirement) {
    val context = LocalContext.current
    val c = Fm.colors
    Box(Modifier.fillMaxSize().background(c.bgBase).clickable(enabled = false) {}, contentAlignment = Alignment.Center) {
        Column(Modifier.padding(FmSpace.x5), horizontalAlignment = Alignment.CenterHorizontally) {
            FmIcon("system_update", size = 56.dp, tint = c.accent)
            Spacer(Modifier.height(FmSpace.x4))
            Text("Update required", style = Fm.text.headlineSmall)
            Spacer(Modifier.height(FmSpace.x2))
            Text(requirement.summary, style = Fm.text.bodyMedium.copy(color = c.textSecondary))
            Spacer(Modifier.height(FmSpace.x2))
            Text(UpdateRequirement.ADVICE, style = Fm.text.bodyMedium.copy(color = c.textSecondary))
            val url = requirement.storeUrl
            if (url != null) {
                Spacer(Modifier.height(FmSpace.x4))
                var problem by remember { mutableStateOf<String?>(null) }
                TextAction("Get the update", onClick = {
                    problem = if (NavigationFeature.openInBrowser(context, url, report = false)) null else "No browser can open $url"
                })
                FmInlineStatusLine(problem, isError = true, modifier = Modifier.padding(top = FmSpace.x2))
            }
        }
    }
}

private const val SEARCH_REPO_LIMIT = 3

private fun repoRefsForSearch(query: String): List<RepoRef> {
    val catalog = runCatching { Services.get<ApiService>()?.repositories() }.getOrNull().orEmpty()
    val named = RepositoriesSearchSource.named(query, catalog, SEARCH_REPO_LIMIT)
    val recents = NavigationFeature.recents.value.mapNotNull { it.repoRef }
    val context = ComposerFeature.repoContext.value?.let(RepoRef::parse)
    return (named + listOfNotNull(context) + recents).distinct().take(SEARCH_REPO_LIMIT)
}

private fun localReposForSearch(): List<LocalRepositoryItem> {
    val store = Services.get<LocalRepoStore>() ?: return emptyList()
    return runCatching {
        store.ensureLoaded()
        store.records.map { LocalRepositoryItem(it.name, it.description, it.publishedAs.orEmpty()) }
    }.getOrDefault(emptyList())
}

private fun searchSources(): List<GlobalSearchSource> {
    val api = Services.get<ApiService>()
    val surfaces = PackageGateFeature.surfaces.value
    val sources = arrayListOf<GlobalSearchSource>(DestinationsSearchSource(DestinationsSearchSource.routes(surfaces.routeKinds)))
    sources += RepositoriesSearchSource({ api?.repositories().orEmpty() }, ::localReposForSearch)
    if (api != null) {
        if (surfaces.shows(PackageGate.ISSUES)) {
            sources += IssuesSearchSource(false, ::repoRefsForSearch) { repo ->
                api.publishedIssues(repo.owner, repo.name).map { SearchableItem(it.number, it.title, it.body, it.status, false) }
            }
        }
        if (surfaces.shows(PackageGate.PULL_REQUESTS)) {
            sources += IssuesSearchSource(true, ::repoRefsForSearch) { repo ->
                api.publishedPulls(repo.owner, repo.name).map { SearchableItem(it.number, it.title, it.body, it.status, true) }
            }
        }
        if (surfaces.shows(PackageGate.COMMITS)) {
            sources += CommitsSearchSource(::repoRefsForSearch) { repo ->
                api.commits(repo.owner, repo.name).map { CommitItem(CommitFields.hash(it), CommitFields.subject(it), CommitFields.author(it)) }
            }
        }
    }
    sources += PeopleSearchSource(
        { ChatFeature.relay?.roster()?.map { it.name }?.filter { it.isNotEmpty() }.orEmpty() },
        { name -> ChatDepth.direct?.searchUsers(name).orEmpty() },
    )
    if (surfaces.shows(PackageGate.CHAT)) sources += ChatSearchSource {
        val relay = ChatFeature.relay ?: return@ChatSearchSource emptyList()
        relay.channels + relay.roster().filter { !it.self && it.name.isNotEmpty() }.map { "@${it.name}" }
    }
    return sources
}

class GlobalSearchScreen(initialQuery: String = "", initialScope: SearchScope = SearchScope.Everything) : Screen() {
    override val title: String get() = "Search"
    private var query by mutableStateOf(initialQuery)
    private var scope by mutableStateOf(GlobalSearchScope.from(initialScope))
    private var buckets by mutableStateOf<List<SearchBucket>>(emptyList())
    private var failures by mutableStateOf<List<SearchBucketKind>>(emptyList())
    private var expanded by mutableStateOf<SearchBucketKind?>(null)
    private var searching by mutableStateOf(false)
    private var generation = 0
    private var savedTick by mutableStateOf(0)

    @Composable
    override fun Content(navigator: Navigator) {
        val c = Fm.colors
        val store = remember { Services.get<SavedSearchStore>() ?: SavedSearchStore(null) }
        val focus = remember { FocusRequester() }
        LaunchedEffect(Unit) { focus.requestFocus() }
        LaunchedEffect(query, scope, expanded) {
            val text = query.trim()
            val mine = ++generation
            if (text.isEmpty()) {
                buckets = emptyList()
                failures = emptyList()
                searching = false
                return@LaunchedEffect
            }
            delay(250)
            searching = true
            val (found, failed) = withContext(Dispatchers.IO) { GlobalSearch.runBlocking(searchSources(), text, scope, expanded) }
            if (mine != generation) return@LaunchedEffect
            buckets = found
            failures = failed
            searching = false
            store.recordRun(text, scope, found.sumOf { it.total })
            savedTick++
        }
        fun openHit(route: Route) {
            store.commit(query, scope)
            NavigationFeature.remember(route)
            NavigationFeature.open(route)
        }
        savedTick.let { }
        val isSaved = store.contains(query.trim(), scope)
        RoutePage(title, navigator, largeTitle = true, actions = {
            if (query.isNotBlank()) {
                FmIconButton(
                    if (isSaved) "bookmark" else "bookmark_border",
                    if (isSaved) "Remove saved search" else "Save search",
                    onPressed = {
                        val existing = store.saved.firstOrNull { it.query == query.trim() && it.scope == scope }
                        if (existing != null) store.remove(existing) else store.save(query.trim(), scope, buckets.sumOf { it.total })
                        savedTick++
                    },
                    color = c.textPrimary,
                )
            }
        }) {
            Column(Modifier.fillMaxSize()) {
                Row(
                    Modifier.fillMaxWidth().padding(FmSpace.x3).clip(RoundedCornerShape(FmRadius.md)).background(c.bgRaised)
                        .padding(horizontal = FmSpace.x3, vertical = FmSpace.x2),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    FmIcon("search", size = 20.dp, tint = c.textTertiary)
                    Spacer(Modifier.width(FmSpace.x2))
                    Box(Modifier.weight(1f)) {
                        if (query.isEmpty()) Text("Repositories, issues, people, destinations…", style = Fm.text.bodyLarge.copy(color = c.textTertiary))
                        BasicTextField(
                            value = query,
                            onValueChange = FmSafeInput.typing { query = it; expanded = null },
                            singleLine = true,
                            textStyle = Fm.text.bodyLarge.copy(color = c.textPrimary),
                            cursorBrush = SolidColor(c.accent),
                            keyboardOptions = KeyboardOptions(imeAction = ImeAction.Search),
                            keyboardActions = KeyboardActions(onSearch = { store.commit(query, scope); savedTick++ }),
                            modifier = Modifier.fillMaxWidth().focusRequester(focus).semantics { contentDescription = "Search" },
                        )
                    }
                    if (query.isNotEmpty()) FmIconButton("close", "Clear", onPressed = { query = "" })
                }
                Row(Modifier.horizontalScroll(rememberScrollState()).padding(horizontal = FmSpace.x3), horizontalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                    GlobalSearchScope.entries.forEach { option ->
                        FilterChip(selected = option == scope, onClick = { scope = option; expanded = null }, label = { Text(option.title) })
                    }
                }
                LazyColumn(Modifier.fillMaxSize(), contentPadding = PaddingValues(bottom = FmSpace.x6)) {
                    if (query.isBlank()) {
                        if (store.saved.isNotEmpty()) {
                            item { FmSectionHeader("Saved searches") }
                            items(store.saved, key = { "saved:${it.id}" }) { saved ->
                                ResultRow("bookmark", saved.query, "${saved.scope.title} · ${saved.lastCount} results") {
                                    query = saved.query
                                    scope = saved.scope
                                }
                            }
                        }
                        if (store.recent.isNotEmpty()) {
                            item {
                                Row(verticalAlignment = Alignment.CenterVertically) {
                                    Box(Modifier.weight(1f)) { FmSectionHeader("Recent") }
                                    TextAction("Clear", onClick = { store.clearRecent(); savedTick++ })
                                }
                            }
                            items(store.recent, key = { "recent:${it.id}" }) { recent ->
                                ResultRow("history", recent.query, recent.scope.title) {
                                    query = recent.query
                                    scope = recent.scope
                                }
                            }
                        }
                    } else {
                        if (searching && buckets.isEmpty()) item { Text("Searching…", Modifier.padding(FmSpace.x4), style = Fm.text.bodyMedium.copy(color = c.textSecondary)) }
                        if (!searching && buckets.isEmpty()) {
                            item { Text("No results for “${query.trim()}”.", Modifier.padding(FmSpace.x4), style = Fm.text.bodyMedium.copy(color = c.textSecondary)) }
                        }
                        buckets.forEach { bucket ->
                            item(key = "h:${bucket.kind.raw}") {
                                Row(verticalAlignment = Alignment.CenterVertically) {
                                    Box(Modifier.weight(1f)) { FmSectionHeader(bucket.kind.title, count = bucket.total) }
                                    if (bucket.hasMore && expanded == null) TextAction("See all", onClick = { expanded = bucket.kind })
                                }
                            }
                            items(bucket.hits, key = { "${bucket.kind.raw}:${it.id}" }) { hit ->
                                ResultRow(hit.icon, hit.title, hit.subtitle) { openHit(hit.route) }
                            }
                        }
                        if (expanded != null) item { TextAction("Show all buckets", onClick = { expanded = null }) }
                        if (failures.isNotEmpty()) {
                            item {
                                Text(
                                    "Could not search ${failures.joinToString { it.title.lowercase() }}.",
                                    Modifier.padding(FmSpace.x4),
                                    style = Fm.text.bodySmall.copy(color = c.danger),
                                )
                            }
                        }
                    }
                }
            }
        }
    }
}

@Composable
fun TextAction(label: String, onClick: () -> Unit) {
    FmTextButton(onClick = onClick) { Text(label) }
}

@Composable
private fun ResultRow(icon: String, title: String, subtitle: String, onClick: () -> Unit) {
    val c = Fm.colors
    Row(
        Modifier.fillMaxWidth().heightIn(min = 48.dp).clickable(onClick = onClick).padding(horizontal = FmSpace.x4, vertical = FmSpace.x2),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        FmIcon(icon, size = 22.dp, tint = c.textTertiary)
        Spacer(Modifier.width(FmSpace.x3))
        Column(Modifier.weight(1f)) {
            Text(title, style = Fm.text.bodyLarge, maxLines = 1, overflow = TextOverflow.Ellipsis)
            if (subtitle.isNotEmpty()) {
                Text(subtitle, style = Fm.text.bodySmall.copy(color = c.textSecondary), maxLines = 1, overflow = TextOverflow.Ellipsis)
            }
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun CommandPaletteSheet(
    tab: AppTab,
    path: List<Route>,
    onClose: () -> Unit,
    onCompose: (ComposeTarget) -> Unit,
    onSelectTab: (AppTab) -> Unit,
) {
    val c = Fm.colors
    var query by remember { mutableStateOf("") }
    val recents by NavigationFeature.recents.collectAsState()
    val pending by InboxFeature.pendingApprovals.collectAsState()
    val guest = Services.get<AuthService>()?.isGuest?.collectAsState()?.value == true
    val conversation = ChatFeature.relaySnapshot.collectAsState().value?.currentConversation.orEmpty()
    val surfaces by PackageGateFeature.surfaces.collectAsState()
    val context = PaletteContext(ComposeContext.target(path, tab, conversation), tab, pending, recents, guest, surfaces.routeKinds)
    val sections = CommandPalette.sections(query, context)
    val focus = remember { FocusRequester() }
    LaunchedEffect(Unit) { focus.requestFocus() }
    fun run(command: PaletteCommand) {
        onClose()
        when (command) {
            is PaletteCommand.Compose -> onCompose(command.target)
            is PaletteCommand.Open -> {
                NavigationFeature.remember(command.route)
                NavigationFeature.open(command.route)
            }
            is PaletteCommand.SelectTab -> onSelectTab(command.tab)
            PaletteCommand.SwitchProject -> com.forkmesh.nativeapp.feature.account.projects.ProjectDrawer.open()
            is PaletteCommand.SearchEverywhere -> NavigationFeature.open(Route.Search(command.query, SearchScope.Everything))
        }
    }
    FmSoundOnShow()
    ModalBottomSheet(
        onDismissRequest = onClose,
        modifier = Modifier.fmTapSounds(),
        sheetState = rememberModalBottomSheetState(skipPartiallyExpanded = true),
        containerColor = c.bgRaised,
        shape = RoundedCornerShape(topStart = FmSheet.corner, topEnd = FmSheet.corner),
    ) {
        Column(Modifier.padding(horizontal = FmSpace.x3)) {
            Row(
                Modifier.fillMaxWidth().clip(RoundedCornerShape(FmRadius.md)).background(c.bgBase).padding(FmSpace.x3),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                FmIcon("keyboard_command_key", size = 20.dp, tint = c.textTertiary)
                Spacer(Modifier.width(FmSpace.x2))
                Box(Modifier.weight(1f)) {
                    if (query.isEmpty()) Text("Type a command, owner/repo#12 or @name", style = Fm.text.bodyLarge.copy(color = c.textTertiary))
                    BasicTextField(
                        value = query,
                        onValueChange = FmSafeInput.typing { query = it },
                        singleLine = true,
                        textStyle = Fm.text.bodyLarge.copy(color = c.textPrimary),
                        cursorBrush = SolidColor(c.accent),
                        keyboardOptions = KeyboardOptions(imeAction = ImeAction.Go),
                        keyboardActions = KeyboardActions(onGo = { sections.firstOrNull()?.entries?.firstOrNull()?.let { run(it.command) } }),
                        modifier = Modifier.fillMaxWidth().focusRequester(focus).semantics { contentDescription = "Command" },
                    )
                }
            }
            LazyColumn(Modifier.fillMaxWidth().heightIn(max = 520.dp), contentPadding = PaddingValues(bottom = FmSpace.x5)) {
                sections.forEach { section ->
                    item(key = "s:${section.id}") { FmSectionHeader(section.title) }
                    items(section.entries, key = { "${section.id}:${it.id}" }) { entry ->
                        ResultRow(entry.icon, entry.title, entry.subtitle) { run(entry.command) }
                    }
                }
            }
        }
    }
}

@Composable
fun rememberHubItemRunner(scope: CoroutineScope, status: FmStatus): (HubItem) -> Boolean {
    val context = LocalContext.current
    val messenger = LocalFmMessenger.current
    val auth = Services.get<AuthService>()
    val guest = auth?.isGuest?.collectAsState()?.value == true
    val requireAccount = AccountFeature.rememberRequireAccount()
    fun run(item: HubItem): Boolean {
        status.clear()
        when (val action = item.action) {
            is HubItem.Action.Open -> {
                val guestAction = item.guestAction
                if (guest && guestAction != null) {
                    scope.launch { if (requireAccount(guestAction)) NavigationFeature.openFromHub(action.route) }
                } else {
                    NavigationFeature.openFromHub(action.route)
                }
            }
            HubItem.Action.World -> {
                val url = Services.get<ProjectsService>()?.directory?.active?.worldUri ?: ProjectDefaults.PROD_WORLD_URL
                try {
                    context.startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(url)).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK))
                } catch (_: ActivityNotFoundException) {
                    status.error("Could not open ForkMesh World at $url")
                }
            }
            HubItem.Action.DesignSystem -> {
                val url = LegalLinks.designFor(Services.get<ProjectsService>()?.active?.value?.homepageUri ?: ProjectDefaults.PROD_HOMEPAGE_URL)
                if (!NavigationFeature.openInBrowser(context, url, report = false)) {
                    status.error("Could not open the design system at $url")
                }
            }
            HubItem.Action.ReportBug -> ToolsFeature.onSharedText(AppNavigation.navigator, "")
            HubItem.Action.SetStatus -> if (auth != null) scope.launch { showUserStatusSheet(messenger, auth) }
            HubItem.Action.SignIn -> AccountFeature.openSignIn()
            HubItem.Action.SignOut -> scope.launch {
                if (messenger.confirmAction("Sign out of this project?", "Sign out")) {
                    auth?.logout()
                }
            }
        }
        return status.message == null
    }
    return ::run
}

private enum class HubPage { Main, WhatsNew, Diagnostics }

object YouHubMetrics {
    val width = 320.dp
    val maxHeight = 640.dp
    val drop = 54.dp
    val corner = 18.dp
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun YouHubPanel(open: Boolean, onClose: () -> Unit, scope: CoroutineScope) {
    val c = Fm.colors
    var page by remember { mutableStateOf(HubPage.Main) }
    val reduced = Fm.reducedMotion
    val spring = spring<Float>(dampingRatio = 0.86f, stiffness = 305f)
    BackHandler(enabled = open) { onClose() }
    AnimatedVisibility(open, enter = fadeIn(), exit = fadeOut()) {
        Box(
            Modifier
                .fillMaxSize()
                .background(Color.Black.copy(alpha = 0.2f))
                .clickable(interactionSource = remember { MutableInteractionSource() }, indication = null, onClick = onClose),
        )
    }
    BoxWithConstraints(Modifier.fillMaxSize().windowInsetsPadding(WindowInsets.statusBars)) {
        val width = minOf(YouHubMetrics.width, maxWidth - FmSpace.x3 * 2)
        val height = maxOf(240.dp, minOf(YouHubMetrics.maxHeight, maxHeight - YouHubMetrics.drop - FmSpace.x4))
        AnimatedVisibility(
            open,
            modifier = Modifier.align(Alignment.TopEnd).padding(top = YouHubMetrics.drop, end = FmSpace.x3),
            enter = if (reduced) fadeIn() else fadeIn(spring) + scaleIn(spring, initialScale = 0.9f, transformOrigin = TransformOrigin(1f, 0f)) +
                expandVertically(spring(dampingRatio = 0.86f, stiffness = 305f), expandFrom = Alignment.Top) +
                slideInVertically(spring(dampingRatio = 0.86f, stiffness = 305f)) { -it / 12 },
            exit = if (reduced) fadeOut() else fadeOut(spring) + scaleOut(spring, targetScale = 0.9f, transformOrigin = TransformOrigin(1f, 0f)) +
                shrinkVertically(spring(dampingRatio = 0.86f, stiffness = 305f), shrinkTowards = Alignment.Top),
        ) {
            Box(
                Modifier
                    .width(width)
                    .heightIn(max = height)
                    .shadow(24.dp, RoundedCornerShape(YouHubMetrics.corner), ambientColor = Color.Black.copy(alpha = 0.28f), spotColor = Color.Black.copy(alpha = 0.28f))
                    .clip(RoundedCornerShape(YouHubMetrics.corner))
                    .background(if (c.dark) c.bgOverlay else c.bgRaised)
                    .semantics { contentDescription = "You hub" },
            ) {
                YouHubContent(scope, onClose, onWhatsNew = { page = HubPage.WhatsNew }, onDiagnostics = { page = HubPage.Diagnostics })
            }
        }
    }
    if (page != HubPage.Main) {
        ModalBottomSheet(
            onDismissRequest = { page = HubPage.Main },
            sheetState = rememberModalBottomSheetState(skipPartiallyExpanded = true),
            containerColor = if (c.dark) c.bgOverlay else c.bgRaised,
            shape = RoundedCornerShape(topStart = FmSheet.corner, topEnd = FmSheet.corner),
        ) {
            if (page == HubPage.WhatsNew) {
                WhatsNewPage(onBack = { page = HubPage.Main })
            } else {
                DiagnosticsSendPage(onBack = { page = HubPage.Main }, onSent = {
                    page = HubPage.Main
                    onClose()
                })
            }
        }
    }
}

@Composable
private fun YouHubContent(scope: CoroutineScope, onClose: () -> Unit, onWhatsNew: () -> Unit, onDiagnostics: () -> Unit) {
    val c = Fm.colors
    val auth = Services.get<AuthService>()
    val guest = auth?.isGuest?.collectAsState()?.value == true
    val session = auth?.session?.collectAsState()?.value
    val account by AccountFeature.shellState.collectAsState()
    val settings = Services.get<SettingsService>()
    val surfaces by PackageGateFeature.surfaces.collectAsState()
    val groups = HubCatalog.groups(guest, session?.isAdmin == true, surfaces.hubItems, NavigationFeature::isRegistered)
    val status = rememberFmStatus()
    val runHubItem = rememberHubItemRunner(scope, status)
    val now = rememberNowMillis()
    val visible = account.userStatus.visible(now)
    val described = listOf(visible.emoji, visible.text).filter { it.isNotEmpty() }.joinToString(" ")
    val all = groups.flatMap { it.items }
    val setStatus = all.firstOrNull { it.action == HubItem.Action.SetStatus }
    val accountItems = all.filter { item ->
        item.route?.kind == Route.Kind.Settings ||
            (item.route == null && item.action != HubItem.Action.World && item.action != HubItem.Action.DesignSystem &&
                item.action != HubItem.Action.ReportBug && item.action != HubItem.Action.SetStatus)
    }
    fun activate(item: HubItem) {
        if (runHubItem(item)) onClose()
    }
    Column(
        Modifier.verticalScroll(rememberScrollState()).padding(FmSpace.x3),
        verticalArrangement = Arrangement.spacedBy(FmSpace.x2),
    ) {
        if (status.message != null) FmInlineStatus(status)
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
            Box {
                FmAvatar(account.label.ifEmpty { "Guest" }, 40.dp, avatarPng = session?.avatarPng.orEmpty())
                Box(
                    Modifier
                        .align(Alignment.BottomEnd)
                        .offset(1.dp, 1.dp)
                        .size(12.dp)
                        .background(if (visible.away) c.warning else c.success, CircleShape),
                )
            }
            Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(2.dp)) {
                Text(if (guest) "Guest" else account.label.ifEmpty { "You" }, style = Fm.text.titleSmall, maxLines = 1, overflow = TextOverflow.Ellipsis)
                if (described.isNotEmpty()) Text(described, style = Fm.text.bodySmall, maxLines = 1, overflow = TextOverflow.Ellipsis)
            }
            if (setStatus != null) HubChip("mood", "Set a status") { activate(setStatus) }
        }
        HubDivider()
        com.forkmesh.nativeapp.feature.pocket.PocketModeRow(onClose)
        com.forkmesh.nativeapp.feature.genie.SuggestionModeRow()
        if (settings != null) {
            val shown by settings.showTelemetryStrip.collectAsState()
            HubToggleRow("monitor_heart", "Diagnostics", shown, settings::setShowTelemetryStrip)
        }
        if (accountItems.isNotEmpty()) {
            HubDivider()
            FlowRow(horizontalArrangement = Arrangement.spacedBy(FmSpace.x2), verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                accountItems.forEach { item ->
                    HubChip(item.icon, item.title, destructive = item.action == HubItem.Action.SignOut) { activate(item) }
                }
            }
        }
        HubVersionRow(onWhatsNew = onWhatsNew, onDiagnostics = onDiagnostics)
    }
}

@Composable
private fun HubDivider() {
    Box(Modifier.fillMaxWidth().height(0.5.dp).background(Fm.colors.textPrimary.copy(alpha = 0.12f)))
}

@Composable
private fun HubToggleRow(icon: String, title: String, checked: Boolean, onChange: (Boolean) -> Unit) {
    val c = Fm.colors
    Row(
        Modifier.fillMaxWidth().heightIn(min = 36.dp).semantics(mergeDescendants = true) {},
        verticalAlignment = Alignment.CenterVertically,
    ) {
        FmIcon(icon, size = 20.dp, tint = c.accent, outlined = true)
        Spacer(Modifier.width(FmSpace.x3))
        Text(title, Modifier.weight(1f), style = Fm.text.bodyLarge.copy(fontSize = Fm.text.bodyMedium.fontSize * 1.15f))
        FmSwitch(checked = checked, onCheckedChange = onChange)
    }
}

@Composable
private fun HubChip(icon: String, title: String, destructive: Boolean = false, onClick: () -> Unit) {
    val c = Fm.colors
    val tint = if (destructive) c.danger else c.textPrimary
    Row(
        Modifier
            .heightIn(min = 34.dp)
            .clip(CircleShape)
            .background(tint.copy(alpha = 0.12f))
            .clickable(onClick = onClick)
            .padding(horizontal = FmSpace.x3)
            .semantics(mergeDescendants = true) { role = Role.Button },
        verticalAlignment = Alignment.CenterVertically,
    ) {
        FmIcon(icon, size = 18.dp, tint = tint, outlined = true)
        Spacer(Modifier.width(FmSpace.x2))
        Text(title, style = Fm.text.labelMedium.copy(color = tint), maxLines = 1)
    }
}

class ShellMenuEntry(
    val id: String,
    val title: String,
    val icon: String,
    val badge: Int = 0,
    val selected: Boolean = false,
    val running: Boolean = false,
    val onClick: () -> Unit,
)

class ShellMenuBadge(val count: Int, val running: Boolean = false)

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun ShellMenuSheet(
    main: List<ShellMenuEntry>,
    hubBadges: Map<Route.Kind, ShellMenuBadge>,
    onClose: () -> Unit,
    scope: CoroutineScope,
) {
    FmSoundOnShow()
    ModalBottomSheet(
        onDismissRequest = onClose,
        modifier = Modifier.fmTapSounds(),
        sheetState = rememberModalBottomSheetState(skipPartiallyExpanded = true),
        containerColor = Fm.colors.bgRaised,
        shape = RoundedCornerShape(topStart = FmSheet.corner, topEnd = FmSheet.corner),
    ) {
        ShellMenuGrid(main, hubBadges, onClose, scope, Modifier.fillMaxSize(), PaddingValues(start = FmSpace.x3, end = FmSpace.x3, bottom = FmSpace.x6))
    }
}

@Composable
fun ShellMenuGrid(
    main: List<ShellMenuEntry>,
    hubBadges: Map<Route.Kind, ShellMenuBadge>,
    onClose: () -> Unit,
    scope: CoroutineScope,
    modifier: Modifier = Modifier,
    contentPadding: PaddingValues = PaddingValues(),
    columns: Int = 5,
    plain: Boolean = false,
) {
    val auth = Services.get<AuthService>()
    val guest = auth?.isGuest?.collectAsState()?.value == true
    val session = auth?.session?.collectAsState()?.value
    val surfaces by PackageGateFeature.surfaces.collectAsState()
    val groups = HubCatalog.pages(HubCatalog.groups(guest, session?.isAdmin == true, surfaces.hubItems, NavigationFeature::isRegistered))
    val status = rememberFmStatus()
    val runHubItem = rememberHubItemRunner(scope, status)
    val desktopOffline = rememberDesktopOffline()
    LazyVerticalGrid(
        columns = GridCells.Fixed(columns),
        modifier = modifier.semantics { contentDescription = "All pages" },
        contentPadding = contentPadding,
        horizontalArrangement = Arrangement.spacedBy(if (plain) 0.dp else FmSpace.x1),
        verticalArrangement = Arrangement.spacedBy(if (plain) 0.dp else FmSpace.x1),
    ) {
        if (status.message != null) {
            item(key = "menu-status", span = { GridItemSpan(maxLineSpan) }) { FmInlineStatus(status) }
        }
        menuSection("main", "Go to", plain) {
            gridItems(main, key = { "main:${it.id}" }) { entry ->
                val offline = desktopOffline && PackageGate.reliesOnDesktop(entry.id)
                ShellMenuTile(entry.icon, entry.title, entry.badge, entry.selected, entry.running, plain, offline) {
                    onClose()
                    entry.onClick()
                }
            }
        }
        groups.forEach { group ->
            menuSection(group.id, group.title, plain) {
                gridItems(group.items, key = { "${group.id}:${it.id}" }) { item ->
                    val badge = item.route?.let { hubBadges[it.kind] }
                    val offline = desktopOffline && PackageGate.reliesOnDesktop(item.id, item.route?.kind)
                    ShellMenuTile(item.icon, item.title, badge?.count ?: 0, false, badge?.running == true, plain, offline) {
                        if (runHubItem(item)) onClose()
                    }
                }
            }
        }
    }
}

private fun LazyGridScope.menuSection(id: String, title: String, plain: Boolean, content: LazyGridScope.() -> Unit) {
    item(key = "section:$id", span = { GridItemSpan(maxLineSpan) }) {
        if (plain) {
            Row(
                Modifier.padding(start = FmSpace.x3, end = FmSpace.x3, top = FmSpace.x4, bottom = FmSpace.x2).semantics { heading() },
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Text(title.uppercase(), style = Fm.text.labelSmall.copy(fontSize = 12.sp, letterSpacing = 1.2.sp), color = Fm.colors.textTertiary)
                Spacer(Modifier.width(FmSpace.x3))
                Box(Modifier.weight(1f).height(1.dp).background(Fm.colors.border))
            }
        } else {
            FmSectionHeader(title, Modifier.padding(start = FmSpace.x1, top = FmSpace.x2, bottom = FmSpace.x1).semantics { heading() })
        }
    }
    content()
}

@Composable
private fun ShellMenuTile(icon: String, title: String, badge: Int, selected: Boolean, running: Boolean, plain: Boolean, desktopOffline: Boolean, onClick: () -> Unit) {
    if (plain) {
        ShellHomeTile(icon, title, badge, selected, running, desktopOffline, onClick = onClick)
        return
    }
    val c = Fm.colors
    val tint = if (selected) c.accent else c.textPrimary
    Column(
        Modifier
            .fillMaxWidth()
            .height(if (plain) 48.dp else 52.dp)
            .clip(RoundedCornerShape(FmRadius.md))
            .background(if (selected) c.accent.copy(alpha = 0.14f) else if (plain) Color.Transparent else c.bgOverlay)
            .clickable(onClick = onClick)
            .semantics(mergeDescendants = true) {
                role = Role.Button
                contentDescription = listOfNotNull(
                    title,
                    if (badge > 0) "$badge ${if (running) "running" else "new"}" else null,
                    if (desktopOffline) "desktop offline" else null,
                ).joinToString(", ")
            }
            .padding(horizontal = 2.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.Center,
    ) {
        Box {
            if (badge > 0) {
                BadgedBox(badge = { Badge(containerColor = if (running) c.accent else c.danger) { Text(if (badge > 999) "999+" else "$badge") } }) {
                    FmIcon(icon, size = 18.dp, tint = tint)
                }
            } else {
                FmIcon(icon, size = 18.dp, tint = tint)
            }
            if (desktopOffline) {
                DesktopOfflineDot(Modifier.align(Alignment.BottomStart).offset(x = (-4).dp, y = 4.dp))
            }
        }
        Spacer(Modifier.height(if (plain) 4.dp else 2.dp))
        Text(
            title,
            style = if (plain) Fm.text.bodyMedium.copy(fontSize = 12.sp, color = if (selected) c.accent else c.textSecondary) else Fm.text.labelSmall.copy(color = if (selected) c.accent else c.textSecondary),
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
        )
    }
}

object ShellHomeMetrics {
    val iconSize = 22.dp
    val glyph = 26.dp
    val box = 39.dp
    val tileHeight = 66.dp
    val gap = 5.dp
    val anchorRise = 2.dp
    const val ACROSS = 5
}

@OptIn(ExperimentalFoundationApi::class)
@Composable
internal fun ShellHomeTile(
    icon: String,
    title: String,
    badge: Int,
    selected: Boolean,
    running: Boolean,
    desktopOffline: Boolean,
    iconStyle: ShellHomeIconStyle = ShellHomeIconStyle.outline,
    heat: Double = 0.0,
    tileHeight: Dp = ShellHomeMetrics.tileHeight,
    updated: Boolean = false,
    loading: Boolean = false,
    onLongClick: (() -> Unit)? = null,
    onClick: () -> Unit,
) {
    val c = Fm.colors
    val unreadBadge = badge > 0 && !running
    val spinning = loading && !unreadBadge
    val lit = selected || running
    val accent = lit || iconStyle == ShellHomeIconStyle.accent
    val top = if (accent) c.accent else c.textPrimary
    val bottom = if (accent) c.accent.copy(alpha = 0.72f) else c.textPrimary.copy(alpha = if (c.dark) 0.74f else 0.82f)
    val tiled = iconStyle == ShellHomeIconStyle.tiles
    val box = ShellHomeMetrics.box
    val iconSize = ShellHomeMetrics.iconSize
    val inset = (box - iconSize) / 2
    val corner = box * 0.3f
    Column(
        Modifier
            .fillMaxWidth()
            .height(tileHeight)
            .combinedClickable(onLongClick = onLongClick, onClick = onClick)
            .semantics(mergeDescendants = true) {
                role = Role.Button
                contentDescription = listOfNotNull(
                    title,
                    if (spinning) "Loading" else null,
                    if (badge > 0 && !spinning) "$badge ${if (running) "running" else "new"}" else null,
                    if (desktopOffline) "desktop offline" else null,
                ).joinToString(", ")
            },
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.Center,
    ) {
        Box(Modifier.size(box), contentAlignment = Alignment.Center) {
            if (heat > 0 && !tiled) {
                val h = heat.toFloat()
                Box(
                    Modifier
                        .size(box * 1.6f)
                        .drawBehind {
                            drawCircle(
                                Brush.radialGradient(
                                    listOf(c.accent.copy(alpha = 0.1f + 0.3f * h), c.accent.copy(alpha = 0f)),
                                    center = center,
                                    radius = box.toPx() * (0.5f + 0.25f * h),
                                ),
                                radius = box.toPx() * (0.5f + 0.25f * h),
                            )
                        },
                )
            }
            if (tiled) {
                val raised = if (c.dark) c.bgElevated else c.bgRaised
                val shape = RoundedCornerShape(corner)
                val shadow = Color.Black.copy(alpha = if (c.dark) 0.5f else 0.1f)
                Box(
                    Modifier
                        .size(box)
                        .shadow(3.dp, shape, clip = false, ambientColor = shadow, spotColor = shadow)
                        .clip(shape)
                        .background(Brush.verticalGradient(listOf(raised, c.bgOverlay)))
                        .then(
                            if (heat > 0) {
                                Modifier.background(Brush.radialGradient(listOf(c.accent.copy(alpha = 0.06f + 0.22f * heat.toFloat()), c.accent.copy(alpha = 0f))))
                            } else {
                                Modifier
                            },
                        )
                        .border(0.75.dp, Brush.verticalGradient(listOf(Color.White.copy(alpha = if (c.dark) 0.16f else 0.9f), Color.White.copy(alpha = if (c.dark) 0.03f else 0.2f))), shape),
                )
            }
            Box(
                Modifier
                    .graphicsLayer(compositingStrategy = CompositingStrategy.Offscreen)
                    .drawWithContent {
                        drawContent()
                        drawRect(Brush.verticalGradient(listOf(top, bottom)), blendMode = BlendMode.SrcIn)
                    },
            ) {
                FmIcon(
                    icon,
                    size = if (tiled) ShellHomeMetrics.glyph * 0.92f else ShellHomeMetrics.glyph,
                    tint = top,
                    outlined = iconStyle != ShellHomeIconStyle.filled,
                )
            }
            if (desktopOffline) {
                val d = if (tiled) corner * 0.3f else inset
                DesktopOfflineDot(Modifier.align(Alignment.TopStart).offset(x = d - 5.dp, y = box - d - 5.dp))
            }
            if (updated) {
                val d = if (tiled) corner * 0.3f else inset
                ShellHomeUpdateDot(Modifier.align(Alignment.TopStart).offset(x = box - d - 4.5.dp, y = box - d - 4.5.dp))
            }
            val anchor = Modifier.align(Alignment.TopStart).offset(
                x = box - if (tiled) minOf(12.dp, box * 0.28f) else inset + iconSize * 0.12f,
                y = -ShellHomeMetrics.anchorRise,
            )
            if (spinning) {
                ShellHomeCountSpinner(anchor)
            } else if (badge > 0) {
                ShellHomeCountMark(
                    if (badge > 9999) "${badge / 1000}k" else "%,d".format(badge),
                    unread = !running,
                    modifier = anchor,
                )
            }
        }
        Spacer(Modifier.height(ShellHomeMetrics.gap))
        Text(
            title,
            style = TextStyle(
                fontFamily = FmType.ui,
                fontSize = 12.sp,
                lineHeight = 14.sp,
                fontWeight = if (selected) FontWeight.SemiBold else FontWeight.Medium,
                color = if (selected) c.accent else c.textSecondary,
            ),
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
            modifier = Modifier.padding(horizontal = 2.dp),
        )
    }
}

@Composable
fun ShellHomeCountSpinner(modifier: Modifier = Modifier) {
    val c = Fm.colors
    Box(
        modifier
            .background(c.bgBase, CircleShape)
            .padding(1.5.dp)
            .size(19.dp)
            .background(if (c.dark) c.bgElevated else c.bgRaised, CircleShape)
            .border(0.5.dp, c.count.copy(alpha = 0.3f), CircleShape),
        contentAlignment = Alignment.Center,
    ) {
        androidx.compose.material3.CircularProgressIndicator(Modifier.size(11.dp), color = c.count, strokeWidth = 1.5.dp)
    }
}

@Composable
fun ShellHomeUpdateDot(modifier: Modifier = Modifier) {
    val c = Fm.colors
    Box(
        modifier
            .background(c.bgBase, CircleShape)
            .padding(1.5.dp)
            .size(6.dp)
            .background(c.accent, CircleShape),
    )
}

@Composable
fun ShellHomeCountMark(text: String, unread: Boolean, modifier: Modifier = Modifier) {
    val c = Fm.colors
    Box(
        modifier
            .background(c.bgBase, CircleShape)
            .padding(1.5.dp),
    ) {
        Box(
            Modifier
                .heightIn(min = 19.dp)
                .widthIn(min = 19.dp)
                .then(
                    if (unread) {
                        Modifier
                            .background(Brush.verticalGradient(listOf(c.accent, c.accent.copy(alpha = 0.82f))), CircleShape)
                            .border(0.5.dp, Color.White.copy(alpha = 0.22f), CircleShape)
                    } else {
                        Modifier
                            .background(if (c.dark) c.bgElevated else c.bgRaised, CircleShape)
                            .background(c.count.copy(alpha = 0.16f), CircleShape)
                            .border(0.5.dp, c.count.copy(alpha = 0.3f), CircleShape)
                    },
                )
                .padding(horizontal = 5.5.dp),
            contentAlignment = Alignment.Center,
        ) {
            Text(
                text,
                style = TextStyle(
                    fontFamily = FmType.ui,
                    fontSize = 11.sp,
                    lineHeight = 13.sp,
                    fontWeight = FontWeight.Bold,
                    fontFeatureSettings = "tnum",
                    color = if (unread) c.onAccent else c.count,
                ),
                maxLines = 1,
            )
        }
    }
}
