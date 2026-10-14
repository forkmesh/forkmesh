package com.forkmesh.nativeapp.feature.market

import android.net.Uri
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.ime
import androidx.compose.foundation.layout.navigationBars
import androidx.compose.foundation.layout.union
import androidx.compose.foundation.layout.windowInsetsBottomHeight
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.material3.HorizontalDivider
import androidx.compose.runtime.mutableStateMapOf
import androidx.compose.ui.unit.Dp
import com.forkmesh.nativeapp.market.MarketCardAction
import com.forkmesh.nativeapp.market.PackageInstallMode
import com.forkmesh.nativeapp.market.PackagesException
import com.forkmesh.nativeapp.ui.kit.FmLinearProgressIndicator
import com.forkmesh.nativeapp.ui.kit.fmSafeAreaInsets
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.feature.account.AccountFeature
import com.forkmesh.nativeapp.feature.insights.StatusPageFeature
import com.forkmesh.nativeapp.feature.navigation.NavigationFeature
import com.forkmesh.nativeapp.feature.tools.ToolsAppBar
import com.forkmesh.nativeapp.market.MarketFilter
import com.forkmesh.nativeapp.market.MarketModel
import com.forkmesh.nativeapp.market.MarketPackage
import com.forkmesh.nativeapp.market.OrgPackageState
import com.forkmesh.nativeapp.market.PackageActivityTotals
import com.forkmesh.nativeapp.market.PackageGate
import com.forkmesh.nativeapp.market.PackagePhoneSurface
import com.forkmesh.nativeapp.market.PackageReviewsModel
import com.forkmesh.nativeapp.market.QuickRate
import com.forkmesh.nativeapp.market.ReviewDraft
import com.forkmesh.nativeapp.navigation.AgentFilter
import com.forkmesh.nativeapp.navigation.CalendarMode
import com.forkmesh.nativeapp.navigation.InboxFilter
import com.forkmesh.nativeapp.navigation.Route
import com.forkmesh.nativeapp.ui.kit.FmCard
import com.forkmesh.nativeapp.ui.kit.FmCircularProgressIndicator
import com.forkmesh.nativeapp.ui.kit.FmConfirmDialog
import com.forkmesh.nativeapp.ui.kit.FmEmptyState
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmIconTile
import com.forkmesh.nativeapp.ui.kit.FmInlineStatus
import com.forkmesh.nativeapp.ui.kit.FmKeyValueRow
import com.forkmesh.nativeapp.ui.kit.FmListRow
import com.forkmesh.nativeapp.ui.kit.FmSectionHeader
import com.forkmesh.nativeapp.ui.kit.FmSegmentTab
import com.forkmesh.nativeapp.ui.kit.FmSegmentTabBar
import com.forkmesh.nativeapp.ui.kit.FmStatus
import com.forkmesh.nativeapp.ui.kit.FmStatusBadge
import com.forkmesh.nativeapp.ui.kit.FmTextField
import com.forkmesh.nativeapp.ui.kit.FmTone
import com.forkmesh.nativeapp.ui.kit.FmToneButton
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmRadius
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

private var sharedMarket: MarketModel? = null

private const val DEVICE_REMOVE_NOTE = "Removing it hides its buttons on this phone only. Settings always keeps Sign in and Sign out."
private const val DEVICE_INSTALL_NOTE = "Install it to show its buttons on this phone"
private const val DEVICE_UNINSTALL_NOTE = "Uninstalling hides it from the home menu, search and recents on this phone until you install it again here. Your organization keeps its data."

internal fun forgetSharedMarket() {
    sharedMarket = null
}

private fun marketModel(filter: MarketFilter): MarketModel? =
    (sharedMarket ?: MarketFeature.client()?.let { client ->
        MarketModel(client, filter, StatusPageFeature::installedOrNull, PackageGateFeature::deviceSwitch).apply {
            organizationCache = PackageGateFeature::organizationCached
            legacy = { PackageGateFeature.legacy }
        }
    }?.also { sharedMarket = it })?.also { it.filter = filter }

