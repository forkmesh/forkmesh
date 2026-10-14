package com.forkmesh.nativeapp.feature.market

import com.forkmesh.nativeapp.App
import com.forkmesh.nativeapp.account.projects.ProjectScope
import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.api.PrefStore
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.account.AuthService
import com.forkmesh.nativeapp.feature.insights.StatusPageFeature
import com.forkmesh.nativeapp.market.OrgPackageState
import com.forkmesh.nativeapp.market.PackageGate
import com.forkmesh.nativeapp.market.PackageSurfaces
import com.forkmesh.nativeapp.market.PackagesText
import com.forkmesh.nativeapp.market.PhonePackages
import com.forkmesh.nativeapp.platform.AppLifecycle
import com.forkmesh.nativeapp.platform.ProjectLifecycle
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.drop
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

object PackageGateFeature {
    private const val ORGANIZATION_KEY = "packages/organization/installed"
    private const val ORGANIZATION_REFRESH_MS = 10 * 60 * 1000L

    private val surfacesState = MutableStateFlow(PackageSurfaces.ALL)

    val surfaces: StateFlow<PackageSurfaces> = surfacesState.asStateFlow()

    @Volatile
    var legacy: Boolean = true
        private set

    @Volatile
    private var prefs: PrefStore? = null

    @Volatile
    private var organizationInstalled: Set<String> = emptySet()

    @Volatile
    private var organizationsAt = 0L

    fun projectPrefs(): PrefStore? {
        val base = Services.get<PrefStore>() ?: return null
        return Services.get<ProjectScope>()?.prefs(base) ?: base
    }

    fun install(@Suppress("UNUSED_PARAMETER") app: App) {
        val store = projectPrefs()
        prefs = store
        val auth = Services.get<AuthService>()
        legacy = PackageGate.legacyProject(store) { auth?.hasAppAccess?.value == true }
        organizationInstalled = store?.getString(ORGANIZATION_KEY).orEmpty().split(',').filter(PackagesText::isPackageId).toSet()
        organizationsAt = 0L
        publish()
        val scope = ProjectLifecycle.scope
        scope.launch {
            AppLifecycle.foreground.collect { front ->
                if (front && System.currentTimeMillis() - organizationsAt > ORGANIZATION_REFRESH_MS) refreshOrganizations()
            }
        }
        auth?.session?.let { session ->
            scope.launch { session.map { it?.nodeName.orEmpty() }.distinctUntilChanged().drop(1).collect { refreshOrganizations() } }
        }
        ProjectLifecycle.onClose {
            prefs = null
            organizationInstalled = emptySet()
            surfacesState.value = PackageSurfaces.ALL
        }
    }

    fun isInstalled(id: String): Boolean = when (id) {
        PackageGate.SIGN_IN -> PhonePackages.isOn(id, prefs, legacy) ?: true
        in PackageGate.ORGANIZATION_PACKAGES -> id in organizationInstalled && PhonePackages.isOn(id, prefs, legacy) != false
        else -> StatusPageFeature.store?.shows(id) ?: PackageGate.fallback(id, null, legacy)
    }

    fun deviceSwitch(id: String): Boolean? = PhonePackages.isOn(id, prefs, legacy)

    fun organizationCached(id: String): Boolean? = if (id in PackageGate.ORGANIZATION_PACKAGES) id in organizationInstalled else null

    fun setDeviceSwitch(id: String, on: Boolean) {
        PhonePackages.set(id, on, prefs)
        publish()
    }

    fun adoptOrganizations(states: List<OrgPackageState>) {
        if (states.isEmpty() && organizationInstalled.isNotEmpty()) return
        val next = PackageGate.organizationInstalled(states)
        organizationsAt = System.currentTimeMillis()
        if (next == organizationInstalled) return
        organizationInstalled = next
        prefs?.putString(ORGANIZATION_KEY, next.sorted().joinToString(","))
        publish()
    }

    suspend fun refreshOrganizations() {
        val api = Services.get<ApiService>() ?: return
        val client = MarketFeature.client() ?: return
        if (Services.get<AuthService>()?.session?.value == null) {
            organizationsAt = System.currentTimeMillis()
            if (organizationInstalled.isNotEmpty()) {
                organizationInstalled = emptySet()
                prefs?.remove(ORGANIZATION_KEY)
                publish()
            }
            return
        }
        val states = withContext(Dispatchers.IO) {
            val names = runCatching { api.myOrgs().map { it.name } }.getOrNull() ?: return@withContext null
            names.filter(PackagesText::isOrgName).take(20).map { name ->
                runCatching { client.organizationPackages(name) }.getOrNull() ?: return@withContext null
            }
        } ?: return
        organizationsAt = System.currentTimeMillis()
        val next = PackageGate.organizationInstalled(states)
        if (next == organizationInstalled) return
        organizationInstalled = next
        prefs?.putString(ORGANIZATION_KEY, next.sorted().joinToString(","))
        publish()
    }

    fun publish() {
        surfacesState.value = PackageSurfaces.of(::isInstalled)
    }
}
