package com.forkmesh.nativeapp.mastodon

import java.net.URI
import java.net.URLDecoder
import java.net.URLEncoder
import java.security.MessageDigest
import java.security.SecureRandom
import java.util.Base64

object Mastodon {
    const val PACKAGE_ID = "mastodon"
    const val REDIRECT = "forkmesh://mastodon/callback"
    const val OOB = "urn:ietf:wg:oauth:2.0:oob"
    const val SCOPES = "read write follow"
    const val CLIENT_NAME = "ForkMesh"
    const val WEBSITE = "https://forkmesh.com"
    const val PAGE = 30
    const val DEFAULT_MAX_CHARACTERS = 500
    const val DEFAULT_MAX_MEDIA = 4
    const val URL_LENGTH = 23

    fun revokedNotice(server: String): String =
        "Your Mastodon sign-in was revoked on ${MastodonServer.host(server)}. Sign in again."
}

object MastodonServer {
    fun normalize(input: String): String? {
        var text = input.trim().removePrefix("@")
        if (text.isEmpty()) return null
        if ("@" in text && "://" !in text) text = text.substringAfterLast('@')
        if ("://" !in text) text = "https://$text"
        val uri = try {
            URI(text)
        } catch (_: Exception) {
            return null
        }
        if (!uri.scheme.equals("https", ignoreCase = true)) return null
        val host = uri.host?.lowercase() ?: return null
        if ('.' !in host || host.startsWith('.') || host.endsWith('.')) return null
        if (uri.rawUserInfo != null) return null
        val path = uri.rawPath.orEmpty()
        if (path.isNotEmpty() && path != "/") return null
        if (uri.rawQuery != null || uri.rawFragment != null) return null
        val port = if (uri.port > 0) ":${uri.port}" else ""
        return "https://$host$port"
    }

    fun webLink(value: String): String {
        val text = value.trim()
        val lowered = text.lowercase()
        val start = when {
            lowered.startsWith("https://") -> "https://".length
            lowered.startsWith("http://") -> "http://".length
            else -> return ""
        }
        if (text.length == start || text[start] == '/' || text.any { it.isWhitespace() || it.isISOControl() }) return ""
        return text
    }

    fun host(server: String): String {
        val uri = runCatching { URI(server) }.getOrNull()
        val host = uri?.host ?: return server.removePrefix("https://")
        return if (uri.port > 0) "$host:${uri.port}" else host
    }
}

data class MastodonPkce(val verifier: String, val challenge: String, val state: String)

object MastodonAuth {
    private val random = SecureRandom()
    private val CODE = Regex("^[A-Za-z0-9_-]{4,256}$")

    fun pkce(): MastodonPkce {
        val bytes = ByteArray(32).also(random::nextBytes)
        val verifier = base64Url(bytes)
        val state = ByteArray(16).also(random::nextBytes).joinToString("") { "%02x".format(it.toInt() and 0xff) }
        return MastodonPkce(verifier, challenge(verifier), state)
    }

    fun challenge(verifier: String): String =
        base64Url(MessageDigest.getInstance("SHA-256").digest(verifier.toByteArray(Charsets.US_ASCII)))

    private fun base64Url(bytes: ByteArray): String = Base64.getUrlEncoder().withoutPadding().encodeToString(bytes)

    fun authorizeUrl(server: String, clientId: String, redirect: String, state: String, challenge: String): String =
        "$server/oauth/authorize?" + MastodonForm.encode(
            listOf(
                "response_type" to "code",
                "client_id" to clientId,
                "redirect_uri" to redirect,
                "scope" to Mastodon.SCOPES,
                "state" to state,
                "code_challenge" to challenge,
                "code_challenge_method" to "S256",
            ),
        )

    fun code(input: String, expectedState: String): String? {
        val text = input.trim()
        if (text.isEmpty()) return null
        val query = runCatching { URI(text) }.getOrNull()?.takeIf { it.scheme != null }?.rawQuery
        if (query != null) {
            val params = MastodonForm.decode(query)
            val code = params["code"]
            if (code != null) {
                if (params.containsKey("error")) return null
                val state = params["state"]
                if (state != null && state != expectedState) return null
                return code.takeIf { it.isNotEmpty() }
            }
        }
        return text.takeIf { CODE.matches(it) }
    }
}

object MastodonForm {
    fun encode(pairs: List<Pair<String, String>>): String =
        pairs.joinToString("&") { (key, value) -> "${enc(key)}=${enc(value)}" }

    fun enc(value: String): String = URLEncoder.encode(value, "UTF-8")

