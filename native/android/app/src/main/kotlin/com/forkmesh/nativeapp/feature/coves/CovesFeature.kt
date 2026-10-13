package com.forkmesh.nativeapp.feature.coves

import android.content.Context
import com.forkmesh.nativeapp.App
import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.api.InboxService
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.coves.CoveAccountCheck
import com.forkmesh.nativeapp.coves.CoveFileRef
import com.forkmesh.nativeapp.coves.CoveRepo
import com.forkmesh.nativeapp.coves.CoveRepositoryIO
import com.forkmesh.nativeapp.coves.CoveStore
import com.forkmesh.nativeapp.coves.CovesModel
import com.forkmesh.nativeapp.feature.account.AuthService
import com.forkmesh.nativeapp.feature.account.projects.UserPresence
import com.forkmesh.nativeapp.feature.navigation.NavigationFeature
import com.forkmesh.nativeapp.feature.tools.vault.VaultScreen
import com.forkmesh.nativeapp.navigation.Route
import com.forkmesh.nativeapp.platform.AppLifecycle
import com.forkmesh.nativeapp.platform.ProjectLifecycle
import com.forkmesh.nativeapp.security.DeviceIdentity
import com.forkmesh.nativeapp.ui.kit.FmMessenger
import com.forkmesh.nativeapp.ui.navigation.Screen
import kotlinx.coroutines.launch
import java.net.URLEncoder

object CovesFeature {
    private const val OWNER = "WS21"
    private val models = HashMap<String, CovesModel>()
    private var app: App? = null

    val api: ApiService? get() = Services.get<ApiService>()
    val account: String get() = Services.get<AuthService>()?.session?.value?.nodeName.orEmpty()
    val openerKey: String get() = Services.get<DeviceIdentity>()?.publicKeyB64url.orEmpty()

    fun install(app: App) {
        this.app = app
        forgetEverything()
        NavigationFeature.register(Route.Kind.Coves, OWNER) { route ->
            val repo = (route as? Route.Coves)?.repo
            if (repo == null) CovesRepoPickerScreen() else CovesScreen(CoveRepo(repo.owner, repo.name))
        }
        NavigationFeature.register(Route.Kind.Cove, OWNER) { route -> CoveLinkScreen((route as Route.Cove).id) }
        NavigationFeature.register(Route.Kind.Vault, OWNER) { route ->
            if ((route as? Route.Vault)?.section == "secrets") ActionsSecretsScreen() else VaultScreen()
        }
        ProjectLifecycle.scope.launch {
            AppLifecycle.foreground.collect { foreground -> if (!foreground) lockEverything() }
        }
        ProjectLifecycle.onClose { forgetEverything() }
    }

    fun covesScreen(owner: String, name: String): Screen = CovesScreen(CoveRepo(owner, name))

    fun secretsScreen(): Screen = ActionsSecretsScreen()

    @Synchronized
    fun model(repo: CoveRepo): CovesModel? {
        val api = api ?: return null
        val key = repo.slug.lowercase()
        val current = account
        models[key]?.let { existing ->
            if (CoveStore.normalizedAccount(existing.account) != CoveStore.normalizedAccount(current)) {
                existing.forget()
                existing.account = current
            }
            existing.openerKey = openerKey
            return existing
        }
        val model = CovesModel(repo, current, openerKey, io(api))
        models[key] = model
        return model
    }

    @Synchronized
    fun lockEverything() {
        for (model in models.values) model.lockAll()
    }

    @Synchronized
    fun forgetEverything() {
        for (model in models.values) model.forget()
        models.clear()
    }

    suspend fun authenticate(context: Context, messenger: FmMessenger, reason: String): Boolean =
        UserPresence.confirm(context, messenger, reason)

    private fun io(api: ApiService): CoveRepositoryIO = object : CoveRepositoryIO {
        override fun listFiles(repo: CoveRepo): List<CoveFileRef> = try {
            api.tree(repo.owner, repo.name, CoveStore.COVES_DIR).entries.filter { !it.isDirectory }.map { CoveFileRef(it.path, it.size) }
        } catch (error: Exception) {
            val message = error.message.orEmpty()
            if (message.contains("404") || message.lowercase().contains("not found")) emptyList() else throw error
        }

        override fun readFile(repo: CoveRepo, path: String, maxBytes: Int): ByteArray =
            api.rawBytes(repo.owner, repo.name, path, maxBytes = maxBytes)

        override fun propose(repo: CoveRepo, title: String, head: String, patch: String, description: String) {
            val inbox = Services.get<InboxService>()
                ?: throw IllegalStateException("This device has no signing identity, so it cannot propose changes.")
            val branches = try {
                api.branches(repo.owner, repo.name)
            } catch (_: Exception) {
                emptyList()
            }
            val base = branches.firstOrNull { it.isDefault }?.name ?: branches.firstOrNull()?.name ?: "main"
            inbox.submitNewPull(repo.owner, repo.name, title, base, head, patch, description, account)
        }

        override fun checkAccount(name: String): CoveAccountCheck = try {
            val record = api.accountApiCall("GET", "/api/accounts/${URLEncoder.encode(name, "UTF-8")}")
            val exists = record["exists"] as? Boolean ?: true
            val status = record["status"] as? String ?: "active"
            CoveAccountCheck(exists, status == "active")
        } catch (_: Exception) {
            CoveAccountCheck(exists = false, active = false)
        }
    }
}
