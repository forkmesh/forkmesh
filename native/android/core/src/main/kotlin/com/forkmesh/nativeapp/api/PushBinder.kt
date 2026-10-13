package com.forkmesh.nativeapp.api

import java.util.concurrent.CopyOnWriteArrayList

interface PushPlatform {
    fun permissionStatus(): String?

    fun requestPermission(): Boolean?

    fun environment(): String?

    fun getToken(): String?
}

class PushBinder(
    private val api: ApiService,
    private val platform: PushPlatform?,
    supportedOverride: Boolean? = null,
) {
    val supported: Boolean = supportedOverride ?: (platform != null)

    @Volatile
    var token: String = ""
        private set

    @Volatile
    var environment: String = ""
        private set

    @Volatile
    var permission: String = "unknown"
        private set

    @Volatile
    var registered: Boolean = false
        private set

    @Volatile
    var relayConfigured: Boolean? = null
        private set

    @Volatile
    var lastError: String = ""

    @Volatile
    var onOpened: (() -> Unit)? = null

    @Volatile
    var accountKeyProvider: (() -> String)? = null

    private var registeredFor = ""
    private var started = false
    private val listeners = CopyOnWriteArrayList<() -> Unit>()

    fun addListener(listener: () -> Unit) {
        listeners.add(listener)
    }

    fun removeListener(listener: () -> Unit) {
        listeners.remove(listener)
    }

    private fun notifyListeners() {
        for (listener in listeners) listener()
    }

    fun start(accountKey: String) {
        if (!supported) return
        val native = platform ?: return
        started = true
        try {
            val status = native.permissionStatus() ?: "unknown"
            permission = status
            if (status == "notDetermined") {
                val granted = native.requestPermission() ?: false
                permission = if (granted) "authorized" else "denied"
                if (!granted) {
                    notifyListeners()
                    return
                }
            } else if (status == "denied") {
                notifyListeners()
                return
            } else {
                native.requestPermission()
            }
            environment = native.environment() ?: "production"
            val cached = native.getToken() ?: ""
            if (cached.isNotEmpty()) {
                token = cached
                publish(accountKey)
            }
        } catch (error: Exception) {
            lastError = error.message ?: error.toString()
        }
        notifyListeners()
    }

    fun stop() {
        started = false
        val current = token
        synchronized(this) { registeredFor = "" }
        registered = false
        relayConfigured = null
        notifyListeners()
        if (current.isEmpty()) return
        api.unregisterPushDevice(current)
    }

    private fun publish(accountKey: String) {
        if (token.isEmpty() || accountKey.isEmpty()) return
        val fingerprint = "$accountKey:$token:$environment"
        synchronized(this) { if (fingerprint == registeredFor) return }
        val result = api.registerPushDevice(
            deviceToken = token,
            environment = environment.ifEmpty { "production" },
        )
        if (result.ok) {
            synchronized(this) { registeredFor = fingerprint }
            registered = true
            relayConfigured = result.relayConfigured
            lastError = ""
        } else {
            registered = false
            lastError = if (result.statusCode == 0) {
                "The relay could not be reached to register this phone."
            } else {
                "The relay refused the registration (HTTP ${result.statusCode})."
            }
        }
        notifyListeners()
    }

    fun rebind(accountKey: String) {
        if (!supported) return
        if (token.isEmpty()) {
            start(accountKey)
            return
        }
        publish(accountKey)
    }

    fun enable() = rebind(accountKeyProvider?.invoke() ?: "")

    fun sendTest(): PushTestResult {
        val result = api.sendTestPush()
        if (result.configured != null) relayConfigured = result.configured
        notifyListeners()
        return result
    }

    fun onToken(newToken: String, newEnvironment: String?) {
        token = newToken
        environment = newEnvironment ?: environment
        lastError = ""
        if (started) publish(accountKeyProvider?.invoke() ?: "")
        notifyListeners()
    }

    fun onError(message: String) {
        lastError = message
        notifyListeners()
    }

    fun onNotificationOpened() {
        onOpened?.invoke()
    }
}