    fun decode(query: String): Map<String, String> = query.split('&').filter { it.isNotEmpty() }.associate { part ->
        val key = part.substringBefore('=')
        val value = if ('=' in part) part.substringAfter('=') else ""
        dec(key) to dec(value)
    }

    private fun dec(value: String): String = runCatching { URLDecoder.decode(value, "UTF-8") }.getOrDefault(value)
}

object MastodonText {
    private val URL = Regex("https?://\\S+")
    private val MENTION = Regex("@([A-Za-z0-9_]+)@[A-Za-z0-9.-]+")

    fun length(text: String): Int {
        val shortened = MENTION.replace(URL.replace(text, "x".repeat(Mastodon.URL_LENGTH)), "@$1")
        return shortened.codePointCount(0, shortened.length)
    }

    fun remaining(limit: Int, text: String, spoiler: String, warning: Boolean): Int =
        limit - length(text) - if (warning) length(spoiler) else 0
}

sealed class MastodonSegment {
    abstract val label: String

    data class Text(val text: String) : MastodonSegment() {
        override val label: String get() = text
    }

    data class Link(override val label: String, val url: String) : MastodonSegment()
    data class Mention(override val label: String, val accountId: String) : MastodonSegment()
    data class Hashtag(override val label: String, val name: String) : MastodonSegment()
}

object MastodonHtml {
    private val TAG_PATH = Regex("/tags/([^/?#]+)")
    private val ATTRIBUTE = Regex("([A-Za-z_:][-A-Za-z0-9_:.]*)\\s*(?:=\\s*(\"[^\"]*\"|'[^']*'|[^\\s\"'>]+))?")

    fun text(html: String): String = segments(html).joinToString("") { it.label }

    fun segments(html: String, mentions: List<MastodonMention> = emptyList(), tags: List<MastodonTag> = emptyList()): List<MastodonSegment> {
        val out = mutableListOf<MastodonSegment>()
        val text = StringBuilder()
        var anchor: Pair<String, String>? = null
        val label = StringBuilder()
        val spans = ArrayDeque<String>()
        var paragraphEnded = false
        fun emitText(value: String) {
            if (anchor != null) {
                if ("invisible" !in spans) label.append(value)
            } else {
                text.append(value)
            }
        }
        fun flush() {
            if (text.isNotEmpty()) {
                out += MastodonSegment.Text(text.toString())
                text.clear()
            }
        }
        var i = 0
        while (i < html.length) {
            val open = html.indexOf('<', i)
            if (open < 0) {
                emitText(decode(html.substring(i)))
                break
            }
            if (open > i) emitText(decode(html.substring(i, open)))
            val close = html.indexOf('>', open)
            if (close < 0) {
                emitText(decode(html.substring(open)))
                break
            }
            val raw = html.substring(open + 1, close).trim()
            i = close + 1
            val closing = raw.startsWith("/")
            val body = raw.removePrefix("/").removeSuffix("/").trim()
            val name = body.takeWhile { !it.isWhitespace() }.lowercase()
            val attributes = attributes(body.drop(name.length))
            when (name) {
                "br" -> emitText("\n")
                "p" -> if (closing) {
                    paragraphEnded = true
                } else if (paragraphEnded) {
                    emitText("\n\n")
                    paragraphEnded = false
                }
                "a" -> if (closing) {
                    val current = anchor
                    if (current != null) {
                        flush()
                        out += anchorSegment(label.toString(), current.first, current.second, mentions, tags)
                        anchor = null
                        label.clear()
                        spans.clear()
                    }
                } else if (anchor == null) {
                    anchor = attributes["href"].orEmpty() to attributes["class"].orEmpty()
                    label.clear()
                    spans.clear()
                }
                "span" -> if (anchor != null) {
                    if (closing) {
                        val kind = spans.removeLastOrNull()
                        if (kind == "ellipsis") label.append("…")
                    } else {
                        val classes = attributes["class"].orEmpty().split(' ')
                        spans.addLast(
                            when {
                                "invisible" in classes -> "invisible"
                                "ellipsis" in classes -> "ellipsis"
                                else -> "span"
                            },
                        )
                    }
                }
            }
        }
        anchor?.let { text.append(label) }
        flush()
        return trimEnd(merge(out))
    }

