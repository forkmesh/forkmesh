package com.forkmesh.nativeapp.account.projects

import com.forkmesh.nativeapp.api.AppEnv
import com.forkmesh.nativeapp.api.DartUri
import com.forkmesh.nativeapp.api.PrefStore
import com.forkmesh.nativeapp.api.ServerUrl
import com.forkmesh.nativeapp.security.SecretStore

enum class RelayFeature(val core: Boolean, val label: String) {
    accounts(true, "Accounts"),
    repositories(true, "Repositories"),
    repoRead(true, "Code browsing"),
    chat(false, "Chat"),
    notifications(false, "Notifications"),
    push(false, "Push notifications"),
    accountEvents(false, "Live account events"),
    nodeSync(false, "Serving as a node"),
    issues(false, "Issues"),
    pulls(false, "Pull requests"),
    agents(false, "Agents"),
    actions(false, "Actions"),
    orgs(false, "Organizations"),
    devices(false, "Account devices"),
    imports(false, "Repository import"),
    bugReports(false, "Bug reports"),
    ads(false, "Sponsored messages"),
    networkStats(false, "Network stats"),
    plugins(false, "Plugins"),
    bounties(false, "Bounties"),
    ownerKeys(false, "Owner signing"),
    userStatus(false, "Status"),
    iosUdid(false, "UDID lookup"),
    ;

    val wireName: String get() = name

    companion object {
        fun fromWireName(value: String): RelayFeature? = entries.firstOrNull { it.wireName == value.trim() }

        val coreFeatures: Set<RelayFeature> get() = entries.filter { it.core }.toSet()

        private val learnablePrefixes = linkedMapOf(
            "/api/notifications" to notifications,
            "/api/accounts/register-device" to push,
            "/api/accounts/push-test" to push,
            "/api/accounts/event-ticket" to accountEvents,
            "/api/nodes/events" to accountEvents,
            "/api/sync" to nodeSync,
            "/api/orgs" to orgs,
            "/api/accounts/devices" to devices,
            "/api/accounts/user-status" to userStatus,
            "/api/repository-imports" to imports,
            "/api/bug-reports" to bugReports,
            "/api/ads" to ads,
            "/api/network" to networkStats,
            "/api/calories" to plugins,
            "/api/notes" to plugins,
            "/api/tasks" to plugins,
            "/api/security/owner-keys" to ownerKeys,
            "/api/chat/room-key" to chat,
            "/api/ios-udid" to iosUdid,
        )

        fun learnableFromPath(path: String): RelayFeature? {
            val clean = path.lowercase()
            if (clean.startsWith("/api/repo/")) return null
            for ((prefix, feature) in learnablePrefixes) {
                if (clean == prefix || clean.startsWith("$prefix/") || clean.startsWith("$prefix?")) return feature
            }
            return null
        }
    }
}

class RelayFeatureSet private constructor(
    private val declared: Set<RelayFeature>?,
    private val knownMissing: Set<RelayFeature>,
) {
    val isDeclared: Boolean get() = declared != null

    fun has(feature: RelayFeature): Boolean = declared?.contains(feature) ?: !knownMissing.contains(feature)

    val hasCore: Boolean get() = RelayFeature.coreFeatures.all(::has)

    val missingCore: Set<RelayFeature> get() = RelayFeature.coreFeatures.filterNot(::has).toSet()

    fun markMissing(feature: RelayFeature): RelayFeatureSet {
        if (isDeclared || feature in knownMissing) return this
        return undeclared(knownMissing + feature)
    }

    fun toJson(): List<String> =
        declared?.map { it.wireName } ?: knownMissing.map { "!" + it.wireName }

    override fun equals(other: Any?): Boolean =
        other is RelayFeatureSet && other.declared == declared && other.knownMissing == knownMissing

    override fun hashCode(): Int = (declared?.hashCode() ?: 0) * 31 + knownMissing.hashCode()

    companion object {
        fun declared(features: Set<RelayFeature>) = RelayFeatureSet(features.toSet(), emptySet())

        fun undeclared(knownMissing: Set<RelayFeature> = emptySet()) = RelayFeatureSet(null, knownMissing.toSet())

        fun fromJson(value: Any?): RelayFeatureSet {
            if (value !is List<*>) return undeclared()
            val missing = LinkedHashSet<RelayFeature>()
            val declared = LinkedHashSet<RelayFeature>()
            for (item in value) {
                val raw = item.toString()
                if (raw.startsWith("!")) {
                    RelayFeature.fromWireName(raw.substring(1))?.let(missing::add)
                } else {
                    RelayFeature.fromWireName(raw)?.let(declared::add)
                }
            }
            if (declared.isEmpty() && missing.isNotEmpty()) return undeclared(missing)
            if (declared.isEmpty()) return undeclared()
            return declared(declared)
        }

        fun fromVersionBody(body: Map<String, Any?>): RelayFeatureSet {
            val raw = body["features"] as? List<*> ?: return undeclared()
            return declared(raw.mapNotNull { RelayFeature.fromWireName(it.toString()) }.toSet())
        }
    }
}

