package com.forkmesh.nativeapp.docs

import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.json.JsonMap
import com.forkmesh.nativeapp.json.asJsonMap
import com.forkmesh.nativeapp.json.bool
import com.forkmesh.nativeapp.json.int
import com.forkmesh.nativeapp.json.maps
import com.forkmesh.nativeapp.json.str
import com.forkmesh.nativeapp.json.strings
import java.net.URI

data class DocsArea(val key: String, val label: String, val blurb: String)

class DocsPage(
    val path: String,
    val title: String,
    val description: String,
    val area: String,
    val url: String,
    val asset: Boolean,
    val markdown: String,
) {
    internal val searchText: String by lazy { markdown.lowercase() }
}

data class DocsHit(val path: String, val title: String, val snippet: String, val score: Int)

data class DocsAuthor(
    val login: String,
    val name: String,
    val profile: String,
    val projects: String,
    val website: String,
    val publicRepos: Int,
    val contributions: Int,
) {
    val handle: String get() = if (name != login) "@$login" else ""

    val commitsLabel: String
        get() = when {
            contributions <= 0 -> ""
            contributions == 1 -> "1 commit"
            else -> String.format(java.util.Locale.US, "%,d commits", contributions)
        }

    val projectsLabel: String
        get() = when {
            publicRepos == 1 -> "1 project"
            publicRepos > 1 -> String.format(java.util.Locale.US, "%,d projects", publicRepos)
            else -> "Projects"
        }
}

data class DocsDependency(
    val ecosystem: String,
    val name: String,
    val title: String,
    val version: String,
    val development: Boolean,
    val usedBy: List<String>,
    val description: String,
    val repository: String,
    val authors: List<DocsAuthor>,
    val authorsNote: String,
    val fetched: String,
) {
    val id: String get() = "$ecosystem:$name"

    val repositoryLabel: String get() = repository.removePrefix(DocsLibrary.GITHUB).trimEnd('/')

    val versionLabel: String get() = version.ifEmpty { "unpinned" }

    val authorsFallback: String get() = authorsNote.ifEmpty { "Top authors not recorded yet." }
}

data class DocsEcosystem(val key: String, val label: String, val dependencies: List<DocsDependency>)

sealed class DocsTarget {
    data class Anchor(val fragment: String) : DocsTarget()
    data class Page(val path: String, val fragment: String) : DocsTarget()
    data class External(val url: String) : DocsTarget()
}

