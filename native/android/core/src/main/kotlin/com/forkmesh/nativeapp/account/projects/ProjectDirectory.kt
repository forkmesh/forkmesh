package com.forkmesh.nativeapp.account.projects

import com.forkmesh.nativeapp.api.AccountSettings
import com.forkmesh.nativeapp.api.AppEnv
import com.forkmesh.nativeapp.api.PrefStore
import com.forkmesh.nativeapp.json.Json
import java.util.concurrent.CopyOnWriteArrayList

class ProjectDirectory private constructor(
    private val prefs: PrefStore,
    private val env: AppEnv,
    private val clock: () -> Long,
    private val canonicalize: (String) -> String,
) {
    companion object {
        const val LIST_KEY = "relays/list"
        const val ACTIVE_KEY = "relays/active"
        const val MIGRATED_KEY = "relays/migratedFromServerUrl"

        fun load(
            prefs: PrefStore,
            env: AppEnv = AppEnv.PROD,
            legacyInstall: Boolean = false,
            clock: () -> Long = System::currentTimeMillis,
            canonicalize: (String) -> String = AccountSettings::canonicalServerUrl,
        ): ProjectDirectory {
            val directory = ProjectDirectory(prefs, env, clock, canonicalize)
            directory.restore(legacyInstall)
            return directory
        }
    }

    @Volatile
    private var list: List<Project> = emptyList()

    @Volatile
    private var activeIdValue: String = ""

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

    val projects: List<Project> get() = list

    val active: Project get() = list.firstOrNull { it.id == activeIdValue } ?: list.first()

    val activeId: String get() = active.id

    val activeServerUrl: String get() = active.serverUrl

    val hasMultiple: Boolean get() = list.size > 1

    fun byId(id: String): Project? = list.firstOrNull { it.id == id }

    fun contains(id: String): Boolean = byId(id) != null

    @Synchronized
    private fun restore(legacyInstall: Boolean) {
        val raw = prefs.getString(LIST_KEY) ?: ""
        val parsed = ArrayList<Project>()
        if (raw.isNotEmpty()) {
            runCatching {
                val decoded = Json.parse(raw)
                if (decoded is List<*>) {
                    for (item in decoded) {
                        @Suppress("UNCHECKED_CAST")
                        val map = item as? Map<String, Any?> ?: continue
                        val project = Project.fromJson(map)
                        if (project.id.isNotEmpty() && parsed.none { it.id == project.id }) parsed.add(project)
                    }
                }
            }
        }
        val hadList = parsed.isNotEmpty()
        list = parsed
        adoptLegacyServerUrl()
        ensureBuiltIn()
        val stored = prefs.getString(ACTIVE_KEY) ?: ""
        activeIdValue = when {
            stored.isNotEmpty() && contains(stored) -> stored
            else -> list.first().id
        }
        persist()
    }

    private fun adoptLegacyServerUrl() {
        if (prefs.getBool(MIGRATED_KEY) == true) return
        prefs.putBool(MIGRATED_KEY, true)
        val raw = prefs.getString(AccountSettings.SERVER_URL_KEY)?.trim() ?: ""
        if (raw.isEmpty()) return
        val stored = canonicalize(raw)
        val project = Project.tryParse(stored, now = clock()) ?: return
        if (contains(project.id)) return
        if (Project.normalizeOrigin(env.defaultServerUrl) == project.id) return
        list = list + project.copy(serverUrl = stored, namespace = "")
        if ((prefs.getString(ACTIVE_KEY) ?: "").isEmpty()) prefs.putString(ACTIVE_KEY, project.id)
    }

    private fun builtInLabel(project: Project): String {
        if (project.label != project.id) return project.label
        if (project.id == ProjectDefaults.PROD_HOST) return ProjectDefaults.PROD_LABEL
        return project.label
    }

    private fun ensureBuiltIn() {
        val defaultUrl = env.defaultServerUrl
        val id = Project.normalizeOrigin(defaultUrl)
        if (id == null) {
            if (list.isEmpty()) list = listOf(ProjectDefaults.production(clock()))
            ensureFixedProjects()
            return
        }
        if (byId(id) != null) {
            list = list.map { project ->
                when {
                    project.id == id -> project.copy(serverUrl = defaultUrl, builtIn = true, label = builtInLabel(project))
                    project.builtIn && !ProjectDefaults.isFixed(project.id) -> project.copy(builtIn = false)
                    else -> project
                }
            }
            ensureFixedProjects()
            return
        }
        val builtIn = if (id == ProjectDefaults.PROD_HOST) {
            ProjectDefaults.production(clock())
        } else {
            Project(
                id = id,
                label = id,
                serverUrl = defaultUrl,
                addedAt = clock(),
                builtIn = true,
                namespace = ProjectScope.namespaceFor(id),
            )
        }
        list = listOf(builtIn) + list.filterNot { it.builtIn && !ProjectDefaults.isFixed(it.id) && it.lastUsedAt == 0L && it.label == it.id }
            .map { it.copy(builtIn = ProjectDefaults.isFixed(it.id)) }
        ensureFixedProjects()
    }

    private fun ensureFixedProjects() {
        for (fixed in listOf(ProjectDefaults.production(clock()))) {
            if (byId(fixed.id) == null) {
                list = list + fixed
                continue
            }
            list = list.map { project ->
                if (project.id != fixed.id) {
                    project
                } else {
                    project.copy(
                        builtIn = true,
                        label = builtInLabel(project),
                        worldUrl = project.worldUrl.ifEmpty { fixed.worldUrl },
                        homepageUrl = project.homepageUrl.ifEmpty { fixed.homepageUrl },
                    )
                }
            }
        }
        list = list.filter { it.builtIn } + list.filterNot { it.builtIn }
    }

    @Synchronized
    fun add(project: Project): Project {
        val existing = byId(project.id)
        if (existing != null) {
            val updated = existing.copy(verifiedAt = project.verifiedAt, verifiedRev = project.verifiedRev)
            replace(updated)
            return updated
        }
        list = list + project
        persist()
        notifyListeners()
        return project
    }

    @Synchronized
    fun addNamed(name: String, logo: String = ""): Project? {
        val project = Project.named(name, clock())?.copy(logo = logo) ?: return null
        list = list + project
        persist()
        notifyListeners()
        return project
    }

    @Synchronized
    fun attach(id: String, instance: Project): Project? {
        val project = byId(id) ?: return null
        if (project.hasInstance || !instance.hasInstance || instance.serverUrl.isEmpty()) return null
        val attached = project.attach(instance)
        replace(attached)
        return attached
    }

    fun setLogo(id: String, logo: String) {
        val project = byId(id) ?: return
        replace(project.copy(logo = logo))
    }

    @Synchronized
    fun useUrl(url: String): Project? {
        val clean = canonicalize(url.trim())
        if (clean.isEmpty()) return null
        val id = Project.normalizeOrigin(clean) ?: return null
        val existing = byId(id)
        val project = existing?.copy(serverUrl = clean) ?: Project(
            id = id,
            label = id,
            serverUrl = clean,
            addedAt = clock(),
            namespace = ProjectScope.namespaceFor(id),
        )
        list = if (existing == null) list + project else list.map { if (it.id == id) project else it }
        activeIdValue = id
        persist()
        notifyListeners()
        return project
    }

    fun rename(id: String, label: String) {
        val project = byId(id) ?: return
        replace(project.copy(label = label.trim().ifEmpty { project.id }))
    }

    fun update(id: String, label: String? = null, worldUrl: String? = null): Project? {
        val project = byId(id) ?: return null
        val cleanLabel = label?.trim()
        val updated = project.copy(
            label = if (cleanLabel == null) project.label else cleanLabel.ifEmpty { project.id },
            worldUrl = if (worldUrl == null || project.builtIn) project.worldUrl else Project.normalizeWorldUrl(worldUrl),
        )
        replace(updated)
        return updated
    }

    fun markFeatureMissing(id: String, feature: RelayFeature) {
        val project = byId(id) ?: return
        val next = project.features.markMissing(feature)
        if (next == project.features) return
        replace(project.copy(features = next))
    }

    val activeFeatures: RelayFeatureSet get() = active.features

    fun markVerified(id: String, rev: String = "", features: RelayFeatureSet? = null) {
        val project = byId(id) ?: return
        replace(
            project.copy(
                verifiedAt = clock(),
                verifiedRev = rev.ifEmpty { project.verifiedRev },
                features = features ?: project.features,
            ),
        )
    }

    @Synchronized
    fun setActive(id: String) {
        if (!contains(id) || id == activeIdValue) return
        activeIdValue = id
        list = list.map { if (it.id == id) it.copy(lastUsedAt = clock()) else it }
        persist()
        notifyListeners()
    }

    @Synchronized
    fun remove(id: String): Boolean {
        val project = byId(id) ?: return false
        if (project.builtIn || id == activeIdValue) return false
        list = list.filterNot { it.id == id }
        persist()
        notifyListeners()
        return true
    }

    fun fallbackFor(id: String): Project {
        val others = list.filter { it.id != id }.sortedByDescending { it.lastUsedAt }
        if (others.isEmpty()) return active
        return others.firstOrNull { it.lastUsedAt > 0 } ?: others.firstOrNull { it.builtIn } ?: others.first()
    }

    @Synchronized
    private fun replace(project: Project) {
        list = list.map { if (it.id == project.id) project else it }
        persist()
        notifyListeners()
    }

    private fun persist() {
        prefs.putString(LIST_KEY, Json.stringify(list.map { it.toJson() }))
        prefs.putString(ACTIVE_KEY, activeIdValue)
    }
}
