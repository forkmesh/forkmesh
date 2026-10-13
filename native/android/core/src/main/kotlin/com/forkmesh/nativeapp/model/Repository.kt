package com.forkmesh.nativeapp.model

import com.forkmesh.nativeapp.json.str

data class Repository(
    val owner: String,
    val name: String,
    val description: String = "",
    val language: String = "",
    val stars: Int = 0,
    val forks: Int = 0,
    val mirrors: Int = 1,
    val defaultBranch: String = "main",
    val isPrivate: Boolean = false,
    val liveHost: Boolean = false,
    val cloneOnline: Boolean = false,
    val updatedMs: Long = 0,
    val solana: String = "",
    val organization: String = "",
    val publicOwner: String = "",
    val rootCommit: String = "",
    val forkedFrom: String = "",
    val source: String = "",
    val cloneUrl: String = "",
) {
    val displayOwner: String get() = publicOwner.ifEmpty { owner }

    val fullName: String get() = "$displayOwner/$name"

    internal val isFork: Boolean
        get() = forkedFrom.contains('/') && forkedFrom != "${owner.lowercase()}/${name.lowercase()}"

    internal fun merged(publicOwner: String, copies: List<Repository>): Repository {
        fun most(field: (Repository) -> Long): Long = copies.maxOfOrNull(field)?.coerceAtLeast(0) ?: 0
        fun first(field: (Repository) -> String): String = copies.map(field).firstOrNull { it.isNotEmpty() } ?: ""
        val mirrorCount = most { it.mirrors.toLong() }.toInt()
        return copy(
            description = description.ifEmpty { first { it.description } },
            language = language.ifEmpty { first { it.language } },
            stars = most { it.stars.toLong() }.toInt(),
            forks = most { it.forks.toLong() }.toInt(),
            mirrors = if (copies.size > mirrorCount) copies.size else mirrorCount,
            liveHost = copies.any { it.liveHost },
            cloneOnline = copies.any { it.cloneOnline || it.liveHost },
            updatedMs = most { it.updatedMs },
            publicOwner = if (publicOwner.lowercase() == owner.lowercase()) "" else publicOwner,
        )
    }

    val donationAddress: String get() = solana

    companion object {
        fun fromJson(j: Map<String, Any?>): Repository = Repository(
            owner = j.str("owner"),
            name = j.str("name"),
            description = j.str("description"),
            language = j.str("language"),
            stars = modelInt(j["stars"]).toInt(),
            forks = modelInt(j["forks"]).toInt(),
            mirrors = modelInt(j["mirrors"] ?: 1L).toInt(),
            defaultBranch = j.str("defaultBranch", "main"),
            isPrivate = j["isPrivate"] == true,
            liveHost = j["liveHost"] == true,
            cloneOnline = j["cloneOnline"] == true,
            updatedMs = modelInt(j["updatedAt"] ?: j["publishedAt"] ?: 0L),
            solana = firstModelString(j, listOf("solana", "donationAddress", "payoutAddress")),
            organization = catalogOrganization(j),
            rootCommit = (j["rootCommit"] ?: "").toString().trim(),
            forkedFrom = (j["forkedFrom"] ?: "").toString().trim().lowercase(),
            source = (j["source"] ?: "").toString().trim(),
            cloneUrl = (j["cloneUrl"] ?: "").toString().trim(),
        )

        internal fun catalogOrganization(j: Map<String, Any?>): String {
            val values = j["logicalOwners"]
            val owners: List<Any?> = if (values is List<*> && values.isNotEmpty()) {
                values
            } else {
                listOf(mapOf("kind" to j["ownerKind"], "owner" to j["logicalOwner"]))
            }
            for (value in owners) {
                if (value !is Map<*, *>) continue
                val kind = (value["kind"] ?: "").toString().trim().lowercase()
                val owner = (value["owner"] ?: "").toString().trim()
                if (kind == "organization" && owner.isNotEmpty()) return owner
            }
            return ""
        }
    }
}

private val cloneSegment = Regex("^[A-Za-z0-9._:-]+$")

internal fun cloneUrlRoute(cloneUrl: String): Pair<String, String>? {
    var path = cloneUrl.trim()
    if (path.isEmpty()) return null
    path = if (path.startsWith("git@") && path.contains(':')) {
        path.substring(path.indexOf(':') + 1)
    } else {
        try {
            java.net.URI(path).rawPath ?: ""
        } catch (error: Exception) {
            ""
        }
    }
    val parts = path.split('/').filter { it.isNotEmpty() }
    if (parts.size < 2) return null
    fun decode(part: String): String = try {
        java.net.URLDecoder.decode(part.replace("+", "%2B"), "UTF-8")
    } catch (error: Exception) {
        part
    }
    val owner = decode(parts[parts.size - 2])
    var name = decode(parts.last())
    if (name.lowercase().endsWith(".git")) name = name.substring(0, name.length - 4)
    if (!cloneSegment.matches(owner) || !cloneSegment.matches(name)) return null
    return owner to name
}

fun collapseRepositoryMirrors(repos: List<Repository>): List<Repository> {
    fun route(owner: String, name: String) = "${owner.lowercase()}/${name.lowercase()}"
    val orgByRoot = HashMap<String, String>()
    val orgByRoute = HashMap<String, String>()
    for (repo in repos) {
        if (repo.organization.isEmpty()) continue
        orgByRoute.putIfAbsent(route(repo.owner, repo.name), repo.organization)
        if (!repo.isFork && repo.rootCommit.isNotEmpty()) {
            orgByRoot.putIfAbsent("${repo.rootCommit}\n${repo.name.lowercase()}", repo.organization)
        }
    }
    fun publicOwnerOf(repo: Repository): String {
        if (repo.organization.isNotEmpty()) return repo.organization
        if (repo.isFork) return repo.owner
        val byRoot = orgByRoot["${repo.rootCommit}\n${repo.name.lowercase()}"]
        if (repo.rootCommit.isNotEmpty() && byRoot != null) return byRoot
        if (repo.source == "remote-clone") {
            val upstream = cloneUrlRoute(repo.cloneUrl)
            if (upstream != null && upstream.second.lowercase() == repo.name.lowercase()) {
                return orgByRoute[route(upstream.first, upstream.second)] ?: upstream.first
            }
        }
        return repo.owner
    }
    val groups = LinkedHashMap<String, MutableList<Repository>>()
    val publicOwners = HashMap<String, String>()
    for (repo in repos) {
        val publicOwner = publicOwnerOf(repo)
        val key = "${route(publicOwner, repo.name)}\n${repo.isPrivate}"
        groups.getOrPut(key) { ArrayList() }.add(repo)
        publicOwners.putIfAbsent(key, publicOwner)
    }
    return groups.map { (key, copies) ->
        val publicOwner = publicOwners.getValue(key)
        groupRepresentative(copies, publicOwner).merged(publicOwner, copies)
    }
}

private fun groupRepresentative(copies: List<Repository>, publicOwner: String): Repository {
    val wanted = publicOwner.lowercase()
    copies.firstOrNull { it.organization.lowercase() == wanted }?.let { return it }
    copies.firstOrNull { it.owner.lowercase() == wanted }?.let { return it }
    val live = copies.filter { it.liveHost }
    val pool = live.ifEmpty { copies }
    return pool.reduce { a, b -> if (b.updatedMs > a.updatedMs) b else a }
}