data class ProjectScope(val projectId: String, val namespace: String) {
    val isLegacy: Boolean get() = namespace.isEmpty()

    fun key(name: String): String = namespace + name

    val slug: String get() = slugFor(projectId)

    fun directory(base: java.io.File): java.io.File =
        if (isLegacy) base else java.io.File(java.io.File(base, "projects"), slug)

    fun prefs(base: PrefStore): PrefStore = if (isLegacy) base else ScopedPrefStore(base, namespace)

    fun secrets(base: SecretStore): SecretStore = if (isLegacy) base else ScopedSecretStore(base, namespace)

    companion object {
        val legacy = ProjectScope("", "")

        fun namespaceFor(projectId: String): String = "p/$projectId/"

        fun slugFor(projectId: String): String = projectId.lowercase().replace(Regex("[^a-z0-9._-]"), "_")
    }
}

class ScopedPrefStore(private val base: PrefStore, private val prefix: String) : PrefStore {
    override fun getString(key: String): String? = base.getString(prefix + key)

    override fun putString(key: String, value: String) = base.putString(prefix + key, value)

    override fun getBool(key: String): Boolean? = base.getBool(prefix + key)

    override fun putBool(key: String, value: Boolean) = base.putBool(prefix + key, value)

    override fun getDouble(key: String): Double? = base.getDouble(prefix + key)

    override fun putDouble(key: String, value: Double) = base.putDouble(prefix + key, value)

    override fun contains(key: String): Boolean = base.contains(prefix + key)

    override fun remove(key: String) = base.remove(prefix + key)
}

class ScopedSecretStore(val base: SecretStore, val prefix: String) : SecretStore {
    override fun read(key: String): String? = base.read(prefix + key)

    override fun write(key: String, value: String) = base.write(prefix + key, value)

    override fun delete(key: String) = base.delete(prefix + key)
}

