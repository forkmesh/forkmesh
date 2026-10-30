package com.forkmesh.nativeapp.market

import com.forkmesh.nativeapp.api.PrefStore
import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.json.JsonMap
import com.forkmesh.nativeapp.json.bool
import com.forkmesh.nativeapp.json.jsonText
import com.forkmesh.nativeapp.json.maps
import com.forkmesh.nativeapp.json.obj
import com.forkmesh.nativeapp.json.str
import com.forkmesh.nativeapp.json.strings
import com.forkmesh.nativeapp.navigation.Route
import java.util.concurrent.CopyOnWriteArrayList

enum class PackageSettingType(val wire: String) {
    BOOLEAN("boolean"), CHOICE("choice");

    companion object {
        fun of(wire: String): PackageSettingType? = entries.firstOrNull { it.wire == wire }
    }
}

data class PackageSettingOption(val value: String, val label: String)

data class PackageSetting(
    val key: String,
    val type: PackageSettingType,
    val label: String,
    val default: Any,
    val options: List<PackageSettingOption> = emptyList(),
) {
    fun accepts(value: Any?): Boolean = when (type) {
        PackageSettingType.BOOLEAN -> value is Boolean
        PackageSettingType.CHOICE -> value is String && options.any { it.value == value }
    }

    fun valueIn(settings: Map<String, Any?>): Any = settings[key]?.takeIf(::accepts) ?: default

    fun optionLabel(value: Any?): String = options.firstOrNull { it.value == value }?.label ?: jsonText(value)

    fun toJson(): Map<String, Any?> = linkedMapOf(
        "key" to key,
        "type" to type.wire,
        "label" to label,
        "default" to default,
        "options" to options.map { linkedMapOf("value" to it.value, "label" to it.label) },
    )

    companion object {
        fun fromJson(json: JsonMap): PackageSetting? {
            val key = json.str("key")
            if (key.isEmpty() || key.length > 63 || !key.all { it.isLetterOrDigit() || it == '_' || it == '-' }) return null
            val type = PackageSettingType.of(json.str("type")) ?: return null
            val options = json.maps("options").take(20).mapNotNull { option ->
                val value = option.str("value")
                if (value.isEmpty()) null else PackageSettingOption(value, PackagesText.bounded(option.str("label"), 80).ifEmpty { value })
            }
            val default: Any = when (type) {
                PackageSettingType.BOOLEAN -> json.bool("default")
                PackageSettingType.CHOICE -> {
                    if (options.isEmpty()) return null
                    json.str("default").takeIf { value -> options.any { it.value == value } } ?: options.first().value
                }
            }
            return PackageSetting(key, type, PackagesText.bounded(json.str("label"), 120).ifEmpty { key }, default, options)
        }
    }
}

data class PackageMonitors(val systems: List<String> = emptyList(), val prefixes: List<String> = emptyList()) {
    val isEmpty: Boolean get() = systems.isEmpty() && prefixes.isEmpty()

    fun covers(systemId: String): Boolean = systemId in systems || prefixes.any { it.isNotEmpty() && systemId.startsWith(it) }

    fun toJson(): Map<String, Any?> = linkedMapOf("systems" to systems, "prefixes" to prefixes)

    companion object {
        fun fromJson(json: JsonMap): PackageMonitors = PackageMonitors(
            json.strings("systems").filter { it.isNotEmpty() }.take(40),
            json.strings("prefixes").filter { it.isNotEmpty() }.take(20),
        )
    }
}

object PackageDependencies {
    fun parse(value: Any?): List<String> = (value as? List<*>).orEmpty().mapNotNull { item ->
        val id = when (item) {
            is String -> item
            is Map<*, *> -> jsonText(item["id"])
            else -> ""
        }
        id.takeIf(PackagesText::isPackageId)
    }.distinct().take(20)
}

object PackagePlaces {
    const val MOBILE = "mobile"
    const val DESKTOP = "desktop"
    const val WEB = "web"
    const val HERE = MOBILE

    val ALL: List<String> = listOf(MOBILE, DESKTOP, WEB)

    fun parse(value: Any?, missing: List<String> = ALL): List<String> {
        val given = (value as? List<*>)?.map(::jsonText) ?: return missing
        return ALL.filter { it in given }.ifEmpty { missing }
    }