    private fun anchorSegment(label: String, href: String, classes: String, mentions: List<MastodonMention>, tags: List<MastodonTag>): MastodonSegment {
        mentions.firstOrNull { it.url.isNotEmpty() && it.url == href }?.let { return MastodonSegment.Mention(label, it.id) }
        val path = runCatching { URI(href).rawPath }.getOrNull().orEmpty()
        val tagMatch = TAG_PATH.find(path)
        val classList = classes.split(' ')
        if ("hashtag" in classList || tagMatch != null) {
            val name = tagMatch?.groupValues?.get(1)?.let { runCatching { URLDecoder.decode(it, "UTF-8") }.getOrDefault(it) }
                ?: tags.firstOrNull { it.url == href }?.name
                ?: label.removePrefix("#")
            return MastodonSegment.Hashtag(label, name.lowercase())
        }
        if (href.startsWith("https://", ignoreCase = true) || href.startsWith("http://", ignoreCase = true)) {
            return MastodonSegment.Link(label, href)
        }
        return MastodonSegment.Text(label)
    }

    private fun attributes(text: String): Map<String, String> {
        val out = HashMap<String, String>()
        for (match in ATTRIBUTE.findAll(text)) {
            val value = match.groupValues[2].let { if (it.length >= 2 && (it[0] == '"' || it[0] == '\'')) it.substring(1, it.length - 1) else it }
            out[match.groupValues[1].lowercase()] = decode(value)
        }
        return out
    }

    private fun merge(segments: List<MastodonSegment>): List<MastodonSegment> {
        val out = mutableListOf<MastodonSegment>()
        for (segment in segments) {
            val last = out.lastOrNull()
            if (segment is MastodonSegment.Text && last is MastodonSegment.Text) {
                out[out.lastIndex] = MastodonSegment.Text(last.text + segment.text)
            } else {
                out += segment
            }
        }
        return out
    }

    private fun trimEnd(segments: List<MastodonSegment>): List<MastodonSegment> {
        val out = segments.toMutableList()
        while (out.isNotEmpty()) {
            val last = out.last()
            if (last !is MastodonSegment.Text) break
            val trimmed = last.text.trimEnd()
            if (trimmed.isEmpty()) {
                out.removeAt(out.lastIndex)
            } else {
                out[out.lastIndex] = MastodonSegment.Text(trimmed)
                break
            }
        }
        return out
    }

    fun decode(text: String): String {
        if ('&' !in text) return text
        val out = StringBuilder()
        var i = 0
        while (i < text.length) {
            val c = text[i]
            val end = if (c == '&') text.indexOf(';', i) else -1
            if (end < 0 || end - i > 12) {
                out.append(c)
                i++
                continue
            }
            val entity = text.substring(i + 1, end)
            val decoded = when {
                entity == "amp" -> "&"
                entity == "lt" -> "<"
                entity == "gt" -> ">"
                entity == "quot" -> "\""
                entity == "apos" -> "'"
                entity == "nbsp" -> "\u00a0"
                entity.startsWith("#x") || entity.startsWith("#X") -> entity.drop(2).toIntOrNull(16)?.let(::codePoint)
                entity.startsWith("#") -> entity.drop(1).toIntOrNull()?.let(::codePoint)
                else -> null
            }
            if (decoded == null) {
                out.append(c)
                i++
            } else {
                out.append(decoded)
                i = end + 1
            }
        }
        return out.toString()
    }

    private fun codePoint(value: Int): String? =
        if (value in 0..0x10FFFF && value !in 0xD800..0xDFFF) String(Character.toChars(value)) else null
}

object MastodonPaging {
    fun next(linkHeader: String?, server: String): String? {
        if (linkHeader.isNullOrBlank()) return null
        for (part in linkHeader.split(Regex(",\\s*(?=<)"))) {
            val url = part.substringAfter('<', "").substringBefore('>', "")
            val params = part.substringAfter('>', "")
            val rel = Regex("rel\\s*=\\s*\"?([^\";]+)\"?").find(params)?.groupValues?.get(1).orEmpty()
            if ("next" in rel.split(' ') && url.startsWith("$server/")) return url
        }
        return null
    }
}

object MastodonCompose {
    val VISIBILITIES = listOf("public", "unlisted", "private", "direct")

    fun visibilityLabel(value: String): String = when (value) {
        "unlisted" -> "Unlisted"
        "private" -> "Followers only"
        "direct" -> "Mentioned people only"
        else -> "Public"
    }

    fun replyText(parent: MastodonStatus, me: String): String {
        val names = buildList {
            add(parent.account.acct)
            parent.mentions.forEach { add(it.acct) }
        }.filter { it.isNotEmpty() && !it.equals(me, ignoreCase = true) }.distinctBy { it.lowercase() }
        return if (names.isEmpty()) "" else names.joinToString(" ") { "@$it" } + " "
    }

    fun replyVisibility(parent: MastodonStatus): String = parent.visibility.takeIf { it in VISIBILITIES } ?: "public"
}
