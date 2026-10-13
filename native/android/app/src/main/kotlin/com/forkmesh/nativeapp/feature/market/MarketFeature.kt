package com.forkmesh.nativeapp.feature.market

import com.forkmesh.nativeapp.App
import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.control.ControlFamilyChannel
import com.forkmesh.nativeapp.control.DesktopControlFamilyName
import com.forkmesh.nativeapp.control.HubControlCalling
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.agents.AgentsFeature
import com.forkmesh.nativeapp.feature.navigation.NavigationFeature
import com.forkmesh.nativeapp.market.ActivityKind
import com.forkmesh.nativeapp.market.ApiPackagesTransport
import com.forkmesh.nativeapp.market.CrmContactRef
import com.forkmesh.nativeapp.market.CrmModel
import com.forkmesh.nativeapp.market.LedgerSection
import com.forkmesh.nativeapp.market.MarketFilter
import com.forkmesh.nativeapp.market.MarketingSection
import com.forkmesh.nativeapp.market.PackageActivityReporter
import com.forkmesh.nativeapp.market.PackageRoutes
import com.forkmesh.nativeapp.market.PackagesClient
import com.forkmesh.nativeapp.market.PersonalLedgerStore
import com.forkmesh.nativeapp.market.WalletClient
import com.forkmesh.nativeapp.navigation.Route
import com.forkmesh.nativeapp.platform.ProjectLifecycle
import com.forkmesh.nativeapp.security.SecretStore
import com.forkmesh.nativeapp.ui.navigation.Screen
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch

object MarketFeature {
    private const val OWNER = "WS19"
    private var crmModel: CrmModel? = null

    private val activity = PackageActivityReporter({ client() })

    fun client(): PackagesClient? = Services.get<ApiService>()?.let { PackagesClient(ApiPackagesTransport(it)) }

    fun reportViewed(packageId: String) {
        ProjectLifecycle.scope.launch(Dispatchers.IO) { activity.report(packageId, ActivityKind.VIEW) }
    }

    fun reportOpened(kind: Route.Kind) {
        ProjectLifecycle.scope.launch(Dispatchers.IO) { activity.opened(kind) }
    }

    fun ledgerStore(): PersonalLedgerStore? = Services.get<SecretStore>()?.let(::PersonalLedgerStore)

    fun wallet(): WalletClient? = runCatching {
        WalletClient(ControlFamilyChannel(DesktopControlFamilyName.Wallet, HubControlCalling(AgentsFeature.hub)))
    }.getOrNull()

    internal fun crm(): CrmModel? = crmModel ?: client()?.let { CrmModel(it) }?.also { crmModel = it }

    suspend fun organizationNames(): List<String> =
        runCatching { Services.get<ApiService>()?.myOrgs()?.map { it.name } }.getOrNull() ?: emptyList()

    suspend fun organizationNamesOrNull(): List<String>? =
        runCatching { Services.get<ApiService>()?.myOrgs()?.map { it.name } }.getOrNull()

    fun install(@Suppress("UNUSED_PARAMETER") app: App) {
        crmModel = null
        forgetSharedMarket()
        ProjectLifecycle.onClose {
            crmModel?.forget()
            crmModel = null
            forgetSharedMarket()
        }
        register()
    }

    private fun register() {
        NavigationFeature.register(Route.Kind.Market, OWNER) { route -> marketScreen(MarketFilter.of((route as Route.Market).filter)) }
        NavigationFeature.register(Route.Kind.Package, OWNER) { route -> packageScreen((route as Route.Package).id) }
        NavigationFeature.register(Route.Kind.Crm, OWNER) { route ->
            val target = PackageRoutes.organizationFromPipeline((route as Route.Crm).pipeline)
            crmScreen(target.organization, target.flow)
        }
        NavigationFeature.register(Route.Kind.CrmOrg, OWNER) { route -> crmScreen((route as Route.CrmOrg).id, null) }
        NavigationFeature.register(Route.Kind.Contact, OWNER) { route ->
            CrmContactRef.of((route as Route.Contact).id)?.let { contactScreen(it) } ?: crmScreen(null, null)
        }
        NavigationFeature.register(Route.Kind.Marketing, OWNER) { route -> marketingScreen(MarketingSection.of((route as Route.Marketing).section)) }
        NavigationFeature.register(Route.Kind.Analytics, OWNER) { analyticsScreen() }
        NavigationFeature.register(Route.Kind.Competitors, OWNER) { competitorsScreen() }
        NavigationFeature.register(Route.Kind.Ads, OWNER) { adsScreen() }
        NavigationFeature.register(Route.Kind.Ledger, OWNER) { route -> ledgerScreen(LedgerSection.of((route as Route.Ledger).section)) }
    }

    fun marketScreen(filter: MarketFilter = MarketFilter.ALL): Screen = MarketScreen(filter)

    fun packageScreen(id: String): Screen = PackageScreen(id)

    fun crmScreen(organization: String? = null, flow: String? = null): Screen = CrmScreen(organization, flow)

    fun contactScreen(ref: CrmContactRef): Screen = ContactScreen(ref)

    fun marketingScreen(section: MarketingSection = MarketingSection.PLAN): Screen = MarketingScreen(section)

    fun analyticsScreen(): Screen = AnalyticsScreen()

    fun competitorsScreen(): Screen = CompetitorsScreen()

    fun adsScreen(): Screen = AdsScreen()

    fun ledgerScreen(section: LedgerSection = LedgerSection.RECORDS): Screen = LedgerScreen(section)
}
