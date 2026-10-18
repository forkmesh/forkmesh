package com.forkmesh.nativeapp.rssreader

import java.net.URI

data class RssUrlParts(
    val scheme: String,
    val authority: String,
    val host: String,
    val port: Int,
    val path: String,
    val query: String?,
    val fragment: String?,
    val hasUser: Boolean,
) {
    val origin: String get() = "$scheme://$authority"
}

object RssUrls {
    const val MAX_URL = 2048

    fun split(text: String): RssUrlParts? {
        val colon = text.indexOf(':')
        if (colon <= 0) return null
        val scheme = text.substring(0, colon).lowercase()
        if (!scheme.all { it.isLetterOrDigit() || it == '+' || it == '-' || it == '.' }) return null
        var rest = text.substring(colon + 1)
        var fragment: String? = null
        rest.indexOf('#').takeIf { it >= 0 }?.let { fragment = rest.substring(it + 1); rest = rest.substring(0, it) }
        var query: String? = null
        rest.indexOf('?').takeIf { it >= 0 }?.let { query = rest.substring(it + 1); rest = rest.substring(0, it) }
        if (!rest.startsWith("//")) return RssUrlParts(scheme, "", "", -1, rest, query, fragment, false)
        rest = rest.substring(2)
        val slash = rest.indexOf('/')
        val authority = if (slash >= 0) rest.substring(0, slash) else rest
        val path = if (slash >= 0) rest.substring(slash) else ""
        val at = authority.lastIndexOf('@')
        val hostPort = if (at >= 0) authority.substring(at + 1) else authority
        var host = hostPort
        var port = -1
        if (hostPort.startsWith("[")) {
            val close = hostPort.indexOf(']')
            if (close < 0) return null
            host = hostPort.substring(1, close)
            val tail = hostPort.substring(close + 1)
            if (tail.startsWith(":") && tail.length > 1) port = tail.substring(1).toIntOrNull() ?: return null
        } else {
            val portColon = hostPort.lastIndexOf(':')
            if (portColon >= 0) {
                host = hostPort.substring(0, portColon)
                val digits = hostPort.substring(portColon + 1)
                if (digits.isNotEmpty()) {
                    if (!digits.all { it.isDigit() }) return null
                    port = digits.toIntOrNull() ?: return null
                }
            }
        }
        if (port > 65535) return null
        return RssUrlParts(scheme, authority, host.lowercase(), port, path, query, fragment, at >= 0)
    }

    fun httpUrl(value: String?, base: String = ""): String {
        var text = value?.trim().orEmpty()
        if (text.isEmpty() || text.length > MAX_URL) return ""
        if (base.isNotEmpty()) text = join(base, text) ?: return ""
        val parts = split(text) ?: return ""
        if (parts.scheme != "http" && parts.scheme != "https") return ""
        if (parts.host.isEmpty() || parts.hasUser) return ""
        if (text.any { it.code < 0x20 }) return ""
        return text
    }

    fun join(base: String, value: String): String? {
        val target = split(value)
        val origin = split(base)
        if (target != null && target.scheme.isNotEmpty()) {
            if (origin == null || target.scheme != origin.scheme) return value
            if (target.authority.isEmpty() && !value.substring(value.indexOf(':') + 1).startsWith("//")) {
                return resolve(base, value.substring(value.indexOf(':') + 1))
            }
            return unsplit(target)
        }
        return resolve(base, value)
    }

    private fun unsplit(parts: RssUrlParts): String = buildString {
        append(parts.scheme).append("://").append(parts.authority).append(parts.path)
        if (!parts.query.isNullOrEmpty()) append('?').append(parts.query)
        if (!parts.fragment.isNullOrEmpty()) append('#').append(parts.fragment)
    }

    private fun resolve(base: String, relative: String): String? = runCatching {
        if (relative.isEmpty()) return@runCatching base
        val origin = split(base)
        val root = if (origin != null && origin.authority.isNotEmpty() && origin.path.isEmpty()) unsplit(origin.copy(path = "/")) else base
        URI(encode(root)).resolve(URI(encode(relative))).toString()
    }.getOrNull()

    fun encode(text: String): String {
        val out = StringBuilder(text.length)
        for (byte in text.toByteArray(Charsets.UTF_8)) {
            val code = byte.toInt() and 0xff
            if (code < 0x21 || code >= 0x7f || code.toChar() in "\"<>\\^`{|}") {
                out.append('%').append(HEX[code shr 4]).append(HEX[code and 15])
            } else {
                out.append(code.toChar())
            }
        }
        return out.toString()
    }

    private const val HEX = "0123456789ABCDEF"

    fun normalizeFeedInput(value: String?): String {
        var text = value?.trim().orEmpty()
        if (text.isEmpty()) return ""
        val lowered = text.lowercase()
        text = when {
            lowered.startsWith("feed://") -> "https://" + text.substring(7)
            lowered.startsWith("feed:") -> text.substring(5)
            lowered.startsWith("//") -> "https:$text"
            else -> text
        }
        if (!text.contains("://")) text = "https://$text"
        return httpUrl(text)
    }

    fun identity(url: String): String {
        val parts = split(url) ?: return url.lowercase()
        val port = if (parts.port > 0 && parts.port != 80 && parts.port != 443) ":${parts.port}" else ""
        val path = parts.path.ifEmpty { "/" }
        val query = if (!parts.query.isNullOrEmpty()) "?" + parts.query else ""
        return parts.host + port + path + query
    }

    fun linkKey(value: String): String {
        val parts = split(RssText.unescape(value.trim())) ?: return ""
        if (parts.authority.isEmpty() || parts.host.isEmpty()) return ""
        val scheme = if (parts.scheme == "https") "http" else parts.scheme
        val port = if (parts.port > 0 && parts.port != 80 && parts.port != 443) ":${parts.port}" else ""
        val query = parts.query?.takeIf { it.isNotEmpty() }?.let { "?$it" }.orEmpty()
        val fragment = parts.fragment?.takeIf { it.isNotEmpty() }?.let { "#$it" }.orEmpty()
        return "$scheme://${parts.host.trimEnd('.')}$port${parts.path.trimEnd('/')}$query$fragment"
    }

    fun origin(url: String): String = split(url)?.takeIf { it.authority.isNotEmpty() }?.origin.orEmpty()

    fun favicon(site: String): String {
        val parts = split(site) ?: return ""
        if ((parts.scheme == "http" || parts.scheme == "https") && parts.host.isNotEmpty()) return "${parts.scheme}://${parts.authority}/favicon.ico"
        return ""
    }

    fun withScheme(url: String, scheme: String): String {
        val colon = url.indexOf(':')
        return if (colon > 0) scheme + url.substring(colon) else url
    }

    fun host(url: String): String = split(url)?.host.orEmpty()
}
