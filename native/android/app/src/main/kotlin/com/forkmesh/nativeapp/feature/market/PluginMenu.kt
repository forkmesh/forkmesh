package com.forkmesh.nativeapp.feature.market

import androidx.compose.foundation.layout.Box
import androidx.compose.material3.HorizontalDivider
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.unit.Dp
import com.forkmesh.nativeapp.feature.insights.StatusPageFeature
import com.forkmesh.nativeapp.feature.tools.ReportBugSheet
import com.forkmesh.nativeapp.market.MarketModel
import com.forkmesh.nativeapp.market.MarketUninstall
import com.forkmesh.nativeapp.market.PackageMenu
import com.forkmesh.nativeapp.market.PackageReviewsModel
import com.forkmesh.nativeapp.ui.kit.FmConfirmDialog
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenu
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenuItem
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.navigation.LocalNavigator
import com.forkmesh.nativeapp.ui.navigation.LocalScreen
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

internal class PluginMenu(val packageId: String, private val navigator: Navigator, private val screen: Screen?) {
    val market: MarketModel? = currentMarketModel()
    private val reviews: PackageReviewsModel? = MarketFeature.client()?.let { PackageReviewsModel(it, packageId) }
    internal var uninstaller: PackageUninstaller? = null
    internal var signedIn = false
    private var version by mutableIntStateOf(0)
    private var loading by mutableStateOf(false)
    private var loaded = false
    private var feedback by mutableStateOf(false)
    private var problem by mutableStateOf<String?>(null)

    val name: String
        get() {
            @Suppress("UNUSED_EXPRESSION") version
            return PackageMenu.name(packageId, market?.pkg(packageId)?.name ?: StatusPageFeature.store?.catalog?.get(packageId)?.name)
        }

    private suspend fun load() {
        if (loaded) return
        loaded = true
        loading = true
        val market = market
        if (market != null) {
            if (market.pkg(packageId) == null) {
                val names = MarketFeature.organizationNamesOrNull()
                withContext(Dispatchers.IO) { market.refresh(names) }
                StatusPageFeature.rememberCatalog(market.packages)
            }
            if (market.pkg(packageId)?.installsPerOrganization == true && market.organizations.isEmpty()) {
                val names = MarketFeature.organizationNames()
                if (names.isNotEmpty()) withContext(Dispatchers.IO) { market.refresh(names) }
            }
        }
        reviews?.let { withContext(Dispatchers.IO) { it.refresh() } }
        loading = false
        version++
    }

    internal fun finished(outcome: UninstallOutcome) {
        version++
        if (outcome.problem != null) {
            problem = outcome.problem
            return
        }
        val exportPending = outcome.request.action is MarketUninstall.Organization &&
            market?.removed?.let { it.packageId == packageId && it.export != null } == true
        if (!exportPending) leave()
    }

    internal fun exportSaved(ok: Boolean) {
        if (ok) leave() else problem = "Could not save the export. Open ${name} in the Market to save it."
    }

    private fun leave() {
        if (screen != null && navigator.top === screen) navigator.pop()
    }

    @Composable
    fun Items(close: () -> Unit, divided: Boolean = false) {
        LaunchedEffect(Unit) { load() }
        @Suppress("UNUSED_EXPRESSION") version
        val label = name
        if (divided) MenuDivider()
        reviews?.let { model ->
            FmDropdownMenuItem(
                PackageMenu.rateLabel(label, model.page?.viewer?.review != null),
                onClick = {
                    close()
                    navigator.push(RateScreen(model))
                },
                icon = "star",
            )
        }
        FmDropdownMenuItem(PackageMenu.FEEDBACK, onClick = { close(); feedback = true }, icon = "feedback")
        val requests = market?.let { model ->
            model.pkg(packageId)?.let { pkg -> model.uninstallAction(pkg, signedIn)?.let { PackageUninstall.requests(pkg, it) } }
        }.orEmpty()
        val waiting = loading && requests.isEmpty()
        if (requests.isNotEmpty() || waiting) MenuDivider()
        if (waiting) FmDropdownMenuItem(PackageMenu.uninstallLabel(null), onClick = {}, icon = "delete", enabled = false)
        requests.forEach { request ->
            FmDropdownMenuItem(
                PackageMenu.uninstallLabel(request.organization),
                onClick = {
                    close()
                    uninstaller?.ask(request)
                },
                icon = "delete",
                enabled = uninstaller?.working == null,
                textColor = Fm.colors.danger,
            )
        }
    }

    @Composable
    fun Host() {
        uninstaller?.Dialog()
        if (feedback) {
            val label = name
            ReportBugSheet(screen = screen?.title ?: label, packageId = packageId, packageName = label) { feedback = false }
        }
        problem?.let { message ->
            FmConfirmDialog(
                title = "Could not uninstall $name",
                body = message,
                onConfirm = { problem = null },
                onDismissRequest = { problem = null },
                hideCancel = true,
            )
        }
    }
}

@Composable
private fun MenuDivider() {
    HorizontalDivider(thickness = Dp.Hairline, color = Fm.colors.border)
}

@Composable
internal fun rememberPluginMenu(packageId: String, navigator: Navigator, screen: Screen?): PluginMenu {
    val menu = remember(packageId, navigator, screen) { PluginMenu(packageId, navigator, screen) }
    val market = menu.market
    if (market != null) {
        val saveExport = rememberExportSaver(market) { ok -> menu.exportSaved(ok) }
        menu.uninstaller = rememberPackageUninstaller(market, saveExport) { outcome -> menu.finished(outcome) }
    }
    menu.signedIn = rememberPackagesSignedIn()
    return menu
}

@Composable
internal fun rememberScreenPluginMenu(): PluginMenu? {
    val screen = LocalScreen.current
    val packageId = screen?.pluginPackage ?: return null
    return rememberPluginMenu(packageId, LocalNavigator.current, screen)
}

@Composable
internal fun PluginMoreMenu(menu: PluginMenu) {
    var open by remember { mutableStateOf(false) }
    Box {
        FmIconButton("more_vert", PackageMenu.MORE, onPressed = { open = true }, color = Fm.colors.textPrimary)
        FmDropdownMenu(expanded = open, onDismissRequest = { open = false }) {
            menu.Items(close = { open = false })
        }
    }
    menu.Host()
}

@Composable
internal fun PluginMenuButton() {
    val menu = rememberScreenPluginMenu() ?: return
    PluginMoreMenu(menu)
}