data class Project(
    val id: String,
    val label: String,
    val serverUrl: String,
    val addedAt: Long = 0,
    val lastUsedAt: Long = 0,
    val verifiedAt: Long = 0,
    val verifiedRev: String = "",
    val builtIn: Boolean = false,
    val features: RelayFeatureSet = RelayFeatureSet.undeclared(),
    val worldUrl: String = "",
    val homepageUrl: String = "",
    val namespace: String = "",
    val logo: String = "",
    val hasInstance: Boolean = true,
) {
    val scope: ProjectScope get() = ProjectScope(id, namespace)

    val host: String get() = ServerUrl.parse(serverUrl).host.ifEmpty { id }

    val isLoopback: Boolean get() = AppEnv.isLoopbackHost(host)

    val subtitle: String
        get() = when {
            !hasInstance -> NO_INSTANCE_LABEL
            isLoopback -> "$id · on this network"
            else -> id
        }

    val httpOrigin: String
        get() {
            if (!hasInstance) return ""
            val parsed = ServerUrl.parse(serverUrl)
            if (parsed.host.isEmpty()) return "https://$id"
            return parsed.httpOrigin
        }

    val worldUri: String
        get() {
            val stored = worldUrl.trim()
            if (stored.isNotEmpty() && ServerUrl.parse(stored).host.isNotEmpty()) return stored
            val derived = deriveWorldHost(host)
            if (derived.isEmpty()) return ProjectDefaults.PROD_WORLD_URL
            return "https://$derived/"
        }

    val homepageUri: String
        get() {
            val stored = homepageUrl.trim()
            if (stored.isNotEmpty() && ServerUrl.parse(stored).host.isNotEmpty()) return stored
            val labels = host.split('.')
            if (labels.size >= 3 && (labels.first() == "app" || labels.first() == "api")) {
                return "https://" + labels.drop(1).joinToString(".") + "/"
            }
            return "$httpOrigin/"
        }

    fun toJson(): Map<String, Any?> = linkedMapOf<String, Any?>(
        "id" to id,
        "label" to label,
        "serverUrl" to serverUrl,
        "addedAt" to addedAt,
        "lastUsedAt" to lastUsedAt,
        "verifiedAt" to verifiedAt,
        "verifiedRev" to verifiedRev,
        "builtIn" to builtIn,
        "features" to features.toJson(),
        "worldUrl" to worldUrl,
        "homepageUrl" to homepageUrl,
        "namespace" to namespace,
    ).apply {
        if (logo.isNotEmpty()) put("logo", logo)
        if (!hasInstance) put("hasInstance", false)
    }

    fun attach(instance: Project): Project = copy(
        serverUrl = instance.serverUrl,
        verifiedAt = instance.verifiedAt,
        verifiedRev = instance.verifiedRev,
        features = instance.features,
        worldUrl = instance.worldUrl,
        homepageUrl = instance.homepageUrl,
        hasInstance = true,
    )

    override fun equals(other: Any?): Boolean = other is Project && other.id == id

    override fun hashCode(): Int = id.hashCode()

    fun sameContent(other: Project): Boolean = toJson() == other.toJson()

    companion object {
        const val NO_INSTANCE_LABEL = "No instance attached"

        fun named(name: String, now: Long = System.currentTimeMillis()): Project? {
            val clean = name.trim()
            if (clean.isEmpty()) return null
            val slug = ProjectScope.slugFor(clean).take(32)
            val suffix = java.util.UUID.randomUUID().toString().replace("-", "").take(8)
            val id = "local-$slug-$suffix"
            return Project(
                id = id,
                label = clean,
                serverUrl = "",
                addedAt = now,
                namespace = ProjectScope.namespaceFor(id),
                hasInstance = false,
            )
        }

        private val HOST_PATTERN = Regex(
            "^(?:[a-z0-9](?:[a-z0-9-]*[a-z0-9])?(?:\\.[a-z0-9](?:[a-z0-9-]*[a-z0-9])?)*|\\[[0-9a-f:]+])$",
        )
        private val URL_PARTS = Regex("^([a-zA-Z][a-zA-Z0-9+.\\-]*)://([^/?#]*)")

        fun deriveWorldHost(appHost: String): String {
            val host = appHost.trim().lowercase()
            if (host.isEmpty() || AppEnv.isLoopbackHost(host)) return ""
            if (Regex("^[0-9.]+$").matches(host) || host.startsWith("[")) return ""
            val labels = host.split('.')
            if (labels.size < 2) return ""
            if (labels.size == 2) return "world.$host"
            return "world." + labels.drop(1).joinToString(".")
        }

        fun normalizeOrigin(input: String): String? {
            var raw = input.trim()
            if (raw.isEmpty()) return null
            if (!raw.contains("://")) raw = "https://$raw"
            val match = URL_PARTS.find(raw) ?: return null
            val scheme = match.groupValues[1].lowercase()
            val authority = match.groupValues[2].substringAfterLast('@')
            var host: String
            var portText = ""
            if (authority.startsWith("[")) {
                val close = authority.indexOf(']')
                if (close < 0) return null
                host = authority.substring(0, close + 1)
                if (close + 1 < authority.length) {
                    if (authority[close + 1] != ':') return null
                    portText = authority.substring(close + 2)
                }
            } else {
                val colon = authority.lastIndexOf(':')
                if (colon >= 0) {
                    host = authority.substring(0, colon)
                    portText = authority.substring(colon + 1)
                } else {
                    host = authority
                }
            }
            host = DartUri.decodePercent(host).trim().lowercase()
            if (host.isEmpty() || !HOST_PATTERN.matches(host)) return null
            if (portText.isEmpty()) return host
            val port = portText.toIntOrNull() ?: return null
            if (port < 0 || port > 65535) return null
            val defaultPort = mapOf("https" to 443, "wss" to 443, "http" to 80, "ws" to 80)[scheme]
            if (port == defaultPort) return host
            return "$host:$port"
        }

        fun normalizeWorldUrl(input: String): String {
            val origin = normalizeOrigin(input) ?: return ""
            return "https://$origin/"
        }

        fun tryParse(input: String, label: String = "", worldUrl: String = "", now: Long = System.currentTimeMillis()): Project? {
            val origin = normalizeOrigin(input) ?: return null
            return Project(
                id = origin,
                label = label.trim().ifEmpty { origin },
                serverUrl = AppEnv.relayUrlForHost(origin),
                addedAt = now,
                worldUrl = normalizeWorldUrl(worldUrl),
                namespace = ProjectScope.namespaceFor(origin),
            )
        }

        fun fromJson(json: Map<String, Any?>): Project {
            val id = (json["id"] ?: "").toString()
            val url = (json["serverUrl"] ?: "").toString()
            val label = (json["label"] ?: "").toString()
            val hasInstance = json["hasInstance"] != false
            return Project(
                id = id,
                label = label.ifEmpty { id },
                serverUrl = if (!hasInstance) "" else url.ifEmpty { AppEnv.relayUrlForHost(id) },
                addedAt = asLong(json["addedAt"]),
                lastUsedAt = asLong(json["lastUsedAt"]),
                verifiedAt = asLong(json["verifiedAt"]),
                verifiedRev = (json["verifiedRev"] ?: "").toString(),
                builtIn = json["builtIn"] == true,
                features = RelayFeatureSet.fromJson(json["features"]),
                worldUrl = (json["worldUrl"] ?: "").toString(),
                homepageUrl = (json["homepageUrl"] ?: "").toString(),
                namespace = (json["namespace"] ?: "").toString(),
                logo = (json["logo"] ?: "").toString(),
                hasInstance = hasInstance,
            )
        }

        private fun asLong(value: Any?): Long = when (value) {
            is Number -> value.toLong()
            else -> value?.toString()?.toLongOrNull() ?: 0L
        }
    }
}

