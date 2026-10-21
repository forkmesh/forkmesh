package com.forkmesh.nativeapp.chat

import com.forkmesh.nativeapp.ci.RepoRef
import com.forkmesh.nativeapp.collab.ReferenceLinks

data class ChatTextSegment(
    val text: String,
    val url: String? = null,
) {
    val isLink: Boolean get() = url != null

    override fun toString(): String = if (url == null) "text($text)" else "link($text -> $url)"
}

private val SAFE_SCHEMES = setOf("http", "https", "mailto")

fun safeChatLink(raw: String): String {
    val candidate = dartTrimText(raw.filter { it.code >= 0x20 && it.code != 0x7f })
    if (candidate.isEmpty() || candidate.length > 2048) return ""
    val uri = DartLinkUri.tryParse(candidate) ?: return ""
    if (uri.scheme.isEmpty()) return ""
    val scheme = uri.scheme.lowercase()
    if (scheme !in SAFE_SCHEMES) return ""
    if (scheme != "mailto" && uri.host.isEmpty()) return ""
    return uri.toString()
}

private const val JS_SPACE = "\\t\\n\\u000B\\f\\r \\u00A0\\u1680\\u2000-\\u200A\\u2028\\u2029\\u202F\\u205F\\u3000\\uFEFF"

private val RAW_URL = Regex("https?://[^$JS_SPACE<>]+", RegexOption.IGNORE_CASE)

private fun rawUrlAt(source: String, index: Int): String {
    val match = RAW_URL.matchAt(source, index) ?: return ""
    var url = match.value
    while (url.isNotEmpty()) {
        val last = url[url.length - 1]
        if (",.!?;:".indexOf(last) >= 0) {
            url = url.substring(0, url.length - 1)
            continue
        }
        if (last == ')' && url.count { it == ')' } > url.count { it == '(' }) {
            url = url.substring(0, url.length - 1)
            continue
        }
        break
    }
    return url
}

private fun linkTargetEnd(source: String, start: Int): Int {
    var depth = 0
    var index = start
    while (index < source.length) {
        val c = source[index]
        if (c == '\n') return -1
        if (c == '(') {
            depth += 1
        } else if (c == ')') {
            if (depth == 0) return index
            depth -= 1
        }
        index += 1
    }
    return -1
}

fun parseChatText(text: String): List<ChatTextSegment> {
    val segments = ArrayList<ChatTextSegment>()
    val buffer = StringBuilder()
    fun flush() {
        if (buffer.isEmpty()) return
        segments.add(ChatTextSegment(buffer.toString()))
        buffer.setLength(0)
    }

    var index = 0
    while (index < text.length) {
        if (text[index] == '[') {
            val labelEnd = text.indexOf("](", index + 1)
            val targetEnd = if (labelEnd > index + 1 && !text.substring(index, labelEnd).contains('\n')) {
                linkTargetEnd(text, labelEnd + 2)
            } else {
                -1
            }
            if (targetEnd > labelEnd + 1) {
                val label = text.substring(index + 1, labelEnd)
                val href = safeChatLink(text.substring(labelEnd + 2, targetEnd))
                if (href.isEmpty()) {
                    buffer.append(label)
                } else {
                    flush()
                    segments.add(ChatTextSegment(label, href))
                }
                index = targetEnd + 1
                continue
            }
        }
        val rawUrl = rawUrlAt(text, index)
        val rawHref = if (rawUrl.isEmpty()) "" else safeChatLink(rawUrl)
        if (rawHref.isNotEmpty()) {
            flush()
            segments.add(ChatTextSegment(rawUrl, rawHref))
            index += rawUrl.length
            continue
        }
        buffer.append(text[index])
        index += 1
    }
    flush()
    return segments
}

internal fun dartTrimText(text: String): String = com.forkmesh.nativeapp.crypto.dartTrim(text)