internal fun routeFor(pkg: MarketPackage): Route? = when ((pkg.phoneSurface as? PackagePhoneSurface.Native)?.destination) {
    "crm" -> Route.Crm(null)
    "marketing" -> Route.Marketing(null)
    "analytics" -> Route.Analytics(null)
    "competitors" -> Route.Competitors
    "ledger" -> Route.Ledger(null)
    "vault" -> Route.Vault(null)
    "calories" -> Route.Calories
    "glassesAlert" -> Route.GlassesAlert
    "people" -> Route.People
    "notes" -> Route.Notes
    "tasks" -> Route.Tasks(null)
    "calendar" -> Route.Calendar(null, CalendarMode.Month)
    "timers" -> Route.Timers
    "tour" -> Route.Tour
    "status" -> Route.Status
    "agents" -> Route.Agents(AgentFilter.All)
    "inbox" -> Route.Inbox(InboxFilter.All)
    "chat" -> Route.Chat
    "desktops" -> Route.Desktops
    "teammates" -> Route.Teammates
    "shortcuts" -> Route.Shortcuts(null)
    "hosts" -> Route.Hosts
    "network" -> Route.Network(null)
    "coves" -> Route.Coves(null)
    "localAI" -> Route.LocalAI
    "activity" -> Route.Activity
    "stats" -> Route.Stats
    "orgs" -> Route.Orgs
    else -> null
}

class MarketScreen(private val initialFilter: MarketFilter) : Screen() {
    override val title: String get() = "Market"

    private val model = marketModel(initialFilter)
    private var loading by mutableStateOf(true)
    private var version by mutableIntStateOf(0)
    private var reload by mutableIntStateOf(1)
    private val installing = mutableStateMapOf<String, Boolean>()
    private val failures = mutableStateMapOf<String, String>()

    @Composable
    override fun Content(navigator: Navigator) {
        val model = model ?: return
        val accountVersion by StatusPageFeature.version.collectAsState()
        val surfaces by PackageGateFeature.surfaces.collectAsState()
        val scope = rememberCoroutineScope()
        LaunchedEffect(reload) {
            loading = true
            val names = MarketFeature.organizationNamesOrNull()
            withContext(Dispatchers.IO) { model.refresh(names) }
            StatusPageFeature.rememberCatalog(model.packages)
            if (names != null && names.isNotEmpty() && model.organizationsComplete) PackageGateFeature.adoptOrganizations(model.organizations)
            StatusPageFeature.refreshAccount()
            loading = false
            version++
        }
        @Suppress("UNUSED_EXPRESSION") version
        @Suppress("UNUSED_EXPRESSION") accountVersion
        @Suppress("UNUSED_EXPRESSION") surfaces

        fun install(pkg: MarketPackage, organization: String?) {
            if (installing[pkg.id] == true) return
            failures.remove(pkg.id)
            when (model.cardAction(pkg, routeFor(pkg) != null)) {
                MarketCardAction.INSTALL_ON_PHONE -> {
                    PackageGateFeature.setDeviceSwitch(pkg.id, true)
                    version++
                    return
                }
                MarketCardAction.NEEDS_ORGANIZATION, MarketCardAction.DESKTOP -> {
                    navigator.push(PackageScreen(pkg.id))
                    return
                }
                else -> Unit
            }
            installing[pkg.id] = true
            scope.launch {
                val problem: String? = if (model.installMode(pkg) == PackageInstallMode.ACCOUNT) {
                    val store = StatusPageFeature.store
                    if (store == null) {
                        PackagesException.sentence("sign_in_required")
                    } else {
                        withContext(Dispatchers.IO) { runCatching { store.install(pkg.id) } }
                            .fold({ StatusPageFeature.adopt(it); null }, { PackagesException.describe(it) })
                    }
                } else {
                    val target = organization ?: model.installTargets(pkg).firstOrNull { !it.isInstalled(pkg.id) }?.organization
                    when {
                        target == null -> PackagesException.sentence("organization_admin_required")
                        withContext(Dispatchers.IO) { model.install(pkg, target) } -> {
                            PackageGateFeature.adoptOrganizations(model.organizations)
                            null
                        }
                        else -> model.errorText.ifEmpty { "Could not install ${pkg.name}." }
                    }
                }
                installing.remove(pkg.id)
                if (problem != null) failures[pkg.id] = problem
                version++
            }
        }

        Column(Modifier.fillMaxSize().background(Fm.colors.bgBase)) {
            ToolsAppBar("Market", navigator) {
                FmIconButton("refresh", "Refresh", onPressed = { reload++ }, color = Fm.colors.textPrimary)
            }
            if (loading && model.packages.isEmpty()) {
                Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) { FmCircularProgressIndicator() }
                return@Column
            }
            val shelves = model.shelves
            LazyColumn(
                Modifier.fillMaxSize().windowInsetsPadding(fmSafeAreaInsets(top = false, bottom = false)),
            ) {
                item(key = "search") {
                    FmTextField(
                        model.query,
                        { model.query = it; version++ },
                        Modifier.fillMaxWidth().padding(start = 16.dp, end = 16.dp, top = 8.dp, bottom = 4.dp),
                        hint = "Search packages",
                    )
                }
                item(key = "controls") {
                    MarketControls(model, version, onChanged = { version++ })
                }
                item(key = "errors") {
                    @Suppress("UNUSED_EXPRESSION") version
                    Column {
                        ErrorLine(model.errorText)
                        ErrorLine(model.partialError)
                        if (loading) FmLinearProgressIndicator(Modifier.fillMaxWidth().height(2.dp))
                    }
                }
                if (shelves.isEmpty()) {
                    item(key = "empty") {
                        FmEmptyState(
                            "shopping_bag",
                            "No packages",
                            message = when {
                                model.query.isNotBlank() -> "Nothing matches “${model.query.trim()}”."
                                model.options.activeFilters > 0 || model.filter != MarketFilter.ALL -> "No package matches these filters."
                                else -> null
                            },
                        )
                    }
                }
                shelves.forEach { shelf ->
                    if (shelf.category.isNotEmpty()) {
                        item(key = "shelf-${shelf.category}") {
                            FmSectionHeader(shelf.category, Modifier.padding(start = 16.dp, end = 16.dp, top = 12.dp, bottom = 4.dp), count = shelf.packages.size)
                        }
                    }
                    itemsIndexed(shelf.packages, key = { _, pkg -> "${shelf.category}/${pkg.id}" }) { index, pkg ->
                        if (index > 0) HorizontalDivider(thickness = Dp.Hairline, color = Fm.colors.border)
                        val route = routeFor(pkg)
                        MarketCard(
                            pkg = pkg,
                            model = model,
                            action = model.cardAction(pkg, route != null && PackageGateFeature.deviceSwitch(pkg.id) != false),
                            busy = installing[pkg.id] == true,
                            failure = failures[pkg.id],
                            onOpenPackage = { navigator.push(PackageScreen(pkg.id)) },
                            onOpen = { route?.let { navigator.push(NavigationFeature.screen(it)) } },
                            onInstall = { organization -> install(pkg, organization) },
                            version = version,
                        )
                    }
                }
                item(key = "end") {
                    Column {
                        Spacer(Modifier.windowInsetsBottomHeight(WindowInsets.navigationBars.union(WindowInsets.ime)))
                        Spacer(Modifier.height(MARKET_LIST_TAIL))
                    }
                }
            }
        }
    }
}