object ProjectDefaults {
    const val PROD_HOST = AppEnv.PROD_HOST
    const val NEXT_HOST = "app.next.forkmesh.com"
    const val PROD_LABEL = "ForkMesh"
    const val PROD_WORLD_URL = "https://world.forkmesh.com/"
    const val PROD_HOMEPAGE_URL = "https://forkmesh.com/"
    val PROD_HOST_ALIASES = setOf("forkmesh.com", "api.forkmesh.com", PROD_HOST)

    fun isFixed(id: String): Boolean = id == PROD_HOST

    fun isNextHost(id: String): Boolean {
        val host = id.substringBefore(':').trim().lowercase()
        return host == NEXT_HOST || host.startsWith("next.") || host.endsWith(".next.forkmesh.com")
    }

    fun canonicalId(id: String): String = if (isNextHost(id)) PROD_HOST else id

    fun production(now: Long) = Project(
        id = PROD_HOST,
        label = PROD_LABEL,
        serverUrl = AppEnv.PROD_SERVER_URL,
        addedAt = now,
        builtIn = true,
        worldUrl = PROD_WORLD_URL,
        homepageUrl = PROD_HOMEPAGE_URL,
    )
}

object LegalLinks {
    private fun withPath(homepage: String, path: String, fragment: String = ""): String {
        val origin = ServerUrl.parse(homepage).let { parsed ->
            if (parsed.host.isEmpty()) ProjectDefaults.PROD_HOMEPAGE_URL.trimEnd('/') else parsed.httpOrigin
        }
        return origin + path + if (fragment.isEmpty()) "" else "#$fragment"
    }

    fun termsFor(homepage: String): String = withPath(homepage, "/terms")

    fun privacyFor(homepage: String): String = withPath(homepage, "/privacy")

    fun membershipFor(homepage: String): String = withPath(homepage, "/", "membership")

    fun designFor(homepage: String): String = withPath(homepage, "/design")

    fun desktopFor(homepage: String): String = withPath(homepage, "/desktop")
}

fun loginSubtitle(projects: List<Project>, active: Project?): String {
    if (projects.size < 2 || active == null) return "Continue with your ForkMesh account."
    if (!active.hasInstance) return "${active.label} is not connected to an instance yet."
    return "Signing in to ${active.label} (${active.id}). Your accounts in your other projects are untouched."
}