internal class DartLinkUri private constructor(
    val scheme: String,
    private val authority: String?,
    val host: String,
    private val path: String,
    private val query: String?,
    private val fragment: String?,
) {
    override fun toString(): String {
        val out = StringBuilder()
        if (scheme.isNotEmpty()) out.append(scheme).append(':')
        if (authority != null) out.append("//").append(authority)
        out.append(path)
        if (query != null) out.append('?').append(query)
        if (fragment != null) out.append('#').append(fragment)
        return out.toString()
    }

    companion object {
        private const val HEX = "0123456789ABCDEF"
        private val SCHEME = Regex("^([A-Za-z][A-Za-z0-9+.\\-]*):")
        private const val UNRESERVED = "-._~"
        private const val SUB_DELIMS = "!$&'()*+,;="

        fun tryParse(text: String): DartLinkUri? {
            var rest = text
            var scheme = ""
            val schemeMatch = SCHEME.find(rest)
            if (schemeMatch != null) {
                scheme = schemeMatch.groupValues[1].lowercase()
                rest = rest.substring(schemeMatch.value.length)
            }
            var fragment: String? = null
            val hash = rest.indexOf('#')
            if (hash >= 0) {
                fragment = rest.substring(hash + 1)
                rest = rest.substring(0, hash)
            }
            var query: String? = null
            val question = rest.indexOf('?')
            if (question >= 0) {
                query = rest.substring(question + 1)
                rest = rest.substring(0, question)
            }
            var authority: String? = null
            var host = ""
            if (rest.startsWith("//")) {
                val end = rest.indexOf('/', 2).let { if (it < 0) rest.length else it }
                val rawAuthority = rest.substring(2, end)
                rest = rest.substring(end)
                val at = rawAuthority.lastIndexOf('@')
                val userInfo = if (at >= 0) rawAuthority.substring(0, at) else null
                val hostPort = rawAuthority.substring(at + 1)
                var portText: String? = null
                if (hostPort.startsWith("[")) {
                    val close = hostPort.indexOf(']')
                    if (close < 0) return null
                    host = hostPort.substring(0, close + 1).lowercase()
                    val tail = hostPort.substring(close + 1)
                    if (tail.isNotEmpty()) {
                        if (!tail.startsWith(":")) return null
                        portText = tail.substring(1)
                    }
                } else {
                    val colon = hostPort.lastIndexOf(':')
                    if (colon >= 0) {
                        host = hostPort.substring(0, colon)
                        portText = hostPort.substring(colon + 1)
                    } else {
                        host = hostPort
                    }
                    host = encode(host, UNRESERVED + SUB_DELIMS) ?: return null
                    host = host.lowercase()
                }
                var port = ""
                if (!portText.isNullOrEmpty()) {
                    if (!portText.all { it in '0'..'9' }) return null
                    val value = portText.toBigInteger()
                    val default = when (scheme) {
                        "http" -> 80
                        "https" -> 443
                        else -> -1
                    }
                    if (value != default.toBigInteger()) port = ":$value"
                }
                val encodedUser = userInfo?.let { encode(it, UNRESERVED + SUB_DELIMS + ":") ?: return null }
                authority = (if (encodedUser != null) "$encodedUser@" else "") + host + port
            }
            val path = encode(rest, UNRESERVED + SUB_DELIMS + ":@/") ?: return null
            val encodedQuery = query?.let { encode(it, UNRESERVED + SUB_DELIMS + ":@/?") ?: return null }
            val encodedFragment = fragment?.let { encode(it, UNRESERVED + SUB_DELIMS + ":@/?") ?: return null }
            return DartLinkUri(scheme, authority, host, path, encodedQuery, encodedFragment)
        }

        private fun encode(text: String, allowed: String): String? {
            val out = StringBuilder()
            var i = 0
            while (i < text.length) {
                val c = text[i]
                when {
                    c == '%' -> {
                        if (i + 2 < text.length && isHex(text[i + 1]) && isHex(text[i + 2])) {
                            out.append('%').append(text[i + 1].uppercaseChar()).append(text[i + 2].uppercaseChar())
                            i += 3
                            continue
                        }
                        out.append("%25")
                    }
                    c.code < 128 && (c.isLetterOrDigit() || allowed.indexOf(c) >= 0) -> out.append(c)
                    else -> {
                        val end = if (Character.isHighSurrogate(c) && i + 1 < text.length) i + 2 else i + 1
                        for (byte in text.substring(i, end).toByteArray(Charsets.UTF_8)) {
                            val v = byte.toInt() and 0xff
                            out.append('%').append(HEX[v shr 4]).append(HEX[v and 0xf])
                        }
                        i = end
                        continue
                    }
                }
                i += 1
            }
            return out.toString()
        }

        private fun isHex(c: Char): Boolean = c in '0'..'9' || c in 'a'..'f' || c in 'A'..'F'
    }
}

fun linkChatReferences(segments: List<ChatTextSegment>, repo: RepoRef? = ReferenceLinks.CHAT_REPO): List<ChatTextSegment> =
    segments.flatMap { segment ->
        if (segment.isLink) {
            listOf(segment)
        } else {
            ReferenceLinks.runs(segment.text, repo, commits = false, mentions = false).map { ChatTextSegment(it.text, it.url) }
        }
    }
