package com.forkmesh.nativeapp.feature.tools.vault

import android.app.Activity
import android.app.Application
import android.app.KeyguardManager
import android.content.Context
import android.content.Intent
import android.hardware.biometrics.BiometricManager
import android.hardware.biometrics.BiometricPrompt
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.CancellationSignal
import android.provider.Settings
import android.view.autofill.AutofillManager
import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.model.OrgApiException
import com.forkmesh.nativeapp.security.SecretStore
import com.forkmesh.nativeapp.vault.VaultClient
import com.forkmesh.nativeapp.vault.VaultCrypto
import com.forkmesh.nativeapp.vault.VaultException
import com.forkmesh.nativeapp.vault.VaultImportReport
import com.forkmesh.nativeapp.vault.VaultItem
import com.forkmesh.nativeapp.vault.VaultTransfer
import com.forkmesh.nativeapp.vault.VaultTransferLogin
import com.forkmesh.nativeapp.vault.VaultLoginAnswer
import com.forkmesh.nativeapp.vault.VaultScreenLogic
import com.forkmesh.nativeapp.vault.VaultRecipient
import com.forkmesh.nativeapp.vault.VaultShareOutcome
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import java.lang.ref.WeakReference
import kotlin.coroutines.resume

enum class VaultPhase { Loading, SignedOut, NotSetUp, Locked, Unlocked }

data class VaultAutofillStatus(
    val supported: Boolean = false,
    val enabled: Boolean = false,
    val deviceSecure: Boolean = false,
    val logins: Int = 0,
)

class VaultDeviceAuth(private val app: Application) : Application.ActivityLifecycleCallbacks {
    private var current: WeakReference<Activity>? = null

    init {
        app.registerActivityLifecycleCallbacks(this)
    }

    fun available(): Boolean {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.P) return false
        return (app.getSystemService(Context.KEYGUARD_SERVICE) as KeyguardManager).isDeviceSecure
    }

    suspend fun authenticate(reason: String): Boolean {
        if (!available()) return false
        val activity = current?.get() ?: return false
        return withContext(Dispatchers.Main) {
            suspendCancellableCoroutine { continuation ->
                val builder = BiometricPrompt.Builder(activity).setTitle("ForkMesh Vault").setSubtitle(reason)
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                    builder.setAllowedAuthenticators(
                        BiometricManager.Authenticators.BIOMETRIC_WEAK or BiometricManager.Authenticators.DEVICE_CREDENTIAL,
                    )
                } else {
                    @Suppress("DEPRECATION")
                    builder.setDeviceCredentialAllowed(true)
                }
                val signal = CancellationSignal()
                continuation.invokeOnCancellation { signal.cancel() }
                try {
                    builder.build().authenticate(signal, activity.mainExecutor, object : BiometricPrompt.AuthenticationCallback() {
                        override fun onAuthenticationSucceeded(result: BiometricPrompt.AuthenticationResult?) {
                            if (continuation.isActive) continuation.resume(true)
                        }

                        override fun onAuthenticationError(errorCode: Int, errString: CharSequence?) {
                            if (continuation.isActive) continuation.resume(false)
                        }
                    })
                } catch (_: Exception) {
                    if (continuation.isActive) continuation.resume(false)
                }
            }
        }
    }

    override fun onActivityResumed(activity: Activity) {
        current = WeakReference(activity)
    }

    override fun onActivityCreated(activity: Activity, savedInstanceState: Bundle?) {}
    override fun onActivityStarted(activity: Activity) {}
    override fun onActivityPaused(activity: Activity) {}
    override fun onActivityStopped(activity: Activity) {}
    override fun onActivitySaveInstanceState(activity: Activity, outState: Bundle) {}
    override fun onActivityDestroyed(activity: Activity) {
        if (current?.get() === activity) current = null
    }
}

class VaultPlatform(private val context: Context) {
    private val supported: Boolean get() = Build.VERSION.SDK_INT >= Build.VERSION_CODES.O

    fun autofillStatus(): VaultAutofillStatus {
        if (!supported) return VaultAutofillStatus()
        val manager = context.getSystemService(AutofillManager::class.java)
        val store = VaultAutofillStore(context)
        return VaultAutofillStatus(
            supported = manager?.isAutofillSupported == true,
            enabled = manager?.hasEnabledAutofillServices() == true,
            deviceSecure = store.deviceSecure,
            logins = store.logins().size,
        )
    }

