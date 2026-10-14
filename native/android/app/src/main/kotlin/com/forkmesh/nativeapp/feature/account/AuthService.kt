package com.forkmesh.nativeapp.feature.account

import android.os.Build
import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.api.AuthController
import com.forkmesh.nativeapp.api.PrefStore
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.model.ChatRetention
import com.forkmesh.nativeapp.model.ChatRetentionException
import com.forkmesh.nativeapp.model.UserStatus
import com.forkmesh.nativeapp.model.UserStatusException
import com.forkmesh.nativeapp.model.auth.AccountAvailability
import com.forkmesh.nativeapp.model.auth.AccountDevice
import com.forkmesh.nativeapp.model.auth.AuthSession
import com.forkmesh.nativeapp.model.auth.androidDeviceLabel
import com.forkmesh.nativeapp.model.chatRetentionErrorMessage
import com.forkmesh.nativeapp.model.userStatusErrorMessage
import com.forkmesh.nativeapp.security.DeviceIdentity
import com.forkmesh.nativeapp.security.SecretStore
import kotlin.coroutines.cancellation.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.withContext

class AuthService(val core: AuthController, private val prefs: PrefStore? = null) {
    private val sessionState = MutableStateFlow(core.session)
    private val guestState = MutableStateFlow(core.isGuest)
    private val accessState = MutableStateFlow(core.hasAppAccess)
    private val bannerState = MutableStateFlow(core.revokedBannerPending)
    private val devicesState = MutableStateFlow<List<AccountDevice>?>(null)
    private val devicesLoadingState = MutableStateFlow(false)
    private val devicesErrorState = MutableStateFlow("")
    private val userStatusState = MutableStateFlow(core.session?.userStatus ?: UserStatus())
    private val chatRetentionState = MutableStateFlow(storedChatRetention(core.session))

    val session: StateFlow<AuthSession?> = sessionState.asStateFlow()

    val isGuest: StateFlow<Boolean> = guestState.asStateFlow()

    val hasAppAccess: StateFlow<Boolean> = accessState.asStateFlow()

    val revokedBannerPending: StateFlow<Boolean> = bannerState.asStateFlow()

    val devices: StateFlow<List<AccountDevice>?> = devicesState.asStateFlow()

    val devicesLoading: StateFlow<Boolean> = devicesLoadingState.asStateFlow()

    val devicesError: StateFlow<String> = devicesErrorState.asStateFlow()

    val userStatus: StateFlow<UserStatus> = userStatusState.asStateFlow()

    val chatRetention: StateFlow<ChatRetention> = chatRetentionState.asStateFlow()

    val isAuthenticated: Boolean get() = core.isAuthenticated

    val signupUrl: String get() = core.signupUrl

    val forgotPasswordUrl: String get() = core.forgotPasswordUrl

    val knownDevices: List<AccountDevice> get() = devicesState.value ?: core.session?.devices.orEmpty()

    private val listener: () -> Unit = { publish() }

    init {
        core.addListener(listener)
    }

    private fun publish() {
        val next = core.session
        if (sessionState.value?.nodeName != next?.nodeName || next == null) {
            devicesState.value = null
            userStatusState.value = next?.userStatus ?: UserStatus()
        }
        sessionState.value = next
        chatRetentionState.value = storedChatRetention(next)
        guestState.value = core.isGuest
        accessState.value = core.hasAppAccess
        bannerState.value = core.revokedBannerPending
    }

    private fun storedChatRetention(session: AuthSession?): ChatRetention {
        if (session == null || session.sessionKind == "preview" || session.nodeName.isEmpty()) return ChatRetention()
        val cached = ChatRetention.decode(prefs?.getString(ChatRetention.prefsKey(session.nodeName)))
        val fromLogin = session.chatRetention
        return when {
            cached == null -> fromLogin ?: ChatRetention()
            fromLogin == null || cached.updatedAt >= fromLogin.updatedAt -> cached
            else -> fromLogin
        }
    }

    private fun keepChatRetention(nodeName: String, retention: ChatRetention) {
        if (core.session?.nodeName != nodeName) return
        prefs?.putString(ChatRetention.prefsKey(nodeName), retention.encode())
        chatRetentionState.value = retention
    }

    private suspend fun <T> io(block: () -> T): T = withContext(Dispatchers.IO) { block() }

    fun enterGuestMode() {
        core.enterGuestMode()
    }

    fun exitGuestMode() {
        core.exitGuestMode()
    }

