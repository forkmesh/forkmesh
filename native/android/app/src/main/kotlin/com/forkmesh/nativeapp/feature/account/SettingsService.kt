package com.forkmesh.nativeapp.feature.account

import android.content.pm.ApplicationInfo
import com.forkmesh.nativeapp.App
import com.forkmesh.nativeapp.api.AccountSettings
import com.forkmesh.nativeapp.api.AppEnv
import com.forkmesh.nativeapp.api.PrefStore
import com.forkmesh.nativeapp.security.SecretStore
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.withContext

class SettingsService(val core: AccountSettings) {
    private val displayNameState = MutableStateFlow(core.displayName)
    private val serverUrlState = MutableStateFlow(core.serverUrl)
    private val serverUrlOverrideState = MutableStateFlow(core.hasServerUrlOverride)
    private val roomState = MutableStateFlow(core.room)
    private val passphraseState = MutableStateFlow(core.passphrase)
    private val solanaAddressState = MutableStateFlow(core.solanaAddress)
    private val githubTokenState = MutableStateFlow(core.githubToken)
    private val gitlabTokenState = MutableStateFlow(core.gitlabToken)
    private val serveAsNodeState = MutableStateFlow(core.serveAsNode)
    private val chatTipsEnabledState = MutableStateFlow(core.chatTipsEnabled)
    private val bugButtonAnchorState = MutableStateFlow(core.bugButtonAnchor)
    private val bugButtonOnLeftState = MutableStateFlow(core.bugButtonOnLeft)
    private val showSponsoredMessagesState = MutableStateFlow(core.showSponsoredMessages)
    private val showTelemetryStripState = MutableStateFlow(core.showTelemetryStrip)
    private val sponsoredHiddenUntilMsState = MutableStateFlow(core.sponsoredHiddenUntilMs)

    val displayName: StateFlow<String> = displayNameState.asStateFlow()

    val serverUrl: StateFlow<String> = serverUrlState.asStateFlow()

    val hasServerUrlOverride: StateFlow<Boolean> = serverUrlOverrideState.asStateFlow()

    val room: StateFlow<String> = roomState.asStateFlow()

    val passphrase: StateFlow<String> = passphraseState.asStateFlow()

    val solanaAddress: StateFlow<String> = solanaAddressState.asStateFlow()

    val githubToken: StateFlow<String> = githubTokenState.asStateFlow()

    val gitlabToken: StateFlow<String> = gitlabTokenState.asStateFlow()

    val serveAsNode: StateFlow<Boolean> = serveAsNodeState.asStateFlow()

    val chatTipsEnabled: StateFlow<Boolean> = chatTipsEnabledState.asStateFlow()

    val bugButtonAnchor: StateFlow<Double> = bugButtonAnchorState.asStateFlow()

    val bugButtonOnLeft: StateFlow<Boolean> = bugButtonOnLeftState.asStateFlow()

    val showSponsoredMessages: StateFlow<Boolean> = showSponsoredMessagesState.asStateFlow()

    val showTelemetryStrip: StateFlow<Boolean> = showTelemetryStripState.asStateFlow()

    val sponsoredHiddenUntilMs: StateFlow<Long> = sponsoredHiddenUntilMsState.asStateFlow()

    val defaultServerUrl: String get() = core.defaultServerUrl

    val worldUri: String get() = core.worldUri

    val environmentLabel: String get() = core.env.describe(core.serverUrl)

    private val listener: () -> Unit = { publish() }

    init {
        core.addListener(listener)
    }

    private fun publish() {
        displayNameState.value = core.displayName
        serverUrlState.value = core.serverUrl
        serverUrlOverrideState.value = core.hasServerUrlOverride
        roomState.value = core.room
        passphraseState.value = core.passphrase
        solanaAddressState.value = core.solanaAddress
        githubTokenState.value = core.githubToken
        gitlabTokenState.value = core.gitlabToken
        serveAsNodeState.value = core.serveAsNode
        chatTipsEnabledState.value = core.chatTipsEnabled
        bugButtonAnchorState.value = core.bugButtonAnchor
        bugButtonOnLeftState.value = core.bugButtonOnLeft
        showSponsoredMessagesState.value = core.showSponsoredMessages
        showTelemetryStripState.value = core.showTelemetryStrip
        sponsoredHiddenUntilMsState.value = core.sponsoredHiddenUntilMs
    }

    fun setDisplayName(value: String) {
        core.setDisplayName(value)
    }

    fun setServerUrl(value: String) {
        core.setServerUrl(value)
    }

    fun resetServerUrl() {
        core.resetServerUrl()
    }

    fun setRoom(value: String) {
        core.setRoom(value)
    }

    fun setSolanaAddress(value: String) {
        core.setSolanaAddress(value)
    }

    fun setServeAsNode(value: Boolean) {
        core.setServeAsNode(value)
    }

    fun setChatTipsEnabled(value: Boolean) {
        core.setChatTipsEnabled(value)
    }

    fun setShowSponsoredMessages(value: Boolean) {
        core.setShowSponsoredMessages(value)
    }

    fun setShowTelemetryStrip(value: Boolean) {
        core.setShowTelemetryStrip(value)
    }

    fun hideSponsoredMessages() {
        core.hideSponsoredMessages()
    }

    fun setBugButtonPlacement(anchor: Double, onLeft: Boolean) {
        core.setBugButtonPlacement(anchor, onLeft)
    }

    suspend fun setPassphrase(value: String) = withContext(Dispatchers.IO) { core.setPassphrase(value) }

    suspend fun setGithubToken(value: String) = withContext(Dispatchers.IO) { core.setGithubToken(value) }

    suspend fun setGitlabToken(value: String) = withContext(Dispatchers.IO) { core.setGitlabToken(value) }

    suspend fun clearSecrets() = withContext(Dispatchers.IO) { core.clearSecrets() }

    companion object {
        fun create(app: App, prefs: PrefStore, secrets: SecretStore): SettingsService {
            val debuggable = (app.applicationInfo.flags and ApplicationInfo.FLAG_DEBUGGABLE) != 0
            return SettingsService(AccountSettings.create(prefs, secrets, AppEnv.PROD, releaseMode = !debuggable))
        }

        fun forProject(
            prefs: PrefStore,
            scope: com.forkmesh.nativeapp.account.projects.ProjectScope,
            secrets: SecretStore,
            serverUrl: () -> String,
            useServerUrl: (String) -> Unit,
        ): SettingsService = SettingsService(
            AccountSettings.forProject(prefs, scope.prefs(prefs), scope.secrets(secrets), serverUrl, useServerUrl, AppEnv.PROD),
        )
    }
}
