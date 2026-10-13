package com.forkmesh.nativeapp.account.projects

import com.forkmesh.nativeapp.api.AuthController
import com.forkmesh.nativeapp.api.PrefStore
import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.push.BackgroundLinkKeys
import com.forkmesh.nativeapp.security.SecretStore

interface FoldPrefs : PrefStore {
    fun keys(): Set<String>

    fun copyRaw(from: String, to: String): Boolean
}

class NextFoldResult(
    val folded: List<String> = emptyList(),
    val movedFrom: String? = null,
    val keptKeysOf: List<String> = emptyList(),
    val replacedSecrets: List<String> = emptyList(),
)

object NextProjectFold {
    const val REPLACED_PREFIX = "fold/replaced/"

    private val prod: String get() = ProjectDefaults.PROD_HOST

    fun run(prefs: FoldPrefs, secrets: SecretStore, secretKeys: () -> Set<String>): NextFoldResult {
        val raw = prefs.getString(ProjectDirectory.LIST_KEY) ?: ""
        val entries = parseList(raw)
        val nextEntries = entries.filter { ProjectDefaults.isNextHost(id(it)) }
        if (nextEntries.isEmpty()) {
            val active = prefs.getString(ProjectDirectory.ACTIVE_KEY) ?: ""
            if (ProjectDefaults.isNextHost(active)) prefs.putString(ProjectDirectory.ACTIVE_KEY, prod)
            return NextFoldResult()
        }
        var prodSignedIn = hasSession(secrets, prod) || hasSession(secrets, "")
        var movedFrom: String? = null
        val kept = ArrayList<String>()
        val replaced = ArrayList<String>()
        for (entry in nextEntries) {
            val nextId = id(entry)
            val namespace = (entry["namespace"] as? String)?.takeIf { it.isNotEmpty() } ?: ProjectScope.namespaceFor(nextId)
            val nextSession = secrets.read(AuthController.sessionKeyFor(nextId))
            if (!nextSession.isNullOrEmpty() && !prodSignedIn) {
                replaced += moveSecrets(secrets, secretKeys(), namespace)
                moveSecret(secrets, AuthController.sessionKeyFor(nextId), AuthController.sessionKeyFor(prod))
                movePrefsUnder(prefs, namespace)
                for ((from, to) in listOf(
                    AuthController.guestModeKeyFor(nextId) to AuthController.guestModeKeyFor(prod),
                    AuthController.revokedBannerKeyFor(nextId) to AuthController.revokedBannerKeyFor(prod),
                    BackgroundLinkKeys.enabled(nextId) to BackgroundLinkKeys.enabled(prod),
                    BackgroundLinkKeys.ledger(nextId) to BackgroundLinkKeys.ledger(prod),
                )) {
                    movePref(prefs, from, to)
                }
                movedFrom = nextId
                prodSignedIn = true
            } else {
                if (nextSession.isNullOrEmpty() && !prodSignedIn) {
                    movePref(prefs, AuthController.guestModeKeyFor(nextId), AuthController.guestModeKeyFor(prod))
                }
                kept.add(nextId)
            }
        }
        val remaining = entries.filterNot { ProjectDefaults.isNextHost(id(it)) }
        prefs.putString(ProjectDirectory.LIST_KEY, Json.stringify(remaining))
        val active = prefs.getString(ProjectDirectory.ACTIVE_KEY) ?: ""
        if (active.isEmpty() || ProjectDefaults.isNextHost(active)) prefs.putString(ProjectDirectory.ACTIVE_KEY, prod)
        return NextFoldResult(nextEntries.map(::id), movedFrom, kept, replaced)
    }

    private fun id(entry: Map<String, Any?>): String = (entry["id"] ?: "").toString()

    private fun parseList(raw: String): List<Map<String, Any?>> {
        if (raw.isEmpty()) return emptyList()
        val decoded = runCatching { Json.parse(raw) }.getOrNull() as? List<*> ?: return emptyList()
        @Suppress("UNCHECKED_CAST")
        return decoded.mapNotNull { it as? Map<String, Any?> }
    }

    private fun hasSession(secrets: SecretStore, projectId: String): Boolean =
        !secrets.read(AuthController.sessionKeyFor(projectId)).isNullOrEmpty()

    private fun moveSecrets(secrets: SecretStore, keys: Set<String>, namespace: String): List<String> {
        val replaced = ArrayList<String>()
        for (key in keys.filter { it.startsWith(namespace) }.sorted()) {
            val target = key.removePrefix(namespace)
            if (target.isEmpty()) continue
            val value = secrets.read(key) ?: continue
            val existing = secrets.read(target)
            if (existing != null && existing != value) {
                writeVerified(secrets, REPLACED_PREFIX + target, existing)
                replaced.add(target)
            }
            writeVerified(secrets, target, value)
            secrets.delete(key)
        }
        return replaced
    }

    private fun moveSecret(secrets: SecretStore, from: String, to: String) {
        val value = secrets.read(from) ?: return
        writeVerified(secrets, to, value)
        secrets.delete(from)
    }

    private fun writeVerified(secrets: SecretStore, key: String, value: String) {
        secrets.write(key, value)
        check(secrets.read(key) == value) { "could not move $key" }
    }

    private fun movePrefsUnder(prefs: FoldPrefs, namespace: String) {
        for (key in prefs.keys().filter { it.startsWith(namespace) }.sorted()) {
            val target = key.removePrefix(namespace)
            if (target.isEmpty() || prefs.contains(target)) {
                continue
            }
            if (prefs.copyRaw(key, target)) prefs.remove(key)
        }
    }

    private fun movePref(prefs: FoldPrefs, from: String, to: String) {
        if (!prefs.contains(from)) return
        if (prefs.contains(to) || prefs.copyRaw(from, to)) prefs.remove(from)
    }
}