    fun openAutofillSettings(activity: Activity?) {
        if (!supported) return
        val intent = Intent(Settings.ACTION_REQUEST_SET_AUTOFILL_SERVICE, Uri.parse("package:${context.packageName}"))
        if (activity != null) activity.startActivity(intent) else context.startActivity(intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK))
    }

    fun syncAutofill(account: String, logins: List<Map<String, String>>): Boolean =
        supported && VaultAutofillStore(context).sync(account, logins)

    fun clearAutofill() {
        if (supported) VaultAutofillStore(context).clear()
    }

    fun takePendingSaves(): List<Map<String, String>> =
        if (supported) VaultAutofillStore(context).takePending() else emptyList()
}

class VaultService(
    api: ApiService,
    private val accountProvider: () -> String,
    private val platform: VaultPlatform,
    private val deviceAuth: VaultDeviceAuth,
    private val store: SecretStore,
) {
    private val client = VaultClient({ method, path, body ->
        try {
            api.accountApiCall(method, path, body)
        } catch (error: OrgApiException) {
            throw VaultException(error.code)
        }
    })
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)
    private val mutex = Mutex()

    private val phaseState = MutableStateFlow(VaultPhase.Loading)
    private val itemsState = MutableStateFlow<List<VaultItem>>(emptyList())
    private val autoLockState = MutableStateFlow(5)
    private val quickUnlockState = MutableStateFlow(false)
    private val quickUnlockAvailableState = MutableStateFlow(false)
    private val autofillState = MutableStateFlow(false)
    private val autofillStatusState = MutableStateFlow(VaultAutofillStatus())

    val phase: StateFlow<VaultPhase> = phaseState.asStateFlow()
    val items: StateFlow<List<VaultItem>> = itemsState.asStateFlow()
    val autoLockMinutes: StateFlow<Int> = autoLockState.asStateFlow()
    val quickUnlockEnabled: StateFlow<Boolean> = quickUnlockState.asStateFlow()
    val quickUnlockAvailable: StateFlow<Boolean> = quickUnlockAvailableState.asStateFlow()
    val autofillEnabled: StateFlow<Boolean> = autofillState.asStateFlow()
    val autofillStatus: StateFlow<VaultAutofillStatus> = autofillStatusState.asStateFlow()

    val account: String get() = client.account

    private var boundAccount = ""
    private var idleTimer: Job? = null
    private var pausedAt: Long? = null

    fun find(id: String): VaultItem? = client.find(id)

    private fun key(name: String): String = "vault/$boundAccount/$name"

    private suspend fun <T> io(block: () -> T): T = mutex.withLock { withContext(Dispatchers.IO) { block() } }

    private fun publish() {
        itemsState.value = client.items
    }

    suspend fun refresh() {
        val signedIn = accountProvider().trim().lowercase()
        if (signedIn != boundAccount) {
            lock()
            boundAccount = signedIn
        }
        if (signedIn.isEmpty()) {
            phaseState.value = VaultPhase.SignedOut
            return
        }
        if (client.unlocked) {
            phaseState.value = VaultPhase.Unlocked
            return
        }
        io { client.status() }
        val minutes = withContext(Dispatchers.IO) { store.read(key("autoLockMinutes")) }?.toIntOrNull() ?: 5
        autoLockState.value = if (minutes in AUTO_LOCK_CHOICES) minutes else 5
        quickUnlockAvailableState.value = deviceAuth.available()
        quickUnlockState.value = quickUnlockAvailableState.value &&
            !withContext(Dispatchers.IO) { store.read(key("quickUnlock")) }.isNullOrEmpty()
        autofillState.value = withContext(Dispatchers.IO) { store.read(key("autofill")) } == "1"
        autofillStatusState.value = withContext(Dispatchers.IO) { platform.autofillStatus() }
        phaseState.value = if (client.installed) VaultPhase.Locked else VaultPhase.NotSetUp
    }

    suspend fun create(masterPassword: String): String {
        val code = io { client.setUp(masterPassword) }
        entered()
        return code
    }

    suspend fun unlock(masterPassword: String) {
        io { client.unlock(masterPassword) }
        entered()
    }

    suspend fun unlockWithDevice(): Boolean {
        if (!quickUnlockState.value) return false
        val stored = withContext(Dispatchers.IO) { store.read(key("quickUnlock")) }.orEmpty()
        if (stored.isEmpty()) return false
        if (!deviceAuth.authenticate("Unlock your ForkMesh vault")) return false
        try {
            @Suppress("UNCHECKED_CAST")
            val saved = Json.parse(stored) as? Map<String, Any?> ?: throw VaultException("invalid_keyset")
            io { client.resume(saved) }
        } catch (error: VaultException) {
            if (error.code == "keyset_mismatch" || error.code == "invalid_keyset") setQuickUnlock(false)
            throw error
        }
        entered()
        return true
    }

    suspend fun recover(recoveryCode: String, newMasterPassword: String): String {
        val renewed = io { client.recover(recoveryCode, newMasterPassword) }
        entered()
        return renewed
    }

    private suspend fun entered() {
        importPendingSaves()
        reload()
        scope.launch {
            try {
                if (io { client.syncPending() } > 0) publish()
            } catch (_: Exception) {
            }
        }
        phaseState.value = VaultPhase.Unlocked
        touch()
    }

    fun lock() {
        idleTimer?.cancel()
        client.lock()
        itemsState.value = emptyList()
        if (phaseState.value == VaultPhase.Unlocked) phaseState.value = VaultPhase.Locked
    }

    fun touch() {
        idleTimer?.cancel()
        if (!client.unlocked) return
        val minutes = autoLockState.value
        idleTimer = scope.launch {
            delay(minutes * 60_000L)
            lock()
        }
    }

    fun appPaused() {
        if (pausedAt == null) pausedAt = System.currentTimeMillis()
    }

    fun appResumed() {
        val at = pausedAt
        pausedAt = null
        if (at != null && System.currentTimeMillis() - at >= BACKGROUND_GRACE_MS) lock()
    }

    suspend fun reload() {
        io { client.load() }
        syncAutofill()
        publish()
    }

    suspend fun save(fields: Map<String, Any?>, id: String = ""): String {
        touch()
        val saved = io { client.save(fields, id) }
        reload()
        return saved
    }

    suspend fun importLogins(logins: List<VaultTransferLogin>): VaultImportReport {
        touch()
        if (!client.unlocked) throw VaultException("vault_locked")
        val plan = VaultTransfer.plan(logins, client.items)
        var added = 0
        var stoppedBy = ""
        for (login in plan.fresh) {
            try {
                io { client.save(login.fields) }
                added++
                touch()
            } catch (error: Exception) {
                stoppedBy = VaultScreenLogic.errorText(error)
                break
            }
        }
        runCatching { reload() }
        return VaultImportReport(added, plan.duplicates, plan.fresh.size - added, stoppedBy)
    }

    fun exportLogins(): List<VaultTransferLogin> {
        touch()
        if (!client.unlocked) throw VaultException("vault_locked")
        return VaultTransfer.logins(client.items)
    }

    fun existingItems(): List<VaultItem> {
        touch()
        return client.items
    }

    suspend fun saveLogin(title: String, url: String, username: String, password: String): VaultLoginAnswer {
        loginBlocked()?.let { return it }
        touch()
        return try {
            val fields = mapOf("title" to title, "url" to url, "username" to username, "password" to password, "notes" to "")
            val (_, changed) = io { client.rememberLogin(fields) }
            syncAutofill()
            publish()
            VaultLoginAnswer.Saved(changed)
        } catch (error: Exception) {
            VaultLoginAnswer.Failed(VaultScreenLogic.errorText(error))
        }
    }

    suspend fun findLogins(url: String): VaultLoginAnswer {
        loginBlocked()?.let { return it }
        touch()
        return try {
            reload()
            VaultLoginAnswer.Found(client.loginsFor(url))
        } catch (error: Exception) {
            VaultLoginAnswer.Failed(VaultScreenLogic.errorText(error))
        }
    }

    private suspend fun loginBlocked(): VaultLoginAnswer? {
        try {
            refresh()
        } catch (error: Exception) {
            return VaultLoginAnswer.Failed(VaultScreenLogic.errorText(error))
        }
        return when (phaseState.value) {
            VaultPhase.Unlocked -> null
            VaultPhase.NotSetUp -> VaultLoginAnswer.NoVault
            VaultPhase.SignedOut -> VaultLoginAnswer.SignedOut
            VaultPhase.Loading, VaultPhase.Locked -> VaultLoginAnswer.Locked
        }
    }

    suspend fun remove(id: String) {
        touch()
        io { client.remove(id) }
        reload()
    }

    suspend fun previewShare(kind: String, name: String): List<VaultRecipient> {
        touch()
        return io { client.previewShare(kind, name) }
    }

    suspend fun share(id: String, kind: String, name: String, permission: String, accepted: List<String> = emptyList()): VaultShareOutcome {
        touch()
        val outcome = io { client.share(id, kind, name, permission, accepted) }
        reload()
        return outcome
    }

    suspend fun unshare(id: String, kind: String, name: String) {
        touch()
        io { client.unshare(id, kind, name) }
        reload()
    }

    suspend fun organizations(): List<String> = io { client.organizations() }

    suspend fun myFingerprint(): String = io { client.myFingerprint() }

    suspend fun exportData(): Map<String, Any?> = io { client.exportData() }

    suspend fun changePassword(masterPassword: String) {
        touch()
        io { client.changePassword(masterPassword) }
    }

    suspend fun renewRecoveryCode(): String {
        touch()
        return io { client.renewRecoveryCode() }
    }

    suspend fun destroy(): Map<String, Any?> {
        val payload = io { client.destroy() }
        forgetDeviceCopies()
        itemsState.value = emptyList()
        phaseState.value = VaultPhase.NotSetUp
        return payload
    }

    suspend fun setAutoLockMinutes(minutes: Int) {
        val value = if (minutes in AUTO_LOCK_CHOICES) minutes else 5
        autoLockState.value = value
        withContext(Dispatchers.IO) { store.write(key("autoLockMinutes"), "$value") }
        touch()
    }

    suspend fun setQuickUnlock(enabled: Boolean): Boolean {
        if (enabled) {
            if (!client.unlocked || !deviceAuth.available()) return false
            if (!deviceAuth.authenticate("Turn on quick unlock for your vault")) return false
            val exported = Json.stringify(client.exportKeyset())
            withContext(Dispatchers.IO) { store.write(key("quickUnlock"), exported) }
        } else {
            withContext(Dispatchers.IO) { store.delete(key("quickUnlock")) }
        }
        quickUnlockState.value = enabled
        return true
    }

    suspend fun setAutofill(enabled: Boolean) {
        autofillState.value = enabled
        withContext(Dispatchers.IO) { store.write(key("autofill"), if (enabled) "1" else "0") }
        if (enabled) syncAutofill() else withContext(Dispatchers.IO) { platform.clearAutofill() }
        autofillStatusState.value = withContext(Dispatchers.IO) { platform.autofillStatus() }
    }

    fun openAutofillSettings(activity: Activity?) = platform.openAutofillSettings(activity)

    suspend fun refreshAutofillStatus() {
        autofillStatusState.value = withContext(Dispatchers.IO) { platform.autofillStatus() }
    }

    private suspend fun syncAutofill() {
        if (!autofillState.value || !client.unlocked) return
        val logins = client.items.filter { it.readable && it.password.isNotEmpty() }.map {
            mapOf("id" to it.id, "title" to it.title, "username" to it.username, "password" to it.password, "host" to it.host)
        }
        withContext(Dispatchers.IO) {
            platform.syncAutofill(client.account, logins)
            autofillStatusState.value = platform.autofillStatus()
        }
    }

    private suspend fun importPendingSaves() {
        if (!autofillState.value) return
        for (pending in withContext(Dispatchers.IO) { platform.takePendingSaves() }) {
            val host = pending["host"].orEmpty()
            try {
                io {
                    client.save(
                        mapOf(
                            "title" to (pending["title"]?.takeIf { it.isNotEmpty() } ?: host),
                            "username" to pending["username"].orEmpty(),
                            "password" to pending["password"].orEmpty(),
                            "url" to if (host.isEmpty()) "" else "https://$host",
                            "notes" to (pending["app"]?.takeIf { it.isNotEmpty() }?.let { "Saved from $it by autofill." } ?: ""),
                        ),
                    )
                }
            } catch (_: VaultException) {
            }
        }
    }

    private suspend fun forgetDeviceCopies() {
        withContext(Dispatchers.IO) {
            platform.clearAutofill()
            if (boundAccount.isNotEmpty()) {
                store.delete(key("quickUnlock"))
                store.delete(key("autofill"))
            }
        }
        quickUnlockState.value = false
        autofillState.value = false
    }

    suspend fun signedOut() {
        lock()
        forgetDeviceCopies()
        boundAccount = ""
        phaseState.value = VaultPhase.SignedOut
    }

    fun generatePassword(): String = VaultCrypto.generatePassword()

    companion object {
        const val BACKGROUND_GRACE_MS = 60_000L
        val AUTO_LOCK_CHOICES = listOf(1, 5, 15)
    }
}
