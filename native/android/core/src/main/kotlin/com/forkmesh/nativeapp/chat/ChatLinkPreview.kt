package com.forkmesh.nativeapp.chat

import com.forkmesh.nativeapp.json.JsonMap
import com.forkmesh.nativeapp.json.jsonOf
import com.forkmesh.nativeapp.json.long
import com.forkmesh.nativeapp.json.str
import java.net.URI
import java.net.URLDecoder

data class ChatLinkPreview(
    val url: String,
    val title: String = "",
    val detail: String = "",
    val author: String = "",
    val siteName: String = "",
    val siteKey: String = "",
    val imageUrl: String = "",
    val kind: ChatLinkKind = ChatLinkKind.Page,
    val fetchedAtMs: Long = 0L,
) {
    val isEmpty: Boolean get() = title.isEmpty() && imageUrl.isEmpty()

    val site: ChatLinkSite? get() = if (siteKey.isEmpty()) null else ChatLinkSites.siteForKey(siteKey)

    val isVideo: Boolean get() = kind == ChatLinkKind.Video

    val byline: String get() = author.ifEmpty { detail }

    fun toJson(): Map<String, Any?> = jsonOf(
        "url" to url,
        "title" to title,
        "detail" to detail,
        "author" to author,
        "siteName" to siteName,
        "siteKey" to siteKey,
        "imageUrl" to imageUrl,
        "kind" to kind.raw,
        "fetchedAt" to fetchedAtMs / 1000L,
    )

    companion object {
        fun fromJson(value: JsonMap): ChatLinkPreview? {
            val url = value.str("url")
            if (url.isEmpty()) return null
            return ChatLinkPreview(
                url = url,
                title = value.str("title"),
                detail = value.str("detail"),
                author = value.str("author"),
                siteName = value.str("siteName"),
                siteKey = value.str("siteKey"),
                imageUrl = value.str("imageUrl"),
                kind = ChatLinkKind.of(value.str("kind")),
                fetchedAtMs = value.long("fetchedAt") * 1000L,
            )
        }
    }
}

object ChatLinkPreviewParsing {
    const val MAX_TITLE = 200
    const val MAX_DETAIL = 300
    val imageExtensions = setOf("png", "jpg", "jpeg", "gif", "webp", "heic", "avif")

    private val blockedSuffixes = listOf(
        "localhost", ".localhost", ".local", ".lan", ".home", ".internal", ".intranet", ".corp", ".home.arpa",
        ".localdomain", ".test", ".invalid", ".example", ".onion",
    )

    fun uri(raw: String): URI? = try {
        URI(raw)
    } catch (_: Exception) {
        null
    }

    fun links(text: String): List<String> {
        val out = ArrayList<String>()
        var inFence = false
        for (line in text.split('\n')) {
            if (line.trim().startsWith("```")) {
                inFence = !inFence
                continue
            }
            if (inFence) continue
            for (word in line.splitWhere { it.isWhitespace() || it == '<' || it == '>' }) {
                val start = word.indexOf("http", ignoreCase = true)
                if (start < 0) continue
                var candidate = word.substring(start)
                val lower = candidate.lowercase()
                if (!lower.startsWith("https://") && !lower.startsWith("http://")) continue
                val pipe = candidate.indexOf('|')
                if (pipe >= 0) candidate = candidate.substring(0, pipe)
                while (candidate.isNotEmpty()) {
                    val last = candidate.last()
                    if (last in ",.!?;:'\"*_~`") {
                        candidate = candidate.dropLast(1)
                        continue
                    }
                    if (last == ')' && candidate.count { it == ')' } > candidate.count { it == '(' }) {
                        candidate = candidate.dropLast(1)
                        continue
                    }
                    if (last == ']' && candidate.count { it == ']' } > candidate.count { it == '[' }) {
                        candidate = candidate.dropLast(1)
                        continue
                    }
                    break
                }
                if (candidate.length > 8 && candidate !in out) out.add(candidate)
            }
        }
        return out
    }