class PackageScreen(private val packageId: String) : Screen() {
    override val title: String get() = "Package"

    private val market = marketModel(sharedMarket?.filter ?: MarketFilter.ALL)
    private val reviews = MarketFeature.client()?.let { PackageReviewsModel(it, packageId) }
    private var version by mutableIntStateOf(0)
    private var loading by mutableStateOf(true)
    private var removeTarget by mutableStateOf<OrgPackageState?>(null)
    private var removing by mutableStateOf("")
    private var rating by mutableStateOf(false)
    private var viewReported = false
    private val orgStatus = FmStatus()
    private val rateStatus = FmStatus()

    @Composable
    override fun Content(navigator: Navigator) {
        val market = market ?: return
        val reviews = reviews ?: return
        val scope = rememberCoroutineScope()
        val context = LocalContext.current
        val saveExport = rememberLauncherForActivityResult(ActivityResultContracts.CreateDocument("application/json")) { uri: Uri? ->
            val export = market.removed?.export
            if (uri == null || export == null) return@rememberLauncherForActivityResult
            scope.launch {
                val ok = withContext(Dispatchers.IO) {
                    runCatching { context.contentResolver.openOutputStream(uri, "wt")?.use { it.write(export.toByteArray()) } != null }.getOrDefault(false)
                }
                if (ok) market.dismissRemoval()
                version++
                if (ok) orgStatus.show("Export saved.") else orgStatus.error("Could not save the export.")
            }
        }
        removeTarget?.let { target ->
            FmConfirmDialog(
                title = "Remove ${market.pkg(packageId)?.name ?: packageId} from ${target.organization}?",
                body = "Everyone in the organization loses it. Its data is deleted from the server; you get a copy to save right after.",
                confirmLabel = "Remove",
                destructive = true,
                onConfirm = {
                    val pkg = market.pkg(packageId)
                    removeTarget = null
                    if (pkg != null) {
                        removing = target.organization
                        orgStatus.clear()
                        scope.launch {
                            val ok = withContext(Dispatchers.IO) { market.uninstall(pkg, target.organization) }
                            if (ok) PackageGateFeature.adoptOrganizations(market.organizations)
                            removing = ""
                            version++
                            orgStatus.report(if (ok) "Removed from ${target.organization}" else market.errorText, !ok)
                            market.removed?.takeIf { ok && it.export != null && it.packageId == pkg.id }?.let { saveExport.launch(it.filename) }
                        }
                    }
                },
                onDismissRequest = { removeTarget = null },
            )
        }
        val accountVersion by StatusPageFeature.version.collectAsState()
        LaunchedEffect(Unit) {
            if (!viewReported) {
                viewReported = true
                MarketFeature.reportViewed(packageId)
            }
            if (market.pkg(packageId) == null) {
                val names = MarketFeature.organizationNamesOrNull()
                withContext(Dispatchers.IO) { market.refresh(names) }
                StatusPageFeature.rememberCatalog(market.packages)
            }
            withContext(Dispatchers.IO) {
                reviews.refresh()
                reviews.refreshActivity()
            }
            loading = false
            version++
        }
        val top = navigator.top
        LaunchedEffect(top) {
            if (top !== this@PackageScreen || loading || market.organizations.isNotEmpty()) return@LaunchedEffect
            if (market.pkg(packageId)?.installsPerOrganization != true) return@LaunchedEffect
            val names = MarketFeature.organizationNames()
            if (names.isEmpty()) return@LaunchedEffect
            withContext(Dispatchers.IO) { market.refresh(names) }
            version++
        }
        @Suppress("UNUSED_EXPRESSION") version
        @Suppress("UNUSED_EXPRESSION") accountVersion
        val pkg = market.pkg(packageId)
        Column(Modifier.fillMaxSize().background(Fm.colors.bgBase)) {
            ToolsAppBar(pkg?.name ?: "Package", navigator)
            when {
                loading && pkg == null -> Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) { FmCircularProgressIndicator() }
                pkg == null -> FmEmptyState("extension_off", "Package not found", message = market.errorText.ifEmpty { null })
                else -> Column(Modifier.verticalScroll(rememberScrollState()).padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    if (pkg.screenshots.isNotEmpty()) MarketScreenshotStrip(pkg.screenshots)
                    Text(pkg.summary, style = Fm.text.bodyMedium)
                    FmCard(Modifier.fillMaxWidth()) {
                        Column {
                            if (pkg.version.isNotEmpty()) FmKeyValueRow("Version", pkg.version)
                            if (pkg.category.isNotEmpty()) FmKeyValueRow("Category", pkg.category)
                            FmKeyValueRow("Price", if (pkg.free) "Free" else "Paid")
                            market.counts[pkg.id]?.let { FmKeyValueRow("Installs", "${it.installs} · ${it.downloads} downloads") }
                            FmKeyValueRow("Rating", (reviews.page?.summary ?: market.ratings[pkg.id])?.label ?: "No ratings yet")
                            reviews.page?.let { page ->
                                FmKeyValueRow("Views", page.activity.viewsLabel)
                                FmKeyValueRow("Opens", page.activity.opens.toString())
                                if (page.viewer.signedIn) FmKeyValueRow("You", PackageActivityTotals.youLabel(page.viewer))
                            }
                            FmKeyValueRow(
                                "On phone",
                                when {
                                    pkg.accountSync && pkg.phoneSurface is PackagePhoneSurface.Native -> "On this phone; follows your account on every device"
                                    pkg.accountSync -> "Follows your account on every device"
                                    PackageGateFeature.deviceSwitch(pkg.id) != null -> "On this phone; switched per phone"
                                    else -> when (pkg.phoneSurface) {
                                        is PackagePhoneSurface.Native -> "Built into the app"
                                        is PackagePhoneSurface.Web -> "Opens on the web"
                                        PackagePhoneSurface.WorldOnly -> "World only"
                                        PackagePhoneSurface.DesktopOnly -> "Desktop only"
                                    }
                                },
                            )
                        }
                    }
                    QuickRateRow(reviews, rating, rateStatus) { stars ->
                        rateStatus.clear()
                        rating = true
                        scope.launch {
                            val result = withContext(Dispatchers.IO) { reviews.quickRate(stars) }
                            rating = false
                            version++
                            when (result) {
                                QuickRate.Submitted -> rateStatus.show("You rated it $stars ${if (stars == 1) "star" else "stars"}.")
                                QuickRate.NeedsReview -> navigator.push(RateScreen(reviews))
                                is QuickRate.Failed -> rateStatus.error(result.message)
                            }
                        }
                    }
                    val onPhone = PackageGateFeature.deviceSwitch(pkg.id)
                    val route = routeFor(pkg)
                    val surface = pkg.phoneSurface
                    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        if (route != null && onPhone != false && (!pkg.accountSync || market.isInstalled(pkg))) FmToneButton("Open", onPressed = { navigator.push(NavigationFeature.screen(route)) }, tone = FmTone.Accent, filled = true)
                        if (surface is PackagePhoneSurface.Web) {
                            FmToneButton("Open on the web", onPressed = {
                                val origin = com.forkmesh.nativeapp.core.Services.get<com.forkmesh.nativeapp.api.ApiService>()?.gitOrigin ?: return@FmToneButton
                                NavigationFeature.openInBrowser(context, origin + surface.route)
                            })
                        }
                    }
                    onPhone?.let { on ->
                        FmListRow(
                            if (on) "Installed on this phone" else "Not installed on this phone",
                            subtitle = when {
                                !on -> DEVICE_INSTALL_NOTE
                                pkg.id == PackageGate.SIGN_IN -> DEVICE_REMOVE_NOTE
                                else -> DEVICE_UNINSTALL_NOTE
                            },
                            leading = { FmIcon("smartphone", tint = if (on) Fm.colors.success else Fm.colors.textTertiary) },
                            showChevron = false,
                        )
                        FmToneButton(
                            if (on) "Uninstall from this phone" else "Install on this phone",
                            onPressed = {
                                PackageGateFeature.setDeviceSwitch(pkg.id, !on)
                                version++
                            },
                            tone = if (on) FmTone.Danger else FmTone.Accent,
                            filled = !on,
                        )
                    }
                    if (pkg.accountSync) AccountPackageSections(pkg, market.nested(pkg))
                    val targets = market.installTargets(pkg)
                    if (pkg.installsPerOrganization) {
                        FmSectionHeader("Organizations", Modifier.padding(top = 8.dp), count = market.organizations.size)
                        if (market.organizations.isEmpty()) {
                            Text("Join or create an organization to install this package.", style = Fm.text.bodySmall, color = Fm.colors.textTertiary)
                            FmToneButton("Create an organization", onPressed = { navigator.push(AccountFeature.orgsScreen()) }, modifier = Modifier.padding(top = 8.dp), tone = FmTone.Accent)
                        }
                        market.organizations.forEach { org ->
                            val installed = org.isInstalled(pkg.id)
                            FmListRow(
                                org.organization,
                                subtitle = if (installed) "Installed" else if (org.canManage) "Not installed" else "Ask an owner or admin to install it",
                                trailing = when {
                                    removing == org.organization -> ({ FmCircularProgressIndicator(Modifier.size(20.dp)) })
                                    installed && org in targets -> ({
                                        FmToneButton("Remove", onPressed = { removeTarget = org }, tone = FmTone.Danger)
                                    })
                                    !installed && org in targets -> ({
                                        FmToneButton("Install", onPressed = {
                                            orgStatus.clear()
                                            scope.launch {
                                                val ok = withContext(Dispatchers.IO) { market.install(pkg, org.organization) }
                                                if (ok) PackageGateFeature.adoptOrganizations(market.organizations)
                                                version++
                                                orgStatus.report(if (ok) "Installed for ${org.organization}" else market.errorText, !ok)
                                            }
                                        }, tone = FmTone.Accent)
                                    })
                                    else -> null
                                },
                                showChevron = false,
                            )
                        }
                        FmInlineStatus(orgStatus)
                        market.removed?.takeIf { it.packageId == pkg.id && it.export != null }?.let { removal ->
                            Text(
                                "The data removed from ${removal.organization} is only on this phone until you save it.",
                                style = Fm.text.bodySmall,
                                color = Fm.colors.textTertiary,
                            )
                            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                                FmToneButton("Save the exported data", onPressed = { saveExport.launch(removal.filename) }, tone = FmTone.Accent, filled = true)
                                FmToneButton("Discard", onPressed = {
                                    market.dismissRemoval()
                                    version++
                                })
                            }
                        }
                    }
                    if (pkg.permissions.isNotEmpty()) {
                        FmSectionHeader("Permissions", Modifier.padding(top = 8.dp), count = pkg.permissions.size)
                        pkg.permissions.forEach { permission ->
                            FmListRow(
                                permission.label.ifEmpty { permission.id },
                                subtitle = permission.risk,
                                leading = { FmIcon(if (permission.risk == "high") "warning" else "lock", tint = if (permission.risk == "high") Fm.colors.danger else Fm.colors.textTertiary) },
                                showChevron = false,
                            )
                        }
                    }
                    if (reviews.canSeeActivity) ActivitySection(reviews)
                    ReviewsSection(reviews, onChanged = { version++ }, onRate = { navigator.push(RateScreen(reviews)) })
                }
            }
        }
    }
}

