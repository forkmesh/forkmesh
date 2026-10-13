package com.forkmesh.nativeapp.ci

import com.forkmesh.nativeapp.api.ApiJsonResult
import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.json.bool
import com.forkmesh.nativeapp.json.maps
import com.forkmesh.nativeapp.json.obj
import com.forkmesh.nativeapp.json.str
import com.forkmesh.nativeapp.json.strings
import com.forkmesh.nativeapp.model.modelInt

class ProviderMirrorError(val status: Int, val code: String, val detail: String = "") : Exception() {
    override val message: String
        get() = when {
            detail.isNotEmpty() -> detail
            code == "invalid_provider" -> "Choose GitHub or GitLab."
            code == "invalid_remote" -> "Enter the repository as owner/name, or paste its https address."
            code == "token_required" -> "Paste an access token so ForkMesh can reach the repository."
            code == "not_configured" -> "This repository is not mirrored yet."
            code == "nothing_selected" -> "Tick at least one item to import."
            code == "not_authorized" -> "Only the repository owner can manage its mirror."
            else -> CiError(status, code).message
        }

    override fun toString(): String = message
}

interface ProviderMirrorClient {
    fun providerMirror(repo: RepoRef, action: String, fields: Map<String, Any?>): Map<String, Any?>
}

object ProviderMirrorRequests {
    fun decode(result: ApiJsonResult): Map<String, Any?> {
        val map = result.body
        if (result.ok && map["error"] == null) return map
        throw ProviderMirrorError(result.statusCode, map.str("error"), map.str("message"))
    }
}

class ApiProviderMirrorClient(private val api: ApiService) : ProviderMirrorClient {
    override fun providerMirror(repo: RepoRef, action: String, fields: Map<String, Any?>): Map<String, Any?> =
        ProviderMirrorRequests.decode(
            api.jsonExchange(
                "POST", CiRequests.repoPath(repo, "provider-mirror"), emptyList(),
                LinkedHashMap(fields).apply { put("action", action) },
            ),
        )
}

data class ProviderMirrorProvider(
    val key: String,
    val label: String,
    val web: String,
    val tokenUrl: String,
    val tokenHelp: String,
) {
    val host: String get() = web.removePrefix("https://")

    companion object {
        val DEFAULTS = listOf(
            ProviderMirrorProvider("github", "GitHub", "https://github.com", "https://github.com/settings/personal-access-tokens/new", ""),
            ProviderMirrorProvider("gitlab", "GitLab", "https://gitlab.com", "https://gitlab.com/-/user_settings/personal_access_tokens", ""),
        )
    }
}

data class ProviderMirrorResource(
    val key: String,
    val label: String,
    val labels: Map<String, String>,
    val icon: String,
    val hint: String,
    val directions: List<String>,
    val providers: List<String>,
    val defaultDirection: String,
) {
    fun title(provider: String): String = labels[provider] ?: label
    fun supported(provider: String): Boolean = provider in providers
    val canImport: Boolean get() = "pull" in directions

    companion object {
        fun fromJson(json: Map<String, Any?>): ProviderMirrorResource? {
            val key = json.str("key")
            if (key.isEmpty()) return null
            val names = json.obj("labels")
            return ProviderMirrorResource(
                key = key,
                label = json.str("label").ifEmpty { key },
                labels = names.keys.associateWith { names.str(it) },
                icon = json.str("icon"),
                hint = json.str("hint"),
                directions = json.strings("directions"),
                providers = json.strings("providers"),
                defaultDirection = json.str("default").ifEmpty { "off" },
            )
        }
    }
}

data class ProviderMirrorSetting(val direction: String = "off", val mode: String = "auto", val comments: Boolean = true) {
    val imports: Boolean get() = direction == "pull" || direction == "both"
    val manual: Boolean get() = imports && mode == "manual"
    val json: Map<String, Any?> get() = linkedMapOf("direction" to direction, "mode" to mode, "comments" to comments)

    companion object {
        fun fromJson(json: Map<String, Any?>) = ProviderMirrorSetting(
            direction = json.str("direction").ifEmpty { "off" },
            mode = json.str("mode").ifEmpty { "auto" },
            comments = json["comments"] != false,
        )
    }
}

data class ProviderMirrorActivity(val atMs: Long, val kind: String, val ok: Boolean, val message: String)