    private inline fun String.splitWhere(predicate: (Char) -> Boolean): List<String> {
        val out = ArrayList<String>()
        val current = StringBuilder()
        for (character in this) {
            if (predicate(character)) {
                if (current.isNotEmpty()) out.add(current.toString())
                current.setLength(0)
            } else {
                current.append(character)
            }
        }
        if (current.isNotEmpty()) out.add(current.toString())
        return out
    }

    fun candidates(hrefs: List<String>, limit: Int = 3): List<URI> {
        val seen = HashSet<String>()
        val out = ArrayList<URI>()
        for (href in hrefs) {
            val url = previewable(href) ?: continue
            if (!seen.add(url.toString())) continue
            out.add(url)
            if (out.size >= limit) break
        }
        return out
    }

    fun previewable(href: String): URI? {
        val url = uri(href.trim()) ?: return null
        val scheme = url.scheme?.lowercase() ?: return null
        if (scheme != "https" && scheme != "http") return null
        val host = url.host ?: return null
        if (!isPublicHost(host)) return null
        if (url.rawUserInfo != null) return null
        if (url.port != -1 && url.port != 443 && url.port != 80) return null
        return url
    }

    fun isPublicHost(raw: String): Boolean {
        var host = raw.lowercase()
        if (host.startsWith("[") && host.endsWith("]")) host = host.substring(1, host.length - 1)
        if (host.endsWith(".")) host = host.dropLast(1)
        if (host.isEmpty() || host.length > 253) return false
        if (':' in host) return isPublicIPv6Text(host)
        ipv4(host)?.let { return isPublicIPv4(it) }
        if ('.' !in host) return false
        val numeric = host.split('.').all { label ->
            label.all { it in '0'..'9' } ||
                (label.startsWith("0x") && label.drop(2).all { it in '0'..'9' || it in 'a'..'f' })
        }
        if (numeric) return false
        for (suffix in blockedSuffixes) {
            if (if (suffix.startsWith(".")) host.endsWith(suffix) else host == suffix) return false
        }
        return true
    }

    fun ipv4(host: String): IntArray? {
        val parts = host.split('.')
        if (parts.size != 4) return null
        val out = IntArray(4)
        for ((index, part) in parts.withIndex()) {
            if (part.isEmpty() || part.length > 3 || !part.all { it in '0'..'9' }) return null
            val value = part.toInt()
            if (value > 255) return null
            out[index] = value
        }
        return out
    }

    fun isPublicIPv4(o: IntArray): Boolean = when (o[0]) {
        0, 10, 127 -> false
        100 -> o[1] !in 64..127
        169 -> o[1] != 254
        172 -> o[1] !in 16..31
        192 -> !(o[1] == 168 || (o[1] == 0 && (o[2] == 0 || o[2] == 2)))
        198 -> !(o[1] == 18 || o[1] == 19 || (o[1] == 51 && o[2] == 100))
        203 -> !(o[1] == 0 && o[2] == 113)
        else -> o[0] < 224
    }

    fun isPublicIPv6Text(host: String): Boolean {
        val address = host.substringBefore('%')
        if (address == "::" || address == "::1") return false
        if (address.startsWith("::ffff:")) {
            val octets = ipv4(address.substring(7)) ?: return false
            return isPublicIPv4(octets)
        }
        val labels = address.split(':')
        val value = labels.firstOrNull()?.takeIf { it.isNotEmpty() && it.length <= 4 }?.toIntOrNull(16) ?: return false
        if (value and 0xFE00 == 0xFC00) return false
        if (value and 0xFFC0 == 0xFE80) return false
        if (value and 0xFF00 == 0xFF00) return false
        if (value == 0x2001) {
            val second = labels.getOrNull(1).orEmpty().lowercase()
            if (second == "db8" || second == "0db8") return false
        }
        return value and 0xE000 == 0x2000
    }

    fun lastPathComponent(url: URI): String = url.path.orEmpty().trimEnd('/').substringAfterLast('/')

    fun isImageLink(url: URI): Boolean {
        val last = lastPathComponent(url)
        if ('.' !in last) return false
        return last.substringAfterLast('.').lowercase() in imageExtensions
    }

    private fun pathParts(url: URI): List<String> = url.path.orEmpty().split('/').filter { it.isNotEmpty() }

