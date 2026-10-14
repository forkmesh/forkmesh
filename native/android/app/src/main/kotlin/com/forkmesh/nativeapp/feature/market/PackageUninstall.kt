package com.forkmesh.nativeapp.feature.market

import android.net.Uri
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.setValue
import androidx.compose.ui.platform.LocalContext
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.account.AuthService
import com.forkmesh.nativeapp.feature.insights.StatusPageFeature
import com.forkmesh.nativeapp.market.AccountCatalogEntry
import com.forkmesh.nativeapp.market.AccountPackageGraph
import com.forkmesh.nativeapp.market.MarketModel
import com.forkmesh.nativeapp.market.MarketPackage
import com.forkmesh.nativeapp.market.MarketUninstall
import com.forkmesh.nativeapp.market.PackageGate
import com.forkmesh.nativeapp.market.PackagesException
import com.forkmesh.nativeapp.model.auth.AuthSession
import com.forkmesh.nativeapp.ui.kit.FmConfirmDialog
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

internal const val DEVICE_REMOVE_NOTE = "Removing it hides its buttons on this phone only. Settings always keeps Sign in and Sign out."
private const val PHONE_UNINSTALL_NOTE = "It disappears from your home menu, search and recents on this phone until you install it again here. Your data is kept."
private const val ORGANIZATION_UNINSTALL_NOTE = "Everyone in the organization loses it. Its data is deleted from the server; you get a copy to save right after."

internal data class UninstallRequest(val pkg: MarketPackage, val action: MarketUninstall, val organization: String? = null)

internal data class UninstallOutcome(val request: UninstallRequest, val problem: String?, val note: String)

@Composable
internal fun rememberPackagesSignedIn(): Boolean {
    val sessionFlow: StateFlow<AuthSession?> = remember { Services.get<AuthService>()?.session ?: MutableStateFlow(null) }
    val guestFlow: StateFlow<Boolean> = remember { Services.get<AuthService>()?.isGuest ?: MutableStateFlow(true) }
    val session by sessionFlow.collectAsState()
    val guest by guestFlow.collectAsState()
    return session != null && !guest
}

internal object PackageUninstall {
    private fun accountEntry(pkg: MarketPackage): AccountCatalogEntry =
        StatusPageFeature.store?.catalog?.get(pkg.id) ?: AccountCatalogEntry.of(pkg)

    fun title(request: UninstallRequest): String = when (request.action) {
        MarketUninstall.Account ->
            "Remove ${request.pkg.name} ${if (StatusPageFeature.store?.perProject == true) "from this project" else "from all your devices"}?"
        MarketUninstall.Phone -> "Uninstall ${request.pkg.name} from this phone?"
        is MarketUninstall.Organization -> "Remove ${request.pkg.name} from ${request.organization}?"
    }

    fun body(request: UninstallRequest): String = when (request.action) {
        MarketUninstall.Account -> {
            val store = StatusPageFeature.store
            val entry = accountEntry(request.pkg)
            if (store == null) {
                ""
            } else {
                AccountPackageGraph.removeNote(entry, store.catalog.values.ifEmpty { listOf(entry) }, store.perProject, store::isInstalled)
            }
        }
        MarketUninstall.Phone -> if (request.pkg.id == PackageGate.SIGN_IN) DEVICE_REMOVE_NOTE else PHONE_UNINSTALL_NOTE
        is MarketUninstall.Organization -> ORGANIZATION_UNINSTALL_NOTE
    }

    fun requests(pkg: MarketPackage, action: MarketUninstall): List<UninstallRequest> = when (action) {
        is MarketUninstall.Organization -> action.organizations.map { UninstallRequest(pkg, action, it) }
        else -> listOf(UninstallRequest(pkg, action))
    }

