package com.forkmesh.nativeapp.code

import com.forkmesh.nativeapp.control.ControlCalling
import com.forkmesh.nativeapp.control.ControlFamilyChannel
import com.forkmesh.nativeapp.control.DesktopControlFamilyName
import com.forkmesh.nativeapp.control.LocalAgentRepository
import com.forkmesh.nativeapp.control.LocalControlException
import com.forkmesh.nativeapp.json.bool
import com.forkmesh.nativeapp.json.int
import com.forkmesh.nativeapp.json.maps
import com.forkmesh.nativeapp.json.obj
import com.forkmesh.nativeapp.json.str
import com.forkmesh.nativeapp.model.Repository
import com.forkmesh.nativeapp.navigation.RepoRef

data class DesktopRepo(
    val owner: String,
    val name: String,
    val description: String = "",
    val branch: String = "",
    val checkout: Boolean = false,
    val isPrivate: Boolean = false,
    val published: Boolean = false,
    val runningAgents: Int = 0,
    val totalAgents: Int = 0,
) {
    val slug: String get() = "$owner/$name"

    val id: String get() = slug.lowercase()

    val repository: Repository
        get() = Repository(owner = owner, name = name, description = description, mirrors = 0, isPrivate = isPrivate, liveHost = true)

    val detail: String
        get() {
            val parts = ArrayList<String>()
            if (branch.isNotEmpty()) parts += branch else if (!checkout) parts += "No checkout"
            if (runningAgents > 0) parts += "$runningAgents running"
            parts += if (isPrivate) "Private" else "Public"
            if (!published) parts += "Not published"
            return parts.joinToString(" · ")
        }

    fun matches(query: String): Boolean =
        query.isEmpty() || id.contains(query) || description.lowercase().contains(query)

    companion object {
        fun fromJson(json: Map<String, Any?>): DesktopRepo? {
            var owner = json.str("owner").trim()
            var name = json.str("name").trim()
            if (owner.isEmpty() || name.isEmpty()) {
                RepoRef.parse(json.str("repo").trim())?.let {
                    owner = it.owner
                    name = it.name
                }
            }
            if (owner.isEmpty() || name.isEmpty()) return null
            val agents = json.obj("agents")
            return DesktopRepo(
                owner = owner,
                name = name,
                description = json.str("description"),
                branch = json.str("branch"),
                checkout = json.bool("checkout"),
                isPrivate = json.bool("private"),
                published = json.bool("published"),
                runningAgents = agents.int("running"),
                totalAgents = agents.int("total"),
            )
        }
    }
}

class DesktopReposModel(
    channel: ControlCalling?,
    desktopName: String = "",
    private val fallback: () -> List<LocalAgentRepository> = { emptyList() },
) {
    private val family = channel?.let { ControlFamilyChannel(DesktopControlFamilyName.Repos, it) }
    private var tap: Any? = null

    @Volatile
    var repos: List<DesktopRepo> = emptyList()
        private set

    @Volatile
    var desktopName: String = desktopName
        private set

    @Volatile
    var loading: Boolean = false
        private set

    @Volatile
    var problem: String? = null
        private set

    @Volatile
    var loaded: Boolean = false
        private set

    val available: Boolean get() = family != null

    val visible: Boolean get() = available && (repos.isNotEmpty() || problem != null || loading)

    val title: String get() = if (desktopName.isEmpty()) "Desktop" else "Desktop · $desktopName"

    fun rows(query: String): List<DesktopRepo> = repos.filter { it.matches(query) }

    @Synchronized
    fun start(onChanged: () -> Unit) {
        val family = family ?: return
        if (tap != null) return
        tap = family.addEventTap { suffix, _ -> if (suffix == "changed") onChanged() }
    }

    @Synchronized
    fun stop() {
        tap?.let { family?.removeEventTap(it) }
        tap = null
    }

    fun load(): Boolean {
        val family = family ?: return false
        loading = true
        try {
            val reply = family.call("list")
            repos = sorted(reply.maps("repos").mapNotNull(DesktopRepo::fromJson))
            val name = reply.str("desktop").trim()
            if (name.isNotEmpty()) desktopName = name
            problem = null
            return true
        } catch (error: Exception) {
            val older = runCatching(fallback).getOrDefault(emptyList())
                .map { DesktopRepo(owner = it.owner, name = it.name, checkout = true, published = true) }
            if (older.isNotEmpty()) {
                repos = sorted(older)
                problem = null
            } else {
                problem = explain(error)
            }
            return false
        } finally {
            loaded = true
            loading = false
        }
    }

    companion object {
        fun sorted(repos: List<DesktopRepo>): List<DesktopRepo> {
            val seen = HashSet<String>()
            return repos.filter { seen.add(it.id) }.sortedWith(
                compareBy<DesktopRepo> { if (it.runningAgents > 0) 0 else 1 }.thenBy { it.id },
            )
        }

        fun explain(error: Throwable): String = when ((error as? LocalControlException)?.code.orEmpty()) {
            "not_paired" -> "Pair this phone with your desktop to see its repositories."
            "not_owner" -> "The desktop is signed in to another account."
            "unsupported", "unknown_method" -> "Update the desktop to list its repositories here."
            else -> "The desktop is offline. Its repositories show here when it is back."
        }

        fun total(catalog: List<Repository>, desktop: List<DesktopRepo>): Int {
            val keys = catalog.mapTo(HashSet()) { "${it.owner}/${it.name}".lowercase() }
            return catalog.size + desktop.count { it.id !in keys }
        }
    }
}