data class ProviderMirrorState(
    val provider: String,
    val providerLabel: String,
    val remote: String,
    val remoteUrl: String,
    val tokenHint: String,
    val active: Boolean,
    val prune: Boolean,
    val resources: Map<String, ProviderMirrorSetting>,
    val login: String,
    val remotePrivate: Boolean,
    val canPush: Boolean,
    val connectionError: String,
    val codePushAtMs: Long,
    val codePushOk: Boolean,
    val codePushRefs: Int,
    val codePushError: String,
    val syncAtMs: Long,
    val syncOk: Boolean,
    val syncImported: Int,
    val syncComments: Int,
    val syncError: String,
    val history: List<ProviderMirrorActivity>,
) {
    fun setting(key: String): ProviderMirrorSetting = resources[key] ?: ProviderMirrorSetting()
    val importing: Boolean get() = resources.values.any { it.imports }

    companion object {
        fun fromJson(json: Map<String, Any?>): ProviderMirrorState? {
            val provider = json.str("provider")
            if (provider.isEmpty()) return null
            val map = json.obj("resources")
            val status = json.obj("status")
            val push = status.obj("codePush")
            val sync = status.obj("lastSync")
            return ProviderMirrorState(
                provider = provider,
                providerLabel = json.str("providerLabel").ifEmpty { if (provider == "gitlab") "GitLab" else "GitHub" },
                remote = json.str("remote"),
                remoteUrl = json.str("remoteUrl"),
                tokenHint = json.str("tokenHint"),
                active = json["active"] != false,
                prune = json.bool("prune"),
                resources = map.keys.associateWith { ProviderMirrorSetting.fromJson(map.obj(it)) },
                login = status.str("login"),
                remotePrivate = status.bool("remotePrivate"),
                canPush = status.bool("canPush"),
                connectionError = status.str("error"),
                codePushAtMs = modelInt(push["at"]),
                codePushOk = push.bool("ok"),
                codePushRefs = modelInt(push["refs"]).toInt(),
                codePushError = push.str("error"),
                syncAtMs = modelInt(sync["at"]),
                syncOk = sync.bool("ok"),
                syncImported = modelInt(sync["imported"]).toInt(),
                syncComments = modelInt(sync["comments"]).toInt(),
                syncError = sync.str("error"),
                history = json.maps("history").take(20).map {
                    ProviderMirrorActivity(modelInt(it["at"]), it.str("kind"), it.bool("ok"), it.str("message"))
                },
            )
        }
    }
}

data class ProviderMirrorCandidate(
    val key: String,
    val number: Int,
    val title: String,
    val author: String,
    val state: String,
    val url: String,
    val updatedAtMs: Long,
    val commentCount: Int,
    val excerpt: String,
    val imported: Boolean,
    val mirrored: Boolean,
    val merged: Boolean,
) {
    val selectable: Boolean get() = !imported && !mirrored
    val stateLabel: String get() = if (merged) "merged" else state

    companion object {
        fun fromJson(json: Map<String, Any?>): ProviderMirrorCandidate? {
            val key = json.str("key")
            if (key.isEmpty()) return null
            return ProviderMirrorCandidate(
                key = key,
                number = modelInt(json["number"]).toInt(),
                title = json.str("title"),
                author = json.str("author"),
                state = json.str("state"),
                url = json.str("url"),
                updatedAtMs = modelInt(json["updatedAt"]),
                commentCount = modelInt(json["commentCount"]).toInt(),
                excerpt = json.str("excerpt"),
                imported = json.bool("imported"),
                mirrored = json.bool("mirrored"),
                merged = json.bool("merged"),
            )
        }
    }
}