    fun label(place: String): String = when (place) {
        MOBILE -> "Phone"
        DESKTOP -> "Desktop"
        WEB -> "Web"
        else -> place
    }
}

data class AccountPackage(
    val id: String,
    val installed: Boolean,
    val settings: Map<String, Any?> = emptyMap(),
    val legacy: Boolean = false,
    val places: List<String> = PackagePlaces.ALL,
    val availablePlaces: List<String> = emptyList(),
) {
    val placedHere: Boolean get() = PackagePlaces.HERE in places

    fun toJson(): Map<String, Any?> =
        linkedMapOf<String, Any?>("id" to id, "installed" to installed, "settings" to settings, "places" to places).apply {
            if (availablePlaces.isNotEmpty()) put("availablePlaces", availablePlaces)
            if (legacy) put("legacy", true)
        }

    companion object {
        fun fromJson(json: JsonMap): AccountPackage? {
            val id = json.str("id")
            if (!PackagesText.isPackageId(id)) return null
            return AccountPackage(
                id,
                json.bool("installed"),
                json.obj("settings").filterValues { it is Boolean || it is String },
                json.bool("legacy"),
                PackagePlaces.parse(json["places"]),
                PackagePlaces.parse(json["availablePlaces"], emptyList()),
            )
        }
    }
}

data class AccountPackagesState(val packages: List<AccountPackage>, val changed: List<String> = emptyList()) {
    fun find(id: String): AccountPackage? = packages.firstOrNull { it.id == id }

    fun isInstalled(id: String): Boolean? = find(id)?.takeUnless { it.legacy }?.installed

    fun placedHere(id: String): Boolean = find(id)?.placedHere ?: true

    fun settings(id: String): Map<String, Any?> = find(id)?.settings ?: emptyMap()

    fun toJson(): Map<String, Any?> = linkedMapOf("packages" to packages.map(AccountPackage::toJson))

    companion object {
        fun fromJson(json: JsonMap): AccountPackagesState = AccountPackagesState(
            json.maps("packages").mapNotNull(AccountPackage::fromJson).distinctBy { it.id },
            json.strings("changed").filter(PackagesText::isPackageId),
        )
    }
}

data class AccountCatalogEntry(
    val id: String,
    val name: String,
    val isDefault: Boolean = true,
    val dependencies: List<String> = emptyList(),
    val settings: List<PackageSetting> = emptyList(),
    val monitors: PackageMonitors = PackageMonitors(),
    val installedBefore: String = "",
) {
    fun toJson(): Map<String, Any?> = linkedMapOf(
        "id" to id,
        "name" to name,
        "default" to isDefault,
        "installedBefore" to installedBefore,
        "dependencies" to dependencies,
        "settings" to settings.map(PackageSetting::toJson),
        "monitors" to monitors.toJson(),
    )

    companion object {
        fun of(pkg: MarketPackage) =
            AccountCatalogEntry(pkg.id, pkg.name, pkg.isDefault, pkg.dependencies, pkg.settings, pkg.monitors, pkg.installedBefore)

        fun fromJson(json: JsonMap): AccountCatalogEntry? {
            val id = json.str("id")
            if (!PackagesText.isPackageId(id)) return null
            return AccountCatalogEntry(
                id,
                json.str("name").ifEmpty { id },
                json.bool("default"),
                PackageDependencies.parse(json["dependencies"]),
                json.maps("settings").mapNotNull(PackageSetting::fromJson),
                PackageMonitors.fromJson(json.obj("monitors")),
                PackagesText.installedBefore(json["installedBefore"]),
            )
        }
    }
}

object AccountPackageGraph {
    fun dependents(id: String, catalog: Collection<AccountCatalogEntry>): List<AccountCatalogEntry> {
        val found = LinkedHashMap<String, AccountCatalogEntry>()
        val queue = ArrayDeque(listOf(id))
        while (queue.isNotEmpty()) {
            val current = queue.removeFirst()
            for (entry in catalog) {
                if (entry.id == id || entry.id in found || current !in entry.dependencies) continue
                found[entry.id] = entry
                queue.addLast(entry.id)
            }
        }
        return found.values.toList()
    }