    fun query(url: URI, name: String): String? {
        val raw = url.rawQuery ?: return null
        for (pair in raw.split('&')) {
            val key = decode(pair.substringBefore('='))
            if (key != name) continue
            return if ('=' in pair) decode(pair.substringAfter('=')) else ""
        }
        return null
    }

    private fun decode(text: String): String = try {
        URLDecoder.decode(text.replace("+", "%2B"), "UTF-8")
    } catch (_: Exception) {
        text
    }

    fun youtubeId(url: URI): String? {
        val host = url.host?.let(ChatLinkSites::bareHost) ?: return null
        val parts = pathParts(url)
        var candidate: String? = null
        if (host == "youtu.be") {
            candidate = parts.firstOrNull()
        } else if (host.endsWith("youtube.com") || host.endsWith("youtube-nocookie.com")) {
            if (parts.firstOrNull() == "watch" || parts.isEmpty()) {
                candidate = query(url, "v")
            } else if (parts.first() in setOf("shorts", "live", "embed", "v", "e") && parts.size > 1) {
                candidate = parts[1]
            }
        }
        val id = candidate ?: return null
        if (id.length < 6 || id.length > 20) return null
        if (!id.all { it in 'a'..'z' || it in 'A'..'Z' || it in '0'..'9' || it == '-' || it == '_' }) return null
        return id
    }

    fun wikipediaPage(url: URI): Pair<String, String>? {
        val host = url.host?.lowercase() ?: return null
        if (!host.endsWith("wikipedia.org")) return null
        val path = url.rawPath.orEmpty().removePrefix("/")
        val slash = path.indexOf('/')
        if (slash < 0) return null
        val first = path.substring(0, slash)
        val title = path.substring(slash + 1)
        if (first != "wiki" || title.isEmpty() || ':' in title || title.contains("%3A", ignoreCase = true)) return null
        return host.replace(".m.wikipedia.org", ".wikipedia.org") to title
    }

    fun hackerNewsId(url: URI): Long? {
        if (url.path != "/item") return null
        val id = query(url, "id")?.toLongOrNull() ?: return null
        return if (id > 0) id else null
    }

    fun packageName(url: URI, after: String): String? {
        val parts = pathParts(url)
        val at = parts.indexOf(after)
        if (at < 0 || at + 1 >= parts.size) return null
        var name = parts[at + 1]
        if (name.startsWith("@") && at + 2 < parts.size) name += "/" + parts[at + 2]
        return name
    }

    fun appStoreId(url: URI): String? {
        for (part in pathParts(url)) {
            if (!part.startsWith("id")) continue
            val digits = part.substring(2)
            if (digits.isNotEmpty() && digits.all { it in '0'..'9' }) return digits
        }
        return null
    }

    fun steamAppId(url: URI): String? {
        val parts = pathParts(url)
        val at = parts.indexOf("app")
        if (at < 0 || at + 1 >= parts.size || !parts[at + 1].all { it in '0'..'9' }) return null
        return parts[at + 1]
    }

    fun stackExchangeQuestion(url: URI): Pair<String, String>? {
        val host = url.host?.let(ChatLinkSites::bareHost) ?: return null
        val parts = pathParts(url)
        if (parts.size < 2 || parts[0] !in setOf("questions", "q", "a") || !parts[1].all { it in '0'..'9' }) return null
        val site = if (host.endsWith(".stackexchange.com")) host.dropLast(".stackexchange.com".length) else host.substringBefore('.')
        return if (site.isEmpty()) null else site to parts[1]
    }

    fun xkcdNumber(url: URI): String? {
        val parts = pathParts(url)
        if (parts.isEmpty()) return ""
        if (parts.size != 1 || !parts[0].all { it in '0'..'9' }) return null
        return parts[0]
    }