class DocsLibrary(
    val webOrigin: String,
    val sourceTree: String,
    val areas: List<DocsArea>,
    val pages: List<DocsPage>,
    val dependenciesGenerated: String,
    val dependencies: List<DocsDependency>,
) {
    private val index: Map<String, Int> = pages.withIndex().associate { it.value.path to it.index }

    val ecosystems: List<DocsEcosystem> = groupEcosystems(dependencies)

    val homePath: String get() = if ("README.md" in index) "README.md" else pages.firstOrNull()?.path.orEmpty()

    val documentAreas: List<DocsArea> get() = areas.filter { it.key != DEPENDENCIES_AREA }

    val dependenciesArea: DocsArea?
        get() = areas.firstOrNull { it.key == DEPENDENCIES_AREA } ?: if (dependencies.isEmpty()) null
        else DocsArea(DEPENDENCIES_AREA, "Dependencies", "")

    val dependenciesSummary: String
        get() = if (dependencies.size == 1) "1 open-source dependency" else "${dependencies.size} open-source dependencies"

    fun page(path: String): DocsPage? = index[path]?.let(pages::get)

    fun indexOf(path: String): Int = index[path] ?: -1

    fun pagesIn(area: String): List<DocsPage> = pages.filter { it.area == area }

    fun previous(path: String): DocsPage? = indexOf(path).takeIf { it > 0 }?.let { pages[it - 1] }

    fun next(path: String): DocsPage? = indexOf(path).takeIf { it >= 0 && it + 1 < pages.size }?.let { pages[it + 1] }

    fun dependency(id: String): DocsDependency? = dependencies.firstOrNull { it.id == id }

    fun sourceUrl(repositoryPath: String): String = sourceTree + repositoryPath

    fun search(query: String, limit: Int = 50): List<DocsHit> {
        val phrase = collapse(query).lowercase()
        val tokens = phrase.split(' ').filter { it.isNotEmpty() }
        if (tokens.isEmpty() || limit <= 0) return emptyList()
        val hits = ArrayList<DocsHit>()
        for (page in pages) {
            val title = page.title.lowercase()
            val description = page.description.lowercase()
            val path = page.path.lowercase()
            var score = if (title.contains(phrase)) 200 else 0
            var bodyToken = ""
            var matched = true
            for (token in tokens) {
                val body = minOf(10, occurrences(page.searchText, token))
                when {
                    title.contains(token) -> score += if (title.startsWith(token)) 150 else 100
                    description.contains(token) -> score += 25
                    path.contains(token) -> score += 15
                    body == 0 -> {
                        matched = false
                    }
                }
                if (!matched) break
                if (body > 0 && bodyToken.isEmpty()) bodyToken = token
                score += body
            }
            if (!matched) continue
            var snippet = if (page.searchText.contains(phrase)) snippet(page.markdown, phrase) else ""
            if (snippet.isEmpty() && bodyToken.isNotEmpty()) snippet = snippet(page.markdown, bodyToken)
            if (snippet.isEmpty() || snippet == page.title) snippet = page.description
            hits += DocsHit(page.path, page.title, snippet, score)
        }
        return hits.sortedByDescending { it.score }.take(limit)
    }

    fun searchDependencies(query: String): List<DocsDependency> {
        val tokens = collapse(query).lowercase().split(' ').filter { it.isNotEmpty() }
        if (tokens.isEmpty()) return dependencies
        return dependencies.filter { dependency ->
            val haystack = buildString {
                append(dependency.name).append('\n').append(dependency.title).append('\n').append(dependency.description)
                for (author in dependency.authors) append('\n').append(author.name).append('\n').append(author.login)
            }.lowercase()
            tokens.all(haystack::contains)
        }
    }

    fun resolve(fromPath: String, href: String): DocsTarget? {
        val source = href.trim()
        if (source.isEmpty()) return null
        if (source.startsWith("#")) return DocsTarget.Anchor(percentDecode(source.substring(1)))
        if (source.startsWith("//")) return DocsTarget.External("https:$source")
        if (source.startsWith("/")) return DocsTarget.External(webOrigin + source)
        if (SCHEME.containsMatchIn(source)) {
            val scheme = source.substringBefore(':').lowercase()
            return if (scheme == "http" || scheme == "https" || scheme == "mailto") DocsTarget.External(source) else null
        }
        var target = source
        var fragment = ""
        val hash = target.indexOf('#')
        if (hash >= 0) {
            fragment = percentDecode(target.substring(hash + 1))
            target = target.substring(0, hash)
        }
        val query = target.indexOf('?')
        if (query >= 0) target = target.substring(0, query)
        val base = REPOSITORY_PREFIX + fromPath
        val repositoryPath = normalise(base.substringBeforeLast('/', "") + "/" + percentDecode(target))
        pagePathFor(repositoryPath)?.let { return DocsTarget.Page(it, fragment) }
        val suffix = if (fragment.isEmpty()) "" else "#$fragment"
        return if (repositoryPath.startsWith(PUBLIC_PREFIX)) {
            DocsTarget.External(webOrigin + "/" + repositoryPath.removePrefix(PUBLIC_PREFIX) + suffix)
        } else {
            DocsTarget.External(sourceUrl(repositoryPath) + suffix)
        }
    }

    private fun pagePathFor(repositoryPath: String): String? {
        if (repositoryPath == REPOSITORY_PREFIX.trimEnd('/')) return "README.md".takeIf { it in index }
        if (!repositoryPath.startsWith(REPOSITORY_PREFIX)) return null
        val inner = repositoryPath.removePrefix(REPOSITORY_PREFIX)
        if (inner in index) return inner
        return "$inner/README.md".takeIf { it in index }
    }

    companion object {
        const val SCHEMA = "forkmesh-docs-library-v1"
        const val GITHUB = "https://github.com/"
        const val DEPENDENCIES_AREA = "dependencies"
        const val REPOSITORY_PREFIX = "www/docs/"
        const val PUBLIC_PREFIX = "www/public/"
        const val TOP_AUTHORS = 3
        private const val DESCRIPTION_LIMIT = 180
        private const val SNIPPET_WIDTH = 140
        private val SCHEME = Regex("^[a-zA-Z][a-zA-Z0-9+.-]*:")
        private val LOGIN = Regex("^[A-Za-z0-9][A-Za-z0-9-]{0,38}$")
        private val SNIPPET_LEAD = Regex("^((?:[#>|+-]|\\*(?!\\*))+|\\d+\\.)\\s*")

        private val ECOSYSTEM_LABELS = mapOf(
            "cpp" to "Desktop (C++)",
            "python" to "Python",
            "go" to "Go",
            "npm" to "JavaScript (npm)",
            "vendored" to "Bundled web libraries",
            "maven" to "Android (Gradle)",
            "swift" to "Swift packages",
        )

        fun ecosystemLabel(ecosystem: String): String = ECOSYSTEM_LABELS[ecosystem] ?: ecosystem

        fun parse(text: String): DocsLibrary? {
            val root = try {
                Json.parse(text).asJsonMap()
            } catch (_: Json.JsonException) {
                return null
            }
            if (root.str("schema") != SCHEMA) return null
            val webOrigin = webLink(root.str("webOrigin")).trimEnd('/').ifEmpty { "https://forkmesh.com" }
            val sourceTree = webLink(root.str("sourceTree")).ifEmpty { "https://app.forkmesh.com/forkmesh/forkmesh/tree/main/" }
                .let { if (it.endsWith("/")) it else "$it/" }
            val areas = root.maps("areas").mapNotNull { item ->
                val key = collapse(item.str("key"))
                if (key.isEmpty()) null else DocsArea(key, collapse(item.str("label")).ifEmpty { key }, collapse(item.str("blurb")))
            }.distinctBy { it.key }
            val seen = HashSet<String>()
            val pages = root.maps("pages").mapNotNull { item ->
                val path = item.str("path").trim()
                if (!relativePath(path) || !seen.add(path)) return@mapNotNull null
                DocsPage(
                    path = path,
                    title = collapse(item.str("title")).ifEmpty { path.substringAfterLast('/').removeSuffix(".md") },
                    description = collapse(item.str("description")),
                    area = collapse(item.str("area")).ifEmpty { "guides" },
                    url = webLink(item.str("url")),
                    asset = item.bool("asset"),
                    markdown = item.str("markdown"),
                )
            }
            val dependencies = root.maps("dependencies").mapNotNull(::dependency)
            return DocsLibrary(webOrigin, sourceTree, areas, pages, collapse(root.str("dependenciesGenerated")), dependencies)
        }

        private fun dependency(item: JsonMap): DocsDependency? {
            val ecosystem = collapse(item.str("ecosystem"))
            val name = collapse(item.str("name"))
            if (ecosystem.isEmpty() || name.isEmpty()) return null
            val authors = ArrayList<DocsAuthor>()
            for (entry in item.maps("authors")) {
                val login = collapse(entry.str("login"))
                if (!LOGIN.matches(login)) continue
                authors += DocsAuthor(
                    login = login,
                    name = collapse(entry.str("name")).ifEmpty { login },
                    profile = gitHubLink(entry.str("profile")).ifEmpty { GITHUB + login },
                    projects = gitHubLink(entry.str("projects")).ifEmpty { "$GITHUB$login?tab=repositories" },
                    website = webLink(entry.str("website")),
                    publicRepos = maxOf(0, entry.int("publicRepos")),
                    contributions = maxOf(0, entry.int("contributions")),
                )
                if (authors.size == TOP_AUTHORS) break
            }
            return DocsDependency(
                ecosystem = ecosystem,
                name = name,
                title = collapse(item.str("title")).ifEmpty { name },
                version = collapse(item.str("version")),
                development = item.bool("development"),
                usedBy = item.strings("usedBy").map(String::trim).filter(::relativePath),
                description = truncated(collapse(item.str("description")), DESCRIPTION_LIMIT),
                repository = gitHubLink(item.str("repository")),
                authors = authors,
                authorsNote = collapse(item.str("authorsNote")),
                fetched = collapse(item.str("fetched")),
            )
        }

        fun groupEcosystems(dependencies: List<DocsDependency>): List<DocsEcosystem> {
            val grouped = LinkedHashMap<String, MutableList<DocsDependency>>()
            for (dependency in dependencies) grouped.getOrPut(dependency.ecosystem) { ArrayList() } += dependency
            return grouped.map { (key, items) -> DocsEcosystem(key, ecosystemLabel(key), items) }
        }

        fun webLink(value: String): String {
            val text = value.trim()
            if (text.isEmpty() || text.any { it.isWhitespace() }) return ""
            val uri = try {
                URI(text)
            } catch (_: Exception) {
                return ""
            }
            val scheme = uri.scheme?.lowercase() ?: return ""
            if (scheme != "https" && scheme != "http") return ""
            if (uri.host.isNullOrEmpty()) return ""
            return text
        }

        fun gitHubLink(value: String): String {
            val link = webLink(value)
            return if (link.startsWith(GITHUB) && link.length > GITHUB.length) link else ""
        }

        fun relativePath(path: String): Boolean =
            path.isNotEmpty() && !path.startsWith("/") && !path.contains("..") && !path.contains('\\') && !SCHEME.containsMatchIn(path)

        fun normalise(path: String): String {
            val parts = ArrayList<String>()
            for (part in path.split('/')) {
                when {
                    part.isEmpty() || part == "." -> Unit
                    part == ".." -> if (parts.isNotEmpty()) parts.removeAt(parts.lastIndex)
                    else -> parts += part
                }
            }
            return parts.joinToString("/")
        }

        fun percentDecode(text: String): String {
            if ('%' !in text) return text
            val out = StringBuilder()
            val bytes = java.io.ByteArrayOutputStream()
            var i = 0
            while (i < text.length) {
                val c = text[i]
                if (c == '%' && i + 2 < text.length) {
                    val value = text.substring(i + 1, i + 3).toIntOrNull(16)
                    if (value != null) {
                        bytes.write(value)
                        i += 3
                        continue
                    }
                }
                if (bytes.size() > 0) {
                    out.append(bytes.toString(Charsets.UTF_8.name()))
                    bytes.reset()
                }
                out.append(c)
                i++
            }
            if (bytes.size() > 0) out.append(bytes.toString(Charsets.UTF_8.name()))
            return out.toString()
        }

        fun snippet(markdown: String, needle: String): String {
            val at = markdown.indexOf(needle, ignoreCase = true)
            if (at < 0) return ""
            val start = markdown.lastIndexOf('\n', at) + 1
            val end = markdown.indexOf('\n', at).let { if (it < 0) markdown.length else it }
            var line = markdown.substring(start, end).trim()
            line = SNIPPET_LEAD.replaceFirst(line, "")
            line = DocsMarkdown.plain(line)
            if (line.length <= SNIPPET_WIDTH) return line
            val hit = line.indexOf(needle, ignoreCase = true)
            val from = (hit - SNIPPET_WIDTH / 3).coerceIn(0, line.length - SNIPPET_WIDTH)
            return (if (from > 0) "…" else "") + line.substring(from, from + SNIPPET_WIDTH).trim() +
                (if (from + SNIPPET_WIDTH < line.length) "…" else "")
        }

        internal fun collapse(text: String): String = text.trim().split(WHITESPACE).filter { it.isNotEmpty() }.joinToString(" ")

        private val WHITESPACE = Regex("\\s+")

        private fun occurrences(text: String, token: String): Int {
            if (token.isEmpty()) return 0
            var count = 0
            var from = text.indexOf(token)
            while (from >= 0) {
                count++
                from = text.indexOf(token, from + token.length)
            }
            return count
        }

        private fun truncated(text: String, limit: Int): String {
            var value = text.trimEnd(':')
            if (value.length <= limit) return value
            var cut = value.substring(0, limit - 1)
            val space = cut.lastIndexOf(' ')
            if (space > 0) cut = cut.substring(0, space)
            value = cut.trimEnd(',', ';', ':')
            return value + "…"
        }
    }
}