data class ProviderMirrorDraft(
    val provider: String = "github",
    val remote: String = "",
    val token: String = "",
    val createRemote: Boolean = true,
    val prune: Boolean = false,
    val resources: Map<String, ProviderMirrorSetting> = emptyMap(),
) {
    fun setting(resource: ProviderMirrorResource): ProviderMirrorSetting =
        resources[resource.key] ?: ProviderMirrorSetting(direction = resource.defaultDirection)

    fun with(resource: ProviderMirrorResource, setting: ProviderMirrorSetting): ProviderMirrorDraft =
        copy(resources = resources + (resource.key to setting))

    fun validationError(hasStoredToken: Boolean): String? {
        if (cleanRemote(remote) == null) return "Enter the repository as owner/name."
        val token = token.trim()
        if (token.isEmpty() && !hasStoredToken) return "Paste an access token."
        if (token.length > 512 || token.any { it.isWhitespace() }) return "That token does not look right."
        return null
    }

    fun fields(catalog: List<ProviderMirrorResource>, description: String): Map<String, Any?> = linkedMapOf(
        "provider" to provider,
        "remote" to (cleanRemote(remote) ?: remote),
        "token" to token.trim(),
        "createRemote" to createRemote,
        "prune" to prune,
        "description" to description.take(300),
        "resources" to catalog.associate { it.key to setting(it).json },
    )

    companion object {
        private val SEGMENT = Regex("^[A-Za-z0-9_.-]{1,100}$")

        fun from(state: ProviderMirrorState) = ProviderMirrorDraft(
            provider = state.provider, remote = state.remote, prune = state.prune, resources = state.resources,
        )

        fun cleanRemote(text: String): String? {
            var value = text.trim()
            for (prefix in listOf("https://github.com/", "https://gitlab.com/", "github.com/", "gitlab.com/")) {
                if (value.lowercase().startsWith(prefix)) value = value.substring(prefix.length)
            }
            value = value.removeSuffix(".git")
            val parts = value.split('/').filter { it.isNotEmpty() }
            if (parts.size != 2) return null
            if (parts.any { !SEGMENT.matches(it) || it.startsWith(".") }) return null
            return parts.joinToString("/")
        }
    }
}

object ProviderMirrorText {
    fun direction(direction: String, provider: String): String = when (direction) {
        "push" -> "ForkMesh → $provider"
        "pull" -> "$provider → ForkMesh"
        "both" -> "Both ways"
        else -> "Off"
    }

    fun codeLine(state: ProviderMirrorState, hosted: Boolean, now: Long): Pair<Boolean?, String> = when {
        state.setting("code").direction != "push" -> null to "Code is not mirrored."
        !hosted -> null to "Waiting for this repository to be hosted on ForkMesh."
        state.codePushAtMs == 0L -> null to "First push is on its way."
        !state.codePushOk -> false to "Last push failed: ${state.codePushError.ifEmpty { "unknown error" }}"
        else -> true to "Pushed ${state.codePushRefs} branch${if (state.codePushRefs == 1) "" else "es"} and tags ${ago(state.codePushAtMs, now)}."
    }

    fun syncLine(state: ProviderMirrorState, now: Long): Pair<Boolean?, String> = when {
        !state.importing -> null to "Nothing is imported into ForkMesh."
        state.syncAtMs == 0L -> null to "First import runs within ten minutes."
        !state.syncOk -> false to "Last import hit a problem: ${state.syncError}"
        else -> true to "Checked ${ago(state.syncAtMs, now)}: ${state.syncImported} new, ${state.syncComments} comment${if (state.syncComments == 1) "" else "s"}."
    }

    fun ago(atMs: Long, now: Long): String {
        val minutes = maxOf(0L, now - atMs) / 60000
        if (minutes < 1) return "just now"
        if (minutes < 60) return "$minutes min ago"
        val hours = minutes / 60
        if (hours < 48) return "$hours h ago"
        return "${hours / 24} days ago"
    }
}

class ProviderMirrorModel(private val client: ProviderMirrorClient, val repo: RepoRef) {
    @Volatile var mirror: ProviderMirrorState? = null
        private set
    @Volatile var providers: List<ProviderMirrorProvider> = ProviderMirrorProvider.DEFAULTS
        private set
    @Volatile var resources: List<ProviderMirrorResource> = emptyList()
        private set
    @Volatile var codeHosted: Boolean = false
        private set
    @Volatile var isPrivate: Boolean = false
        private set
    @Volatile var pending: Int = 0
        private set
    @Volatile var loaded: Boolean = false
        private set
    @Volatile var loading: Boolean = false
        private set
    @Volatile var busy: Boolean = false
        private set
    @Volatile var error: String? = null
        private set
    @Volatile var notice: String? = null
        private set
    @Volatile var candidateKind: String? = null
        private set
    @Volatile var candidates: List<ProviderMirrorCandidate> = emptyList()
        private set
    @Volatile var candidatePage: Int = 1
        private set
    @Volatile var candidatesMore: Boolean = false
        private set
    @Volatile var candidatesLoading: Boolean = false
        private set
    @Volatile var selected: Set<String> = emptySet()
        private set

    fun provider(key: String): ProviderMirrorProvider? = providers.firstOrNull { it.key == key }