    fun parseHtml(html: String, page: URI): ChatLinkPreview {
        val meta = HashMap<String, String>()
        for (tag in tags("meta", html)) {
            val attributes = attributes(tag)
            val key = (attributes["property"] ?: attributes["name"] ?: attributes["itemprop"] ?: "").lowercase()
            val content = attributes["content"] ?: continue
            if (key.isEmpty()) continue
            val value = clean(decodeEntities(content))
            if (value.isEmpty() || meta.containsKey(key)) continue
            meta[key] = value
        }
        val title = first(meta, "og:title", "twitter:title", "title") ?: titleTag(html)
        val detail = first(meta, "og:description", "twitter:description", "description") ?: ""
        var image = first(meta, "og:image:secure_url", "og:image", "og:image:url", "twitter:image", "twitter:image:src", "image") ?: ""
        if (image.isEmpty()) image = linkImage(html)
        val siteName = first(meta, "og:site_name", "application-name") ?: ""
        var author = first(meta, "author", "article:author", "twitter:creator") ?: ""
        if (author.lowercase().startsWith("http")) author = ""
        val type = (meta["og:type"] ?: "").lowercase()
        val video = type.startsWith("video") || meta.containsKey("og:video") || meta.containsKey("og:video:url") ||
            meta.containsKey("twitter:player")
        return ChatLinkPreview(
            url = page.toString(),
            title = truncate(title, MAX_TITLE),
            detail = truncate(detail, MAX_DETAIL),
            author = truncate(author, 80),
            siteName = truncate(siteName.removePrefix("@"), 60),
            imageUrl = resolve(image, page),
            kind = if (video) ChatLinkKind.Video else ChatLinkKind.Page,
        )
    }

    private fun first(meta: Map<String, String>, vararg keys: String): String? {
        for (key in keys) meta[key]?.takeIf { it.isNotEmpty() }?.let { return it }
        return null
    }

    private fun lowered(text: String): String {
        val chars = CharArray(text.length)
        for (index in text.indices) chars[index] = text[index].lowercaseChar()
        return String(chars)
    }

    private fun isTagBreak(character: Char): Boolean =
        character == ' ' || character == '\t' || character == '\n' || character == '\r' || character == '/' || character == '>'

    fun tags(name: String, html: String): List<String> {
        val out = ArrayList<String>()
        val lower = lowered(html)
        val opener = "<$name"
        var cursor = 0
        while (true) {
            val start = lower.indexOf(opener, cursor)
            if (start < 0) break
            val after = start + opener.length
            if (after < lower.length && !isTagBreak(lower[after])) {
                cursor = after
                continue
            }
            val close = closingBracket(lower, after) ?: break
            out.add(html.substring(after, close))
            cursor = close + 1
        }
        return out
    }

    private fun closingBracket(text: String, from: Int): Int? {
        var quote: Char? = null
        var index = from
        while (index < text.length) {
            val character = text[index]
            val open = quote
            if (open != null) {
                if (character == open) quote = null
            } else if (character == '"' || character == '\'') {
                quote = character
            } else if (character == '>') {
                return index
            }
            index++
        }
        return null
    }

    private fun isSpace(character: Char): Boolean = character == ' ' || character == '\t' || character == '\n' || character == '\r'

    fun attributes(tag: String): Map<String, String> {
        val out = HashMap<String, String>()
        var i = 0
        val n = tag.length
        while (i < n) {
            while (i < n && (isSpace(tag[i]) || tag[i] == '/')) i++
            val name = StringBuilder()
            while (i < n && !(isSpace(tag[i]) || tag[i] == '/') && tag[i] != '=') {
                name.append(tag[i])
                i++
            }
            while (i < n && isSpace(tag[i])) i++
            val value = StringBuilder()
            if (i < n && tag[i] == '=') {
                i++
                while (i < n && isSpace(tag[i])) i++
                if (i < n && (tag[i] == '"' || tag[i] == '\'')) {
                    val quote = tag[i]
                    i++
                    while (i < n && tag[i] != quote) {
                        value.append(tag[i])
                        i++
                    }
                    i++
                } else {
                    while (i < n && !isSpace(tag[i])) {
                        value.append(tag[i])
                        i++
                    }
                }
            }
            val key = name.toString().lowercase()
            if (key.isNotEmpty() && !out.containsKey(key)) out[key] = value.toString()
        }
        return out
    }

    private fun titleTag(html: String): String {
        val lower = lowered(html)
        val open = lower.indexOf("<title")
        if (open < 0) return ""
        val openEnd = closingBracket(lower, open + 6) ?: return ""
        val close = lower.indexOf("</title>", openEnd)
        if (close < 0) return ""
        return clean(decodeEntities(html.substring(openEnd + 1, close)))
    }