@Composable
private fun QuickRateRow(model: PackageReviewsModel, saving: Boolean, status: FmStatus, onStar: (Int) -> Unit) {
    val viewer = model.page?.viewer ?: return
    if (!viewer.canRate) {
        Text(viewer.cannotRateReason, style = Fm.text.bodySmall, color = Fm.colors.textTertiary)
        return
    }
    val mine = viewer.review?.stars ?: 0
    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(2.dp)) {
        Text(if (mine > 0) "Your rating" else "Tap to rate", style = Fm.text.bodySmall, color = Fm.colors.textSecondary, modifier = Modifier.padding(end = 6.dp))
        (1..5).forEach { star ->
            FmIconButton(
                if (star <= mine) "star" else "star_border",
                "Rate $star ${if (star == 1) "star" else "stars"}",
                onPressed = if (saving) null else ({ onStar(star) }),
                color = Fm.colors.warning,
                iconSize = 28.dp,
            )
        }
        if (saving) FmCircularProgressIndicator(Modifier.size(18.dp))
    }
    FmInlineStatus(status)
}

@Composable
private fun ActivitySection(model: PackageReviewsModel) {
    val activity = model.activity
    FmSectionHeader("Who opened it", Modifier.padding(top = 8.dp), count = activity?.totals?.people)
    ErrorLine(model.activityError)
    if (activity == null) return
    if (activity.people.isEmpty()) Text("No one has opened it yet.", style = Fm.text.bodySmall, color = Fm.colors.textTertiary)
    activity.people.forEach { person ->
        FmListRow(
            person.name,
            subtitle = listOfNotNull(person.label, shortDate(person.lastAt).ifEmpty { null }?.let { "last $it" }).joinToString(" · "),
            leading = { FmIcon("person", tint = Fm.colors.textTertiary) },
            showChevron = false,
        )
    }
    if (activity.hasMore) Text("Showing the ${activity.people.size} most active people.", style = Fm.text.bodySmall, color = Fm.colors.textTertiary)
}

