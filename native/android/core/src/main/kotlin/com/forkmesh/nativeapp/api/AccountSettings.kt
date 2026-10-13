package com.forkmesh.nativeapp.api

import com.forkmesh.nativeapp.security.SecretStore
import java.util.concurrent.CopyOnWriteArrayList

fun migrateSecretsFromPrefs(secrets: SecretStore, prefs: PrefStore, keys: Iterable<String>) {
    for (key in keys) {
        val existing = secrets.read(key)
        if (!existing.isNullOrEmpty()) {
            prefs.remove(key)
            continue
        }
        val legacy = prefs.getString(key)
        if (legacy.isNullOrEmpty()) continue
        secrets.write(key, legacy)
        prefs.remove(key)
    }
}

class AccountSettings(
    private val prefs: PrefStore,
    private val secrets: SecretStore,
    val env: AppEnv = AppEnv.PROD,
    private val releaseMode: Boolean = false,
    private val projectPrefs: PrefStore = prefs,
    private val projectServerUrl: (() -> String)? = null,
    private val useServerUrl: ((String) -> Unit)? = null,
) : ClientSettings {
    @Volatile
    private var passphraseValue = DEFAULT_PASSPHRASE

    @Volatile
    private var githubTokenValue = ""

    @Volatile
    private var gitlabTokenValue = ""

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

    val defaultServerUrl: String get() = env.defaultServerUrl

    val configuredServerUrl: String get() = env.serverUrlOverride

    val displayName: String get() = projectPrefs.getString(DISPLAY_NAME_KEY) ?: ""

    override val serverUrl: String
        get() {
            projectServerUrl?.let { return it() }
            val stored = prefs.getString(SERVER_URL_KEY)?.trim() ?: ""
            if (stored.isEmpty()) return defaultServerUrl
            if (releaseMode) return defaultServerUrl
            return canonicalServerUrl(stored)
        }

    val worldUri: String get() = env.worldSiteUrl

    override val room: String get() = projectPrefs.getString(ROOM_KEY) ?: DEFAULT_ROOM

    val passphrase: String get() = passphraseValue

    val solanaAddress: String get() = projectPrefs.getString(SOLANA_KEY) ?: ""

    val githubToken: String get() = githubTokenValue

    val gitlabToken: String get() = gitlabTokenValue

    fun setDisplayName(value: String) = setProject(DISPLAY_NAME_KEY, value)

    fun setServerUrl(value: String) {
        val clean = value.trim()
        if (clean.isEmpty()) {
            resetServerUrl()
            return
        }
        val route = useServerUrl
        if (route != null) {
            route(clean)
            notifyListeners()
            return
        }
        set(SERVER_URL_KEY, clean)
    }

    val hasServerUrlOverride: Boolean
        get() {
            if (projectServerUrl != null) return false
            val stored = prefs.getString(SERVER_URL_KEY)?.trim() ?: ""
            return stored.isNotEmpty() && canonicalServerUrl(stored) != defaultServerUrl
        }

    fun resetServerUrl() {
        useServerUrl?.let {
            it("")
            notifyListeners()
            return
        }
        prefs.remove(SERVER_URL_KEY)
        notifyListeners()
    }

    fun setRoom(value: String) = setProject(ROOM_KEY, value)

    fun setPassphrase(value: String) {
        val clean = normalizePassphrase(value)
        passphraseValue = clean
        if (clean.isEmpty()) secrets.delete(PASSPHRASE_KEY) else secrets.write(PASSPHRASE_KEY, clean)
        notifyListeners()
    }

    val bugButtonAnchor: Double
        get() {
            val stored = prefs.getDouble(BUG_BUTTON_ANCHOR_KEY)
            if (stored == null || stored.isNaN()) return DEFAULT_BUG_BUTTON_ANCHOR
            return stored.coerceIn(0.0, 1.0)
        }

    val bugButtonOnLeft: Boolean get() = prefs.getBool(BUG_BUTTON_LEFT_KEY) ?: false

    fun setBugButtonPlacement(anchor: Double, onLeft: Boolean) {
        prefs.putDouble(BUG_BUTTON_ANCHOR_KEY, if (anchor.isNaN()) DEFAULT_BUG_BUTTON_ANCHOR else anchor.coerceIn(0.0, 1.0))
        prefs.putBool(BUG_BUTTON_LEFT_KEY, onLeft)
        notifyListeners()
    }

    val serveAsNode: Boolean get() = projectPrefs.getBool(SERVE_AS_NODE_KEY) ?: false

    fun setServeAsNode(value: Boolean) {
        projectPrefs.putBool(SERVE_AS_NODE_KEY, value)
        notifyListeners()
    }

    val showSponsoredMessages: Boolean get() = prefs.getBool(SHOW_SPONSORED_KEY) ?: true

    fun setShowSponsoredMessages(value: Boolean) {
        prefs.putBool(SHOW_SPONSORED_KEY, value)
        notifyListeners()
    }

    val showTelemetryStrip: Boolean get() = prefs.getBool(SHOW_TELEMETRY_STRIP_KEY) ?: true

    fun setShowTelemetryStrip(value: Boolean) {
        prefs.putBool(SHOW_TELEMETRY_STRIP_KEY, value)
        notifyListeners()
    }

    val sponsoredHiddenUntilMs: Long get() = prefs.getString(SPONSORED_HIDDEN_UNTIL_KEY)?.trim()?.toLongOrNull() ?: 0L

    fun sponsoredMessagesHidden(nowMs: Long = System.currentTimeMillis()): Boolean = sponsoredHiddenUntilMs > nowMs

    fun hideSponsoredMessages(nowMs: Long = System.currentTimeMillis()) {
        prefs.putString(SPONSORED_HIDDEN_UNTIL_KEY, (nowMs + SPONSORED_HIDE_MS).toString())
        notifyListeners()
    }

    fun setSolanaAddress(value: String) = setProject(SOLANA_KEY, value)

    fun setGithubToken(value: String) {
        githubTokenValue = value
        if (value.isEmpty()) secrets.delete(GITHUB_TOKEN_KEY) else secrets.write(GITHUB_TOKEN_KEY, value)
        notifyListeners()
    }

    fun setGitlabToken(value: String) {
        gitlabTokenValue = value
        if (value.isEmpty()) secrets.delete(GITLAB_TOKEN_KEY) else secrets.write(GITLAB_TOKEN_KEY, value)
        notifyListeners()
    }

    fun clearSecrets() {
        for (key in SECRET_KEYS) {
            secrets.delete(key)
            prefs.remove(key)
        }
        passphraseValue = DEFAULT_PASSPHRASE
        githubTokenValue = ""
        gitlabTokenValue = ""
        notifyListeners()
    }

    private fun setProject(key: String, value: String) {
        projectPrefs.putString(key, value)
        notifyListeners()
    }

    private fun set(key: String, value: String) {
        prefs.putString(key, value)
        notifyListeners()
    }

    private fun adoptEnvironmentServerUrl() {
        val envName = env.environmentName
        val storedEnv = prefs.getString(SERVER_URL_ENV_KEY)
        if (storedEnv == envName) return
        val stored = prefs.getString(SERVER_URL_KEY)?.trim() ?: ""
        val mismatched = storedEnv != null && stored.isNotEmpty() && AppEnv.isLoopbackUrl(stored) != env.isDev
        if (mismatched) prefs.remove(SERVER_URL_KEY)
        prefs.putString(SERVER_URL_ENV_KEY, envName)
    }

    private fun loadSecrets() {
        passphraseValue = normalizePassphrase(secrets.read(PASSPHRASE_KEY) ?: "")
        githubTokenValue = secrets.read(GITHUB_TOKEN_KEY) ?: ""
        gitlabTokenValue = secrets.read(GITLAB_TOKEN_KEY) ?: ""
    }

    companion object {
        const val SERVER_URL_KEY = "server/url"
        const val LOCAL_WORKER_URL_MIGRATION_KEY = "server/localWorkerUrlMigrated"
        const val SERVER_URL_ENV_KEY = "server/urlEnv"
        const val PASSPHRASE_KEY = "server/passphrase"

        const val ROOM_SEAL_KEY = "server/roomSeal"
        const val GITHUB_TOKEN_KEY = "import/githubToken"
        const val GITLAB_TOKEN_KEY = "import/gitlabToken"
        val SECRET_KEYS = listOf(PASSPHRASE_KEY, ROOM_SEAL_KEY, GITHUB_TOKEN_KEY, GITLAB_TOKEN_KEY)

        const val DISPLAY_NAME_KEY = "profile/name"
        const val ROOM_KEY = "server/room"
        const val SOLANA_KEY = "profile/solana"
        const val BUG_BUTTON_ANCHOR_KEY = "bugReport/anchor"
        const val BUG_BUTTON_LEFT_KEY = "bugReport/onLeft"
        const val SERVE_AS_NODE_KEY = "node/serve"
        const val SHOW_SPONSORED_KEY = "ads/showSponsored"
        const val SHOW_TELEMETRY_STRIP_KEY = "diagnostics/showStrip"
        const val DESIGN_SYSTEM_ON_KEY = "packages/design-system/on"
        const val TOUR_ON_KEY = "packages/tour/on"

        const val SPONSORED_HIDDEN_UNTIL_KEY = "ads/hiddenUntilMs"
        const val SPONSORED_HIDE_MS = 7L * 24 * 60 * 60 * 1000

        const val DEFAULT_ROOM = "world-general"
        const val DEFAULT_PASSPHRASE = ""
        const val LEGACY_PUBLIC_ROOM_PASSPHRASE = "forkmesh-public-room"
        const val DEFAULT_BUG_BUTTON_ANCHOR = 0.72

        const val PRODUCTION_SERVER_URL = AppEnv.PROD_SERVER_URL
        const val LOCAL_WORKER_SERVER_URL = AppEnv.DEV_SERVER_URL
        const val ANDROID_EMULATOR_LOCAL_WORKER_SERVER_URL = AppEnv.ANDROID_EMULATOR_SERVER_URL
        const val WORLD_SITE_URL = "https://world.forkmesh.com/"

        private const val LEGACY_ROOM_PATH = "/api/repo/mainnode/forkmesh/rooms/general/ws"
        private const val CURRENT_ROOM_PATH = "/api/repo/mainnode/forkmesh/rooms/world-general/ws"
        private val CANONICAL_HOSTS = setOf("forkmesh.com", "api.forkmesh.com", "app.forkmesh.com")
        private val URI_PARTS = Regex("^([a-zA-Z][a-zA-Z0-9+.\\-]*):(?://([^/?#]*))?([^?#]*)(?:\\?([^#]*))?(?:#(.*))?$")

        fun create(
            prefs: PrefStore,
            secrets: SecretStore,
            env: AppEnv = AppEnv.PROD,
            releaseMode: Boolean = false,
        ): AccountSettings {
            migrateSecretsFromPrefs(secrets, prefs, SECRET_KEYS)
            secrets.delete(ROOM_SEAL_KEY)
            migrateLocalWorkerServerUrl(prefs)
            val service = AccountSettings(prefs, secrets, env, releaseMode)
            service.adoptEnvironmentServerUrl()
            service.loadSecrets()
            return service
        }

        fun prepareForProjects(prefs: PrefStore, secrets: SecretStore, env: AppEnv = AppEnv.PROD) {
            migrateSecretsFromPrefs(secrets, prefs, SECRET_KEYS)
            secrets.delete(ROOM_SEAL_KEY)
            migrateLocalWorkerServerUrl(prefs)
            AccountSettings(prefs, secrets, env).adoptEnvironmentServerUrl()
        }

        fun forProject(
            prefs: PrefStore,
            projectPrefs: PrefStore,
            projectSecrets: SecretStore,
            serverUrl: () -> String,
            useServerUrl: (String) -> Unit,
            env: AppEnv = AppEnv.PROD,
            releaseMode: Boolean = false,
        ): AccountSettings {
            val service = AccountSettings(prefs, projectSecrets, env, releaseMode, projectPrefs, serverUrl, useServerUrl)
            service.loadSecrets()
            return service
        }

        fun migrateLocalWorkerServerUrl(prefs: PrefStore) {
            if (prefs.getBool(LOCAL_WORKER_URL_MIGRATION_KEY) == true) return
            prefs.putBool(LOCAL_WORKER_URL_MIGRATION_KEY, true)
            val stored = prefs.getString(SERVER_URL_KEY) ?: return
            if (isRetiredLocalWorkerServerUrl(stored)) prefs.remove(SERVER_URL_KEY)
        }

        fun isRetiredLocalWorkerServerUrl(value: String): Boolean =
            canonicalServerUrl(value) in setOf(LOCAL_WORKER_SERVER_URL, ANDROID_EMULATOR_LOCAL_WORKER_SERVER_URL)

        fun canonicalServerUrl(value: String): String {
            val match = URI_PARTS.find(value.trim()) ?: return value
            val scheme = match.groupValues[1].lowercase()
            if (scheme != "ws" && scheme != "wss") return value
            val authority = match.groups[2]?.value
            val path = match.groupValues[3]
            val query = match.groups[4]?.value
            val fragment = match.groups[5]?.value
            var userInfo = ""
            var host = ""
            var port: Int? = null
            if (authority != null) {
                val at = authority.lastIndexOf('@')
                if (at >= 0) userInfo = authority.substring(0, at)
                val hostPort = authority.substring(at + 1)
                var portText = ""
                if (hostPort.startsWith("[")) {
                    val close = hostPort.indexOf(']')
                    if (close < 0) return value
                    host = hostPort.substring(1, close)
                    val rest = hostPort.substring(close + 1)
                    if (rest.isNotEmpty()) {
                        if (!rest.startsWith(":")) return value
                        portText = rest.substring(1)
                    }
                } else {
                    val colon = hostPort.lastIndexOf(':')
                    if (colon >= 0) {
                        host = hostPort.substring(0, colon)
                        portText = hostPort.substring(colon + 1)
                    } else {
                        host = hostPort
                    }
                }
                if (portText.isNotEmpty()) {
                    if (portText.any { it !in '0'..'9' }) return value
                    port = portText.toIntOrNull() ?: return value
                }
                host = host.lowercase()
            }
            val canonicalHost = host in CANONICAL_HOSTS && port == null
            val sharedRoom = path == LEGACY_ROOM_PATH || path == CURRENT_ROOM_PATH
            if (!sharedRoom) return value
            val out = StringBuilder()
            out.append(if (canonicalHost) "wss" else scheme).append("://")
            if (userInfo.isNotEmpty()) out.append(userInfo).append('@')
            val finalHost = if (canonicalHost) "app.forkmesh.com" else host
            out.append(if (finalHost.contains(':')) "[$finalHost]" else finalHost)
            if (port != null) out.append(':').append(port)
            out.append(CURRENT_ROOM_PATH)
            if (query != null) out.append('?').append(query)
            if (fragment != null) out.append('#').append(fragment)
            return out.toString()
        }

        @Suppress("UNUSED_PARAMETER")
        fun worldUriForServerUrl(serverUrl: String): String = WORLD_SITE_URL

        private fun normalizePassphrase(value: String): String =
            if (value == LEGACY_PUBLIC_ROOM_PASSPHRASE) "" else value
    }
}
