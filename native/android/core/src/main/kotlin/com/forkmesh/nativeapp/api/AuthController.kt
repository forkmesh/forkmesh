package com.forkmesh.nativeapp.api

import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.json.jsonOf
import com.forkmesh.nativeapp.json.str
import com.forkmesh.nativeapp.model.auth.AccountAvailability
import com.forkmesh.nativeapp.model.auth.AuthException
import com.forkmesh.nativeapp.model.auth.AuthSession
import com.forkmesh.nativeapp.model.auth.authErrorMessage
import com.forkmesh.nativeapp.model.auth.deviceBindCanonical
import com.forkmesh.nativeapp.net.HttpClient
import com.forkmesh.nativeapp.security.DeviceIdentity
import com.forkmesh.nativeapp.security.SecretStore
import java.util.concurrent.CopyOnWriteArrayList

class AuthController(
    private val settings: AccountSettings,
    private val prefs: PrefStore,
    private val secrets: SecretStore,
    private val identity: () -> DeviceIdentity?,
    private val transport: HttpTransport = HttpTransport.DEFAULT,
    session: AuthSession? = null,
    private val deviceLabelProvider: () -> String = { "Mobile device" },
    private val deviceTimestampProvider: () -> String = { System.currentTimeMillis().toString() },
    val projectId: String = "",
    sessionSecrets: SecretStore? = null,
) {
    private val sessionStore: SecretStore = sessionSecrets ?: secrets
    private val sessionKey: String = sessionKeyFor(projectId)
    private val guestKey: String = guestModeKeyFor(projectId)
    private val bannerKey: String = revokedBannerKeyFor(projectId)
    private val lock = Any()
    private val listeners = CopyOnWriteArrayList<() -> Unit>()

    @Volatile
    var session: AuthSession? = session
        private set

    val isAuthenticated: Boolean get() = session != null

    val isGuest: Boolean get() = !isAuthenticated && prefs.getBool(guestKey) == true

    val hasAppAccess: Boolean get() = isAuthenticated || isGuest

    val revokedBannerPending: Boolean get() = prefs.getBool(bannerKey) == true

    val signupUrl: String get() = base("/signup")

    val forgotPasswordUrl: String get() = base("/forgot-password")

    fun addListener(listener: () -> Unit) {
        listeners.add(listener)
    }

    fun removeListener(listener: () -> Unit) {
        listeners.remove(listener)
    }

    private fun notifyListeners() {
        for (listener in listeners) listener()
    }

    fun enterGuestMode() {
        prefs.putBool(guestKey, true)
        notifyListeners()
    }

    fun exitGuestMode() {
        prefs.remove(guestKey)
        notifyListeners()
    }

    fun clearRevokedBanner() {
        if (prefs.contains(bannerKey)) prefs.remove(bannerKey)
    }

    private fun base(path: String): String = ServerUrl.parse(settings.serverUrl).http(path)

    fun login(identifier: String, password: String, totp: String = ""): AuthSession {
        val cleanIdentifier = identifier.trim().lowercase()
        val cleanTotp = totp.trim()
        val device = identity()
            ?: throw AuthException("identity_unavailable", "This device's signing key is not ready yet. Try again in a moment.")
        var accountName = cleanIdentifier
        var intermediateToken: String? = null
        if (cleanIdentifier.contains('@') || !NODE_NAME_PATTERN.matches(cleanIdentifier)) {
            val probe = postJson(
                "/api/accounts/login",
                jsonOf(
                    "identifier" to cleanIdentifier,
                    "email" to cleanIdentifier,
                    "password" to password,
                    "totp" to cleanTotp,
                ),
            )
            val probed = AuthSession.fromJson(probe)
            if (probed.nodeName.isEmpty()) {
                return saveSession(probed)
            }
            accountName = probed.nodeName.lowercase()
            intermediateToken = probed.sessionToken
        }

        val body = try {
            bindLogin(device, accountName, password, cleanTotp)
        } catch (error: AuthException) {
            if (error.code != "device_key_conflict") throw error
            clearStoredSeed()
            device.rotate()
            bindLogin(device, accountName, password, cleanTotp)
        }
        if (!intermediateToken.isNullOrEmpty()) revokeTokenInBackground(intermediateToken)
        clearRevokedBanner()
        return saveSession(AuthSession.fromJson(body))
    }

    private fun bindLogin(device: DeviceIdentity, accountName: String, password: String, totp: String): Map<String, Any?> {
        val pubkey = device.publicKeyB64url
        val ts = deviceTimestampProvider()
        val canonical = deviceBindCanonical(accountName, pubkey, ts).toByteArray(Charsets.UTF_8)
        val sig = device.sign(canonical)
        return postJson(
            "/api/accounts/login",
            jsonOf(
                "identifier" to accountName,
                "email" to accountName,
                "password" to password,
                "totp" to totp,
                "pubkey" to pubkey,
                "deviceTs" to ts,
                "deviceSig" to sig,
                "deviceKind" to MOBILE_DEVICE_KIND,
                "deviceLabel" to safeDeviceLabel(),
            ),
        )
    }

    private fun safeDeviceLabel(): String = try {
        val label = deviceLabelProvider().trim()
        label.ifEmpty { "Mobile device" }
    } catch (_: Exception) {
        "Mobile device"
    }

    private fun revokeTokenInBackground(token: String) {
        val worker = Thread({
            try {
                postJson("/api/accounts/logout", jsonOf("sessionToken" to token), timeoutMs = REVOKE_TIMEOUT_MS)
            } catch (_: Exception) {
            }
        }, "AuthController-revoke")
        worker.isDaemon = true
        worker.start()
    }

    fun handleUnauthorized(code: String = "") {
        val current = synchronized(lock) { session?.also { session = null } } ?: return
        val revoked = code == "device_revoked" || (current.deviceId.isNotEmpty() && current.sessionKind != "preview")
        clearLocalSession()
        if (!revoked) return
        try {
            clearStoredSeed()
            identity()?.rotate()
        } catch (_: Exception) {
        }
        prefs.putBool(bannerKey, true)
        notifyListeners()
    }

    fun checkNodeName(nodeName: String): AccountAvailability {
        val clean = nodeName.trim().lowercase()
        if (!NODE_NAME_PATTERN.matches(clean)) {
            return AccountAvailability(
                ok = false,
                exists = false,
                available = false,
                name = clean,
                message = "Use lowercase letters, numbers and hyphens. Start with a letter and end with a letter or number.",
            )
        }
        return try {
            val data = getJson("/api/accounts/$clean")
            AccountAvailability(
                ok = true,
                exists = data["exists"] == true,
                available = data["available"] != false,
                name = data.str("name", clean),
                message = if (data["available"] == false) "That name is already taken." else "“$clean” is available.",
            )
        } catch (_: Exception) {
            AccountAvailability(
                ok = false,
                exists = false,
                available = true,
                name = clean,
                message = "Could not check availability. You can still try creating the account.",
            )
        }
    }

    fun authenticatePreview() {
        val current = session
        if (current != null && current.sessionKind != "preview") return
        val now = System.currentTimeMillis()
        saveSession(
            AuthSession(
                nodeName = "preview-node",
                email = "preview@forkmesh.local",
                status = "active",
                pubkey = identity()?.publicKeyB64url ?: "",
                emailVerified = true,
                isAdmin = false,
                solana = "",
                hasPayoutAddress = false,
                avatarPng = "",
                avatarUpdatedAt = 0,
                createdAt = now,
                savedAt = now,
                sessionKind = "preview",
                deviceKind = "mobile",
                capabilities = listOf("browse", "comment", "submit_issue", "submit_pr"),
            ),
        )
    }

    fun deleteAccount(password: String) {
        val current = session
        if (current == null || current.sessionKind == "preview") {
            throw AuthException("not_authenticated", "Sign in with a real account before deleting it.")
        }
        val cleanPassword = password.trim()
        if (cleanPassword.isEmpty()) {
            throw AuthException("password_required", "Enter your current password to delete this account.")
        }
        postJson(
            "/api/accounts/profile",
            jsonOf(
                "nodeName" to current.nodeName,
                "email" to current.email,
                "sessionToken" to current.sessionToken,
                "password" to cleanPassword,
                "deleteAccount" to true,
            ),
        )
        clearLocalSession()
    }

    fun logout() {
        val sessionToken = session?.sessionToken ?: ""
        clearLocalSession()
        try {
            val payload = jsonOf()
            if (sessionToken.isNotEmpty()) payload["sessionToken"] = sessionToken
            postJson("/api/accounts/logout", payload, timeoutMs = REVOKE_TIMEOUT_MS)
        } catch (_: Exception) {
        }
    }

    private fun clearLocalSession() {
        synchronized(lock) { session = null }
        notifyListeners()
        runBounded(STORE_TIMEOUT_MS) { sessionStore.delete(sessionKey) }
        prefs.remove(SESSION_KEY)
    }

    private fun clearStoredSeed() {
        secrets.delete(IDENTITY_SEED_KEY)
        prefs.remove(IDENTITY_SEED_KEY)
    }

    private fun loadSession(): AuthSession? {
        val raw = sessionStore.read(sessionKey)
        if (raw.isNullOrEmpty()) return null
        try {
            val json = Json.parse(raw)
            if (json is Map<*, *>) {
                @Suppress("UNCHECKED_CAST")
                val restored = AuthSession.fromJson(json as Map<String, Any?>)
                if (restored.sessionKind == "preview") {
                    runCatching { sessionStore.delete(sessionKey) }
                    return null
                }
                if (restored.nodeName.isNotEmpty() || restored.email.isNotEmpty()) return restored
            }
        } catch (_: Exception) {
        }
        return null
    }

    private fun saveSession(newSession: AuthSession): AuthSession {
        sessionStore.write(sessionKey, Json.stringify(newSession.toJson()))
        prefs.remove(SESSION_KEY)
        prefs.remove(guestKey)
        if (newSession.nodeName.isNotEmpty()) settings.setDisplayName(newSession.nodeName)
        if (newSession.solana.isNotEmpty()) settings.setSolanaAddress(newSession.solana)
        synchronized(lock) { session = newSession }
        notifyListeners()
        return newSession
    }

    fun refreshAvatar(): Boolean {
        val current = session ?: return false
        if (current.sessionKind == "preview" || current.nodeName.isEmpty()) return false
        val name = java.net.URLEncoder.encode(current.nodeName, "UTF-8").replace("+", "%20")
        val data = getJson("/api/accounts/$name")
        if (data["exists"] == false || !data.containsKey("avatarPng")) return false
        val avatar = data["avatarPng"]?.toString() ?: ""
        if (avatar == current.avatarPng) return false
        val latest = session
        if (latest == null || latest.nodeName != current.nodeName) return false
        val updatedAt = (data["avatarUpdatedAt"] as? Number)?.toLong() ?: latest.avatarUpdatedAt
        saveSession(latest.copy(avatarPng = avatar, avatarUpdatedAt = updatedAt))
        return true
    }

    private fun getJson(path: String): Map<String, Any?> {
        val request = HttpClient.Request("GET", base(path))
            .header("accept", "application/json")
            .timeouts(CONNECT_TIMEOUT_MS, REQUEST_TIMEOUT_MS)
        return decodeResponse(transport.send(request))
    }

    private fun postJson(path: String, payload: Map<String, Any?>, timeoutMs: Int = REQUEST_TIMEOUT_MS): Map<String, Any?> {
        val request = HttpClient.Request("POST", base(path))
            .header("content-type", "application/json")
            .header("accept", "application/json")
            .body(Json.stringify(payload).toByteArray(Charsets.UTF_8), null)
            .timeouts(minOf(CONNECT_TIMEOUT_MS, timeoutMs), timeoutMs)
        return decodeResponse(transport.send(request))
    }

    private fun decodeResponse(response: HttpClient.Response): Map<String, Any?> {
        var body: Map<String, Any?> = emptyMap()
        try {
            val decoded = Json.parse(response.text())
            @Suppress("UNCHECKED_CAST")
            if (decoded is Map<*, *>) body = decoded as Map<String, Any?>
        } catch (_: Exception) {
        }
        if (response.status in 200..299) return body
        val code = body.str("error", "http_${response.status}")
        throw AuthException(code, authErrorMessage(code))
    }

    companion object {
        const val SESSION_KEY = "auth/session"

        const val REVOKED_BANNER_KEY = "auth/revokedBanner"
        const val REVOKED_BANNER_MESSAGE = "This device was signed out from the web."

        const val MOBILE_DEVICE_KIND = "mobile_node"
        const val DEVICE_BIND_PREFIX = "forkmesh-device-bind-v1"
        val NODE_NAME_PATTERN = Regex("^[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?$")

        const val GUEST_MODE_KEY = "auth/guestMode"

        const val IDENTITY_SEED_KEY = "identity/ed25519Seed"

        private const val REQUEST_TIMEOUT_MS = 20_000
        private const val CONNECT_TIMEOUT_MS = 15_000
        private const val REVOKE_TIMEOUT_MS = 5_000
        private const val STORE_TIMEOUT_MS = 5_000L

        fun create(
            settings: AccountSettings,
            prefs: PrefStore,
            secrets: SecretStore,
            identity: () -> DeviceIdentity?,
            transport: HttpTransport = HttpTransport.DEFAULT,
            deviceLabelProvider: () -> String = { "Mobile device" },
            deviceTimestampProvider: () -> String = { System.currentTimeMillis().toString() },
            projectId: String = "",
            sessionSecrets: SecretStore? = null,
        ): AuthController {
            val sessionStore = sessionSecrets ?: secrets
            migrateSecretsFromPrefs(sessionStore, prefs, listOf(SESSION_KEY))
            if (projectId.isNotEmpty()) migrateUnscopedSession(sessionStore, prefs, projectId)
            val controller = AuthController(
                settings, prefs, secrets, identity, transport,
                deviceLabelProvider = deviceLabelProvider,
                deviceTimestampProvider = deviceTimestampProvider,
                projectId = projectId,
                sessionSecrets = sessionSecrets,
            )
            controller.session = controller.loadSession()
            return controller
        }

        fun sessionKeyFor(projectId: String): String = if (projectId.isEmpty()) SESSION_KEY else "$SESSION_KEY/$projectId"

        fun guestModeKeyFor(projectId: String): String = if (projectId.isEmpty()) GUEST_MODE_KEY else "$GUEST_MODE_KEY/$projectId"

        fun revokedBannerKeyFor(projectId: String): String = if (projectId.isEmpty()) REVOKED_BANNER_KEY else "$REVOKED_BANNER_KEY/$projectId"

        fun migrateUnscopedSession(sessionStore: SecretStore, prefs: PrefStore, projectId: String) {
            val legacy = sessionStore.read(SESSION_KEY)
            if (!legacy.isNullOrEmpty()) {
                sessionStore.write(sessionKeyFor(projectId), legacy)
                if (sessionStore.read(sessionKeyFor(projectId)) == legacy) {
                    sessionStore.delete(SESSION_KEY)
                    prefs.remove(SESSION_KEY)
                }
            }
            if (prefs.getBool(GUEST_MODE_KEY) == true) {
                prefs.putBool(guestModeKeyFor(projectId), true)
                prefs.remove(GUEST_MODE_KEY)
            }
            if (prefs.getBool(REVOKED_BANNER_KEY) == true) {
                prefs.putBool(revokedBannerKeyFor(projectId), true)
                prefs.remove(REVOKED_BANNER_KEY)
            }
        }

        fun readSession(sessionStore: SecretStore, projectId: String): AuthSession? {
            val raw = runCatching { sessionStore.read(sessionKeyFor(projectId)) }.getOrNull()
            if (raw.isNullOrEmpty()) return null
            return runCatching {
                @Suppress("UNCHECKED_CAST")
                val restored = AuthSession.fromJson(Json.parse(raw) as Map<String, Any?>)
                restored.takeIf { it.sessionKind != "preview" && (it.nodeName.isNotEmpty() || it.email.isNotEmpty()) }
            }.getOrNull()
        }

        fun forgetProject(sessionStore: SecretStore, prefs: PrefStore, projectId: String) {
            if (projectId.isEmpty()) return
            runCatching { sessionStore.delete(sessionKeyFor(projectId)) }
            prefs.remove(guestModeKeyFor(projectId))
            prefs.remove(revokedBannerKeyFor(projectId))
        }

        fun isSignedInOn(sessionStore: SecretStore, prefs: PrefStore, projectId: String): Boolean =
            readSession(sessionStore, projectId) != null || prefs.getBool(guestModeKeyFor(projectId)) == true

        private fun runBounded(timeoutMs: Long, block: () -> Unit) {
            val worker = Thread({
                try {
                    block()
                } catch (_: Exception) {
                }
            }, "AuthController-store")
            worker.isDaemon = true
            worker.start()
            try {
                worker.join(timeoutMs)
            } catch (_: InterruptedException) {
                Thread.currentThread().interrupt()
            }
        }
    }
}