    fun clearRevokedBanner() {
        core.clearRevokedBanner()
        publish()
    }

    suspend fun login(identifier: String, password: String, totp: String = ""): AuthSession =
        io { core.login(identifier, password, totp) }.also { publish() }

    suspend fun logout() {
        io { core.logout() }
        publish()
    }

    suspend fun handleUnauthorized(code: String = "") {
        io { core.handleUnauthorized(code) }
        publish()
    }

    suspend fun checkNodeName(nodeName: String): AccountAvailability = io { core.checkNodeName(nodeName) }

    suspend fun deleteAccount(password: String) {
        io { core.deleteAccount(password) }
        publish()
    }

    suspend fun authenticatePreview() {
        io { core.authenticatePreview() }
        publish()
    }

    suspend fun refreshAvatar() {
        try {
            if (io { core.refreshAvatar() }) publish()
        } catch (cancelled: CancellationException) {
            throw cancelled
        } catch (_: Exception) {
        }
    }

    suspend fun refreshDevices(api: ApiService = Services.require()) {
        val current = core.session
        if (current == null || current.sessionKind == "preview") return
        devicesLoadingState.value = true
        devicesErrorState.value = ""
        try {
            devicesState.value = io { api.listDevices() }
        } catch (cancelled: CancellationException) {
            throw cancelled
        } catch (_: Exception) {
            devicesErrorState.value = "Could not refresh the node list. Showing the last known nodes."
        } finally {
            devicesLoadingState.value = false
        }
    }

    suspend fun refreshUserStatus(api: ApiService = Services.require()) {
        val current = core.session
        if (current == null || current.sessionKind == "preview") return
        try {
            val status = io { api.updateUserStatus() }
            if (core.session?.nodeName == current.nodeName) userStatusState.value = status
        } catch (cancelled: CancellationException) {
            throw cancelled
        } catch (_: Exception) {
        }
    }

    suspend fun refreshChatRetention(api: ApiService = Services.require()) {
        val current = core.session
        if (current == null || current.sessionKind == "preview" || current.nodeName.isEmpty()) return
        try {
            val retention = io { api.updateChatRetention() }
            keepChatRetention(current.nodeName, retention)
        } catch (cancelled: CancellationException) {
            throw cancelled
        } catch (_: Exception) {
        }
    }

    suspend fun updateChatRetention(payload: Map<String, Any?>, api: ApiService = Services.require()): ChatRetention {
        val current = core.session
        if (current == null || current.sessionKind == "preview" || current.nodeName.isEmpty()) {
            throw ChatRetentionException("not_authenticated", "Sign in to choose when messages disappear.")
        }
        val retention = try {
            io { api.updateChatRetention(payload) }
        } catch (cancelled: CancellationException) {
            throw cancelled
        } catch (refused: ChatRetentionException) {
            throw refused
        } catch (_: Exception) {
            throw ChatRetentionException("unreachable", chatRetentionErrorMessage("unreachable"))
        }
        keepChatRetention(current.nodeName, retention)
        return retention
    }

    suspend fun updateUserStatus(payload: Map<String, Any?>, api: ApiService = Services.require()): UserStatus {
        val current = core.session
        if (current == null || current.sessionKind == "preview") {
            throw UserStatusException("not_authenticated", "Sign in to set a status.")
        }
        val status = try {
            io { api.updateUserStatus(payload) }
        } catch (cancelled: CancellationException) {
            throw cancelled
        } catch (refused: UserStatusException) {
            throw refused
        } catch (_: Exception) {
            throw UserStatusException("unreachable", userStatusErrorMessage("unreachable"))
        }
        if (core.session?.nodeName == current.nodeName) userStatusState.value = status
        return status
    }

    companion object {
        fun create(
            settings: SettingsService,
            prefs: PrefStore,
            secrets: SecretStore,
            projectId: String = "",
            sessionSecrets: SecretStore? = null,
        ): AuthService = AuthService(
            prefs = prefs,
            core = AuthController.create(
                settings.core,
                prefs,
                secrets,
                projectId = projectId,
                sessionSecrets = sessionSecrets,
                identity = { Services.get<DeviceIdentity>() },
                deviceLabelProvider = {
                    androidDeviceLabel(Build.MANUFACTURER.orEmpty(), Build.MODEL.orEmpty(), Build.VERSION.RELEASE.orEmpty())
                },
            ),
        )
    }
}