    fun dependencies(id: String, catalog: Collection<AccountCatalogEntry>): List<AccountCatalogEntry> {
        val byId = catalog.associateBy { it.id }
        val found = LinkedHashMap<String, AccountCatalogEntry>()
        val queue = ArrayDeque(byId[id]?.dependencies.orEmpty())
        while (queue.isNotEmpty()) {
            val current = queue.removeFirst()
            if (current == id || current in found) continue
            val entry = byId[current] ?: AccountCatalogEntry(current, current)
            found[current] = entry
            queue.addAll(entry.dependencies)
        }
        return found.values.toList()
    }

    fun names(entries: List<AccountCatalogEntry>, limit: Int = 3): String {
        val names = entries.map { it.name }
        if (names.size <= limit) return joined(names)
        return names.take(limit).joinToString(", ") + " and ${names.size - limit} more"
    }

    private fun joined(names: List<String>): String = when (names.size) {
        0 -> ""
        1 -> names[0]
        else -> names.dropLast(1).joinToString(", ") + " and " + names.last()
    }

    const val PROJECT_FOOTER = "Your other projects install it on their own. Its settings and where it shows stay with your account."
    const val OTHER_PROJECTS_KEEP = "Your other projects keep it."

    fun removeNote(entry: AccountCatalogEntry, catalog: Collection<AccountCatalogEntry>, project: Boolean = false, installed: (String) -> Boolean): String {
        val going = dependents(entry.id, catalog).filter { installed(it.id) }
        if (project) {
            if (going.isEmpty()) return OTHER_PROJECTS_KEEP
            return "Removing ${entry.name} also removes the ${dependentNoun(going)} from this project: ${names(going)}. $OTHER_PROJECTS_KEEP"
        }
        if (going.isEmpty()) return "Removing ${entry.name} takes it off all your devices."
        return "Removing ${entry.name} also removes the ${dependentNoun(going)} from all your devices: ${names(going)}."
    }

    private fun dependentNoun(going: List<AccountCatalogEntry>): String =
        if (going.size == 1) "package that needs it" else "${going.size} packages that need it"

    fun installNote(entry: AccountCatalogEntry, catalog: Collection<AccountCatalogEntry>, project: Boolean = false, installed: (String) -> Boolean): String {
        val coming = dependencies(entry.id, catalog).filter { !installed(it.id) }
        val where = if (project) "this project" else "all your devices"
        if (coming.isEmpty()) return "Installing ${entry.name} adds it to $where."
        return "Installing ${entry.name} also brings back ${names(coming)} ${if (project) "in" else "on"} $where."
    }

    fun changedNote(installing: Boolean, entry: AccountCatalogEntry, changed: List<String>, catalog: Collection<AccountCatalogEntry>, project: Boolean = false): String {
        val byId = catalog.associateBy { it.id }
        val others = changed.filter { it != entry.id }.map { byId[it] ?: AccountCatalogEntry(it, it) }
        val verb = if (installing) "Installed" else "Removed"
        val where = when {
            !project -> "on all your devices"
            installing -> "in this project"
            else -> "from this project"
        }
        if (others.isEmpty()) return "$verb ${entry.name} $where."
        return "$verb ${entry.name} and ${names(others)} $where."
    }
}