    private fun linkImage(html: String): String {
        for (tag in tags("link", html)) {
            val attributes = attributes(tag)
            val href = attributes["href"] ?: continue
            if ((attributes["rel"] ?: "").lowercase() == "image_src") return decodeEntities(href)
        }
        return ""
    }

    fun resolveUri(raw: String, base: URI): URI? {
        val relative = uri(raw) ?: uri(raw.replace(" ", "%20")) ?: return null
        val anchor = if (base.rawPath.isNullOrEmpty() && base.isAbsolute && !base.isOpaque) {
            uri(base.scheme + "://" + base.rawAuthority + "/" + (base.rawQuery?.let { "?$it" } ?: "")) ?: base
        } else {
            base
        }
        return try {
            anchor.resolve(relative)
        } catch (_: Exception) {
            null
        }
    }

    fun resolve(raw: String, page: URI): String {
        val trimmed = raw.trim()
        if (trimmed.isEmpty()) return ""
        val url = resolveUri(trimmed, page) ?: return ""
        return if (previewable(url.toString()) != null) url.toString() else ""
    }

    fun clean(text: String): String {
        val out = StringBuilder(text.length)
        var pending = false
        for (character in text) {
            if (character.isWhitespace()) {
                pending = out.isNotEmpty()
            } else {
                if (pending) out.append(' ')
                pending = false
                out.append(character)
            }
        }
        return out.toString()
    }

    fun truncate(text: String, limit: Int): String {
        val cleaned = clean(text)
        if (cleaned.length <= limit) return cleaned
        var cut = cleaned.substring(0, limit - 1)
        if (cut.isNotEmpty() && cut.last().isHighSurrogate()) cut = cut.dropLast(1)
        return cut.trim() + "…"
    }

    private val namedEntities = mapOf(
        "amp" to "&", "lt" to "<", "gt" to ">", "quot" to "\"", "apos" to "'", "nbsp" to " ", "ndash" to "–",
        "mdash" to "—", "hellip" to "…", "rsquo" to "’", "lsquo" to "‘", "rdquo" to "”",
        "ldquo" to "“", "middot" to "·", "copy" to "©", "reg" to "®", "trade" to "™",
        "bull" to "•", "laquo" to "«", "raquo" to "»",
    )

    private fun scalar(code: Long?): String? {
        if (code == null || code < 0 || code > 0x10FFFF || code in 0xD800L..0xDFFFL) return null
        return String(Character.toChars(code.toInt()))
    }

    fun decodeEntities(text: String): String {
        if ('&' !in text) return text
        val out = StringBuilder(text.length)
        var index = 0
        while (index < text.length) {
            val character = text[index]
            val semi = if (character == '&') text.indexOf(';', index).takeIf { it >= 0 && it - index < 12 } else null
            if (semi == null) {
                out.append(character)
                index++
                continue
            }
            val body = text.substring(index + 1, semi)
            val replacement = when {
                body.startsWith("#x") || body.startsWith("#X") -> scalar(body.substring(2).toLongOrNull(16))
                body.startsWith("#") -> scalar(body.substring(1).toLongOrNull())
                else -> namedEntities[body.lowercase()]
            }
            if (replacement != null) {
                out.append(replacement)
                index = semi + 1
            } else {
                out.append(character)
                index++
            }
        }
        return out.toString()
    }

    fun stripTags(html: String): String {
        val out = StringBuilder(html.length)
        var inTag = false
        for (character in html) {
            when {
                character == '<' -> {
                    inTag = true
                    out.append(' ')
                }
                character == '>' -> inTag = false
                !inTag -> out.append(character)
            }
        }
        return clean(decodeEntities(out.toString()))
    }

    fun tweetText(html: String): String {
        val lower = lowered(html)
        val open = lower.indexOf("<p")
        if (open < 0) return stripTags(html)
        val openEnd = closingBracket(lower, open + 2) ?: return stripTags(html)
        val close = lower.indexOf("</p>", openEnd)
        if (close < 0) return stripTags(html)
        return stripTags(html.substring(openEnd + 1, close))
    }
}