    suspend fun perform(request: UninstallRequest, model: MarketModel): UninstallOutcome {
        val pkg = request.pkg
        return when (request.action) {
            MarketUninstall.Account -> {
                val store = StatusPageFeature.store ?: return UninstallOutcome(request, PackagesException.sentence("sign_in_required"), "")
                val entry = accountEntry(pkg)
                withContext(Dispatchers.IO) { runCatching { store.uninstall(pkg.id) } }.fold(
                    { state ->
                        StatusPageFeature.adopt(state)
                        UninstallOutcome(request, null, AccountPackageGraph.changedNote(false, entry, state.changed, store.catalog.values, store.perProject))
                    },
                    { UninstallOutcome(request, PackagesException.describe(it), "") },
                )
            }
            MarketUninstall.Phone -> {
                PackageGateFeature.setDeviceSwitch(pkg.id, false)
                UninstallOutcome(request, null, "Uninstalled ${pkg.name} from this phone.")
            }
            is MarketUninstall.Organization -> {
                val organization = request.organization ?: return UninstallOutcome(request, PackagesException.sentence("organization_admin_required"), "")
                if (withContext(Dispatchers.IO) { model.uninstall(pkg, organization) }) {
                    PackageGateFeature.adoptOrganizations(model.organizations)
                    UninstallOutcome(request, null, "Removed from $organization")
                } else {
                    UninstallOutcome(request, model.errorText.ifEmpty { "Could not remove ${pkg.name} from $organization." }, "")
                }
            }
        }
    }
}

internal class PackageUninstaller(
    private val model: MarketModel,
    private val scope: CoroutineScope,
    private val saveExport: () -> ((String) -> Unit),
    private val onDone: () -> ((UninstallOutcome) -> Unit),
) {
    var pending by mutableStateOf<UninstallRequest?>(null)
    var working by mutableStateOf<UninstallRequest?>(null)
        private set

    fun ask(request: UninstallRequest) {
        if (working == null) pending = request
    }

    fun isWorking(pkg: MarketPackage): Boolean = working?.pkg?.id == pkg.id

    private fun run(request: UninstallRequest) {
        working = request
        scope.launch {
            val outcome = PackageUninstall.perform(request, model)
            working = null
            onDone()(outcome)
            if (outcome.problem == null && request.action is MarketUninstall.Organization) {
                model.removed?.takeIf { it.packageId == request.pkg.id && it.export != null }?.let { saveExport()(it.filename) }
            }
        }
    }

    @Composable
    fun Dialog() {
        val request = pending ?: return
        FmConfirmDialog(
            title = PackageUninstall.title(request),
            body = PackageUninstall.body(request).ifEmpty { null },
            confirmLabel = "Uninstall",
            destructive = true,
            onConfirm = {
                pending = null
                run(request)
            },
            onDismissRequest = { pending = null },
        )
    }
}

@Composable
internal fun rememberExportSaver(model: MarketModel, onSaved: (Boolean) -> Unit): (String) -> Unit {
    val scope = rememberCoroutineScope()
    val context = LocalContext.current
    val saved by rememberUpdatedState(onSaved)
    val launcher = rememberLauncherForActivityResult(ActivityResultContracts.CreateDocument("application/json")) { uri: Uri? ->
        val export = model.removed?.export
        if (uri == null || export == null) return@rememberLauncherForActivityResult
        scope.launch {
            val ok = withContext(Dispatchers.IO) {
                runCatching { context.contentResolver.openOutputStream(uri, "wt")?.use { it.write(export.toByteArray()) } != null }.getOrDefault(false)
            }
            if (ok) model.dismissRemoval()
            saved(ok)
        }
    }
    return { filename -> launcher.launch(filename) }
}

@Composable
internal fun rememberPackageUninstaller(model: MarketModel, saveExport: (String) -> Unit, onDone: (UninstallOutcome) -> Unit): PackageUninstaller {
    val scope = rememberCoroutineScope()
    val save by rememberUpdatedState(saveExport)
    val done by rememberUpdatedState(onDone)
    return remember(model) { PackageUninstaller(model, scope, { save }, { done }) }
}