class AccountPackagesStore(
    private val client: PackagesClient?,
    private val prefs: PrefStore?,
    private val legacyProject: () -> Boolean = { true },
) {
    private val listeners = CopyOnWriteArrayList<() -> Unit>()

    @Volatile
    var state: AccountPackagesState? = loadState()
        private set

    @Volatile
    var catalog: Map<String, AccountCatalogEntry> = loadCatalog()
        private set

    @Volatile
    var projectChoices: Map<String, Boolean> = loadChoices()
        private set

    private var seenVersions: Map<String, String> = loadVersions("seen")

    private var latestVersions: Map<String, String> = loadVersions("latest")

    @Volatile
    var updated: Set<String> = changed(seenVersions, latestVersions)
        private set

    fun isUpdated(id: String): Boolean = id in updated

    fun isUpdated(hubItemId: String, kind: Route.Kind?): Boolean = packageFor(hubItemId, kind)?.let { it in updated } ?: false

    fun canMarkUpdated(hubItemId: String, kind: Route.Kind?): Boolean =
        packageFor(hubItemId, kind)?.let { latestVersions[it] != null && it !in updated } ?: false

    val canShowUpdates: Boolean get() = updated.size < latestVersions.size

    fun markViewed(id: String) {
        val version = latestVersions[id] ?: return
        if (seenVersions[id] == version) return
        seenVersions = seenVersions + (id to version)
        saveVersions()
    }

    fun markViewed(hubItemId: String, kind: Route.Kind?) {
        packageFor(hubItemId, kind)?.let(::markViewed)
    }

    fun markAllViewed() {
        if (updated.isEmpty()) return
        seenVersions = seenVersions + latestVersions
        saveVersions()
    }

    fun markAllUnviewed() {
        if (!canShowUpdates) return
        seenVersions = emptyMap()
        saveVersions()
    }

    private fun packageFor(hubItemId: String, kind: Route.Kind?): String? =
        PackageGate.HUB_ITEMS[hubItemId] ?: kind?.let(PackageGate.ROUTE_KINDS::get) ?: hubItemId.takeIf { it in latestVersions }

    private fun adoptVersions(packages: List<MarketPackage>) {
        val next = latestVersions + packages.filter { it.version.isNotEmpty() && PackagesText.isPackageId(it.id) }.associate { it.id to it.version }
        if (next == latestVersions) return
        latestVersions = next
        saveVersions()
    }

    private fun saveVersions() {
        prefs?.putString(VERSIONS_KEY, Json.stringify(linkedMapOf("seen" to seenVersions.toSortedMap(), "latest" to latestVersions.toSortedMap())))
        val next = changed(seenVersions, latestVersions)
        if (next == updated) return
        updated = next
        notifyListeners()
    }

    private fun loadVersions(field: String): Map<String, String> {
        val text = prefs?.getString(VERSIONS_KEY) ?: return emptyMap()
        return versionMap(Json.parseObject(text).obj(field))
    }

    val perProject: Boolean get() = !legacyProject()

    val starterPack: StarterPack get() = PackageGate.starterPack(prefs)

    fun addListener(listener: () -> Unit) {
        listeners.add(listener)
    }

    fun removeListener(listener: () -> Unit) {
        listeners.remove(listener)
    }

    fun isInstalled(id: String): Boolean = isInstalled(id, HashSet())

    private fun isInstalled(id: String, seen: MutableSet<String>): Boolean {
        if (!seen.add(id)) return true
        val entry = catalog[id]
        val row = state?.find(id)
        val own = when {
            !legacyProject() -> projectOwn(id, entry, row)
            row == null -> PackageGate.fallback(id, entry, true, starterPack)
            row.legacy -> true
            else -> row.installed
        }
        return own && entry?.dependencies.orEmpty().all { isInstalled(it, seen) }
    }

    private fun projectOwn(id: String, entry: AccountCatalogEntry?, row: AccountPackage?): Boolean {
        val accountKeeps = row?.installed != false
        return when (projectChoices[id]) {
            false -> false
            true -> accountKeeps
            null -> id in starterPack.packages || (entry?.isDefault == true && accountKeeps)
        }
    }

    fun shows(id: String): Boolean = isInstalled(id) && state?.placedHere(id) != false

    fun installedOrNull(id: String): Boolean? = if (state?.find(id) != null || id in catalog) isInstalled(id) else null

    fun settings(id: String): Map<String, Any?> {
        val stored = state?.settings(id).orEmpty()
        val declared = catalog[id]?.settings.orEmpty()
        if (declared.isEmpty()) return stored
        return LinkedHashMap(stored).also { out -> declared.forEach { out[it.key] = it.valueIn(stored) } }
    }

    fun refresh(): AccountPackagesState = adopt(AccountPackagesState.fromJson(requireClient().call("GET", PATH)))

    fun install(id: String): AccountPackagesState {
        val path = "$PATH/${checked(id)}/install"
        if (!perProject) return adopt(AccountPackagesState.fromJson(requireClient().call("POST", path, body = emptyMap())))
        val missing = AccountPackageGraph.dependencies(id, catalog.values).map { it.id }.filterNot { isInstalled(it) }
        val answer = adopt(AccountPackagesState.fromJson(requireClient().call("POST", path, body = emptyMap())))
        val chosen = listOf(id) + missing
        choose(chosen, true)
        return answer.copy(changed = chosen)
    }

    fun uninstall(id: String): AccountPackagesState {
        if (!perProject) return adopt(AccountPackagesState.fromJson(requireClient().call("POST", "$PATH/${checked(id)}/uninstall", body = emptyMap())))
        val going = listOf(checked(id)) + AccountPackageGraph.dependents(id, catalog.values).map { it.id }.filter { isInstalled(it) }
        choose(going, false)
        return (state ?: AccountPackagesState(emptyList())).copy(changed = going)
    }

    private fun choose(ids: List<String>, installed: Boolean) {
        val next = LinkedHashMap(projectChoices).apply { ids.forEach { put(it, installed) } }
        if (next == projectChoices) return
        projectChoices = next
        prefs?.putString(PackageGate.PROJECT_INSTALLS_KEY, Json.stringify(next))
        notifyListeners()
    }

    fun setPlaces(id: String, places: Collection<String>): AccountPackagesState {
        val wanted = PackagePlaces.ALL.filter { it in places }
        if (wanted.isEmpty()) throw PackagesException("invalid_places", 400)
        val answer = try {
            requireClient().call("POST", "$PATH/${checked(id)}/places", body = mapOf("places" to wanted))
        } catch (error: PackagesException) {
            if (error.code == "plugin_not_installed") throw PackagesException("install_first", error.status)
            throw error
        }
        return adopt(AccountPackagesState.fromJson(answer))
    }

    fun saveSettings(id: String, values: Map<String, Any?>): AccountPackagesState =
        adopt(AccountPackagesState.fromJson(requireClient().call("POST", "$PATH/${checked(id)}/settings", body = mapOf("settings" to values))))

    fun refreshCatalog(): Map<String, AccountCatalogEntry> {
        rememberCatalog(requireClient().packages())
        return catalog
    }

    fun rememberCatalog(packages: List<MarketPackage>) {
        adoptVersions(packages)
        val entries = packages.filter { it.accountSync }.map(AccountCatalogEntry::of)
        if (entries.isEmpty()) return
        val next = entries.associateBy { it.id }
        if (next == catalog) return
        catalog = next
        prefs?.putString(CATALOG_KEY, Json.stringify(mapOf("packages" to entries.map(AccountCatalogEntry::toJson))))
        notifyListeners()
    }

    fun adopt(next: AccountPackagesState): AccountPackagesState {
        if (next.packages.isEmpty() && state != null) return next
        val changed = next.copy(changed = emptyList()) != state
        state = next.copy(changed = emptyList())
        prefs?.putString(STATE_KEY, Json.stringify(next.toJson()))
        if (changed) notifyListeners()
        return next
    }

    private fun requireClient(): PackagesClient = client ?: throw PackagesException("offline")

    private fun checked(id: String): String {
        if (!PackagesText.isPackageId(id)) throw PackagesException("plugin_not_found", 404)
        return id
    }

    private fun notifyListeners() {
        for (listener in listeners) listener()
    }

    private fun loadState(): AccountPackagesState? {
        val text = prefs?.getString(STATE_KEY) ?: return null
        val json = Json.parseObject(text)
        if (json.isEmpty()) return null
        return AccountPackagesState.fromJson(json)
    }

    private fun loadCatalog(): Map<String, AccountCatalogEntry> {
        val text = prefs?.getString(CATALOG_KEY) ?: return emptyMap()
        return Json.parseObject(text).maps("packages").mapNotNull(AccountCatalogEntry::fromJson).associateBy { it.id }
    }

    private fun loadChoices(): Map<String, Boolean> {
        val text = prefs?.getString(PackageGate.PROJECT_INSTALLS_KEY) ?: return emptyMap()
        return Json.parseObject(text).entries.mapNotNull { (id, on) -> if (on is Boolean && PackagesText.isPackageId(id)) id to on else null }.toMap()
    }

    companion object {
        const val PATH = "/api/plugins/@account"
        const val STATE_KEY = "packages/account/state"
        const val CATALOG_KEY = "packages/account/catalog"
        const val VERSIONS_KEY = "packages/versions"

        fun changed(seen: Map<String, String>, latest: Map<String, String>): Set<String> =
            latest.filter { (id, version) -> seen[id] != version }.keys

        fun versionMap(json: Map<String, Any?>): Map<String, String> =
            json.mapNotNull { (id, version) -> if (PackagesText.isPackageId(id) && version is String && version.isNotEmpty()) id to version else null }.toMap()
    }
}