@Composable
private fun ReviewsSection(model: PackageReviewsModel, onChanged: () -> Unit, onRate: () -> Unit) {
    val scope = rememberCoroutineScope()
    val page = model.page
    FmSectionHeader("Reviews", Modifier.padding(top = 8.dp), count = page?.total, trailing = {
        if (page?.viewer?.canRate == true) FmToneButton(if (page.viewer.review != null) "Edit your review" else "Rate", onPressed = onRate, tone = FmTone.Accent)
    })
    ErrorLine(model.errorText)
    page?.reviews?.forEach { review ->
        FmCard(Modifier.fillMaxWidth().padding(vertical = 4.dp)) {
            Column(Modifier.padding(12.dp)) {
                Text("★".repeat(review.stars) + "☆".repeat(5 - review.stars) + "  " + review.reviewerLabel, style = Fm.text.bodyMedium)
                if (review.verifiedUse) Text("Verified use", style = Fm.text.labelSmall, color = Fm.colors.success)
                listOf("Problem" to review.problem, "How to improve" to review.improvement, "Context" to review.context, "Comment" to review.comment)
                    .filter { it.second.isNotEmpty() }
                    .forEach { (label, text) ->
                        Text(label, style = Fm.text.labelSmall, color = Fm.colors.textTertiary, modifier = Modifier.padding(top = 4.dp))
                        Text(text, style = Fm.text.bodySmall)
                    }
                review.reply?.let { Text("Reply from @${it.by}: ${it.text}", Modifier.padding(top = 6.dp), style = Fm.text.bodySmall, color = Fm.colors.accent) }
                if (review.addressedIn.isNotEmpty()) Text("Addressed in ${review.addressedIn}", style = Fm.text.labelSmall, color = Fm.colors.success)
                FmToneButton(
                    "Helpful · ${review.helpful}",
                    onPressed = { scope.launch { withContext(Dispatchers.IO) { model.toggleHelpful(review) }; onChanged() } },
                    tone = if (review.viewerFoundHelpful) FmTone.Accent else FmTone.Neutral,
                    modifier = Modifier.padding(top = 6.dp),
                )
            }
        }
    }
}