    val importKinds: List<ProviderMirrorResource>
        get() {
            val current = mirror ?: return emptyList()
            return resources.filter { it.supported(current.provider) && current.setting(it.key).imports }
        }

    fun apply(data: Map<String, Any?>) {
        val catalog = data.obj("catalog")
        val parsedProviders = catalog.maps("providers").mapNotNull { map ->
            val key = map.str("key")
            if (key.isEmpty()) null else ProviderMirrorProvider(
                key, map.str("label").ifEmpty { key }, map.str("web"), map.str("tokenUrl"), map.str("tokenHelp"),
            )
        }
        if (parsedProviders.isNotEmpty()) providers = parsedProviders
        val parsedResources = catalog.maps("resources").mapNotNull(ProviderMirrorResource::fromJson)
        if (parsedResources.isNotEmpty()) resources = parsedResources
        mirror = ProviderMirrorState.fromJson(data.obj("mirror"))
        codeHosted = data.bool("codeHosted")
        isPrivate = data.bool("private")
        pending = modelInt(data["pending"]).toInt()
        loaded = true
    }

    fun load() {
        loading = true
        try {
            apply(client.providerMirror(repo, "get", emptyMap()))
            error = null
        } catch (failure: Exception) {
            error = failure.message ?: "$failure"
        } finally {
            loading = false
        }
    }

    private fun mutate(action: String, fields: Map<String, Any?>): Map<String, Any?>? {
        if (busy) return null
        busy = true
        return try {
            val data = client.providerMirror(repo, action, fields)
            if (data["catalog"] != null) apply(data)
            error = null
            data
        } catch (failure: Exception) {
            error = failure.message ?: "$failure"
            notice = null
            null
        } finally {
            busy = false
        }
    }

    fun save(draft: ProviderMirrorDraft, description: String = ""): Boolean {
        val current = mirror
        val storedToken = current != null && current.provider == draft.provider && current.tokenHint.isNotEmpty()
        draft.validationError(storedToken)?.let {
            error = it
            return false
        }
        val data = mutate("save", draft.fields(resources, description)) ?: return false
        notice = if (data.bool("created")) "Created ${mirror?.remote ?: "the repository"} and connected." else "Mirror saved."
        return true
    }

    fun sync() {
        val data = mutate("sync", emptyMap()) ?: return
        val sync = data.obj("sync")
        notice = "Synced: ${modelInt(sync["imported"])} new, ${modelInt(sync["comments"])} comments."
    }

    fun verify() {
        mutate("verify", emptyMap()) ?: return
        notice = mirror?.connectionError?.ifEmpty { null } ?: "Connection works."
    }

    fun setActive(active: Boolean) {
        mutate("toggle", mapOf("active" to active)) ?: return
        notice = if (active) "Mirroring resumed." else "Mirroring paused."
    }

    fun disconnect() {
        mutate("disconnect", emptyMap()) ?: return
        closeCandidates()
        notice = "Disconnected."
    }

    fun openCandidates(kind: String, page: Int = 1) {
        candidateKind = kind
        candidatePage = maxOf(1, page)
        candidates = emptyList()
        selected = emptySet()
        candidatesLoading = true
        try {
            val data = client.providerMirror(repo, "candidates", mapOf("kind" to kind, "page" to candidatePage))
            if (candidateKind != kind) return
            candidates = data.maps("items").mapNotNull(ProviderMirrorCandidate::fromJson)
            candidatesMore = data.bool("more")
            error = null
        } catch (failure: Exception) {
            error = failure.message ?: "$failure"
        } finally {
            candidatesLoading = false
        }
    }

    fun closeCandidates() {
        candidateKind = null
        candidates = emptyList()
        selected = emptySet()
    }

    fun toggle(candidate: ProviderMirrorCandidate) {
        if (!candidate.selectable) return
        selected = if (candidate.key in selected) selected - candidate.key else selected + candidate.key
    }

    fun importSelected(): Int {
        val kind = candidateKind
        if (kind == null || selected.isEmpty()) {
            error = ProviderMirrorError(400, "nothing_selected").message
            return 0
        }
        val data = mutate(
            "import",
            linkedMapOf("kind" to kind, "page" to candidatePage, "keys" to selected.sorted()),
        ) ?: return 0
        val count = modelInt(data["imported"]).toInt()
        notice = "Imported $count item${if (count == 1) "" else "s"}. They appear once the repository host picks them up."
        openCandidates(kind, candidatePage)
        return count
    }
}