class RateScreen(private val model: PackageReviewsModel) : Screen() {
    override val title: String get() = "Rate"

    private var draft by mutableStateOf(model.draft)
    private var saving by mutableStateOf(false)
    private var version by mutableIntStateOf(0)

    @Composable
    override fun Content(navigator: Navigator) {
        val scope = rememberCoroutineScope()
        @Suppress("UNUSED_EXPRESSION") version
        model.draft = draft
        val rules = model.rules
        val issues = model.fieldIssues
        Column(Modifier.fillMaxSize().background(Fm.colors.bgBase)) {
            ToolsAppBar("Rate ${model.packageId}", navigator) {
                FmToneButton(if (saving) "Saving…" else "Submit", onPressed = if (!saving && model.canSubmit) ({
                    saving = true
                    scope.launch {
                        val ok = withContext(Dispatchers.IO) { model.submit() }
                        saving = false
                        version++
                        if (ok) navigator.pop()
                    }
                }) else null, tone = FmTone.Accent, filled = true)
            }
            Column(Modifier.verticalScroll(rememberScrollState()).padding(16.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
                ErrorLine(model.errorText)
                Row(horizontalArrangement = Arrangement.spacedBy(4.dp)) {
                    (1..5).forEach { star ->
                        FmIconButton(if (star <= draft.stars) "star" else "star_border", "$star stars", onPressed = { draft = draft.copy(stars = star) }, color = Fm.colors.warning, iconSize = 30.dp)
                    }
                }
                if (draft.needsReview(rules)) Text("Below ${rules.reviewRequiredBelow} stars, say what didn't work and how to improve it.", style = Fm.text.bodySmall, color = Fm.colors.textTertiary)
                FmTextField(draft.problem, { draft = draft.copy(problem = it) }, hint = "What didn't work", minLines = 3, maxLines = 8, errorText = issues["problem"]?.let(ReviewDraft::fieldMessage))
                FmTextField(draft.improvement, { draft = draft.copy(improvement = it) }, hint = "How to improve it", minLines = 3, maxLines = 8, errorText = issues["improvement"]?.let(ReviewDraft::fieldMessage))
                FmTextField(draft.context, { draft = draft.copy(context = it) }, hint = "Context (optional)", errorText = issues["context"]?.let(ReviewDraft::fieldMessage))
                FmTextField(draft.comment, { draft = draft.copy(comment = it) }, hint = "Comment (optional)", minLines = 2, maxLines = 6, errorText = issues["comment"]?.let(ReviewDraft::fieldMessage))
                FmSectionHeader("Category")
                Row(horizontalArrangement = Arrangement.spacedBy(6.dp), modifier = Modifier.fillMaxWidth()) {
                    rules.categories.take(3).forEach { category ->
                        FmToneButton(category.label, onPressed = { draft = draft.copy(category = if (draft.category == category.id) "" else category.id) }, tone = if (draft.category == category.id) FmTone.Accent else FmTone.Neutral)
                    }
                }
                Row(horizontalArrangement = Arrangement.spacedBy(6.dp), modifier = Modifier.fillMaxWidth()) {
                    rules.categories.drop(3).forEach { category ->
                        FmToneButton(category.label, onPressed = { draft = draft.copy(category = if (draft.category == category.id) "" else category.id) }, tone = if (draft.category == category.id) FmTone.Accent else FmTone.Neutral)
                    }
                }
                Spacer(Modifier.height(24.dp))
            }
        }
    }
}
