package com.forkmesh.nativeapp.api

class ServerUrl private constructor(
    val scheme: String,
    val host: String,
    val port: Int?,
) {
    fun http(path: String, query: Map<String, String>? = null): String =
        build(if (scheme == "ws") "http" else "https", path, query)

    fun webSocket(path: String, query: Map<String, String>? = null): String {
        val secure = scheme != "ws" && scheme != "http"
        return build(if (secure) "wss" else "ws", path, query)
    }

    val httpOrigin: String
        get() {
            val target = if (scheme == "ws") "http" else "https"
            return target + "://" + authority(target)
        }

    private fun authority(targetScheme: String): String {
        val hostPart = if (host.contains(':')) "[$host]" else host
        val keptPort = port?.takeIf { it != defaultPort(targetScheme) }
        return if (keptPort == null) hostPart else "$hostPart:$keptPort"
    }

    private fun build(targetScheme: String, path: String, query: Map<String, String>?): String {
        val out = StringBuilder()
        out.append(targetScheme).append("://").append(authority(targetScheme))
        val cleanPath = if (path.isEmpty() || path.startsWith("/")) path else "/$path"
        out.append(DartUri.encodePath(cleanPath))
        if (!query.isNullOrEmpty()) out.append('?').append(DartUri.queryString(query))
        return out.toString()
    }

    companion object {
        private val PREFIX = Regex("^(?:([a-zA-Z][a-zA-Z0-9+.\\-]*):)?(?://([^/?#]*))?")

        private fun defaultPort(scheme: String): Int = when (scheme) {
            "http" -> 80
            "https" -> 443
            else -> 0
        }

        fun parse(url: String): ServerUrl {
            val match = PREFIX.find(url)
            val scheme = match?.groups?.get(1)?.value?.lowercase() ?: ""
            val authority = match?.groups?.get(2)?.value ?: ""
            val hostPort = authority.substring(authority.lastIndexOf('@') + 1)
            var host: String
            var portText = ""
            if (hostPort.startsWith("[")) {
                val close = hostPort.indexOf(']')
                host = if (close > 0) hostPort.substring(1, close) else hostPort.substring(1)
                if (close > 0 && close + 1 < hostPort.length && hostPort[close + 1] == ':') {
                    portText = hostPort.substring(close + 2)
                }
            } else {
                val colon = hostPort.lastIndexOf(':')
                if (colon >= 0) {
                    host = hostPort.substring(0, colon)
                    portText = hostPort.substring(colon + 1)
                } else {
                    host = hostPort
                }
            }
            host = DartUri.decodePercent(host).lowercase()
            val port = portText.toIntOrNull()?.takeIf { it != defaultPort(scheme) }
            return ServerUrl(scheme, host, port)
        }
    }
}

object DartUri {
    private const val HEX = "0123456789ABCDEF"
    private const val COMPONENT_EXTRA = "-_.!~*'()"
    private const val QUERY_EXTRA = "-._~"
    private const val PATH_EXTRA = "-._~!$&'()*+,;=:@/"

    fun encodeComponent(value: String): String = encode(value, COMPONENT_EXTRA, spaceToPlus = false, keepEscapes = false)

    fun encodeQueryComponent(value: String): String = encode(value, QUERY_EXTRA, spaceToPlus = true, keepEscapes = false)

    fun encodePath(path: String): String = encode(path, PATH_EXTRA, spaceToPlus = false, keepEscapes = true)

    fun queryString(params: Map<String, String>): String =
        params.entries.joinToString("&") { (key, value) -> encodeQueryComponent(key) + "=" + encodeQueryComponent(value) }

    fun decodePercent(text: String): String {
        if (!text.contains('%')) return text
        val bytes = java.io.ByteArrayOutputStream()
        var i = 0
        while (i < text.length) {
            val c = text[i]
            if (c == '%' && i + 2 < text.length && isHex(text[i + 1]) && isHex(text[i + 2])) {
                bytes.write(text.substring(i + 1, i + 3).toInt(16))
                i += 3
            } else {
                bytes.write(c.toString().toByteArray(Charsets.UTF_8))
                i++
            }
        }
        return String(bytes.toByteArray(), Charsets.UTF_8)
    }

    private fun isHex(c: Char): Boolean = c in '0'..'9' || c in 'a'..'f' || c in 'A'..'F'

    private fun encode(value: String, extra: String, spaceToPlus: Boolean, keepEscapes: Boolean): String {
        val out = StringBuilder(value.length + 8)
        var i = 0
        while (i < value.length) {
            val c = value[i]
            when {
                c in 'a'..'z' || c in 'A'..'Z' || c in '0'..'9' || extra.indexOf(c) >= 0 -> out.append(c)
                c == ' ' && spaceToPlus -> out.append('+')
                keepEscapes && c == '%' && i + 2 < value.length && isHex(value[i + 1]) && isHex(value[i + 2]) -> {
                    out.append('%').append(value[i + 1].uppercaseChar()).append(value[i + 2].uppercaseChar())
                    i += 2
                }
                else -> {
                    val end = if (Character.isHighSurrogate(c) && i + 1 < value.length) i + 2 else i + 1
                    for (byte in value.substring(i, end).toByteArray(Charsets.UTF_8)) {
                        val b = byte.toInt() and 0xff
                        out.append('%').append(HEX[b shr 4]).append(HEX[b and 0xf])
                    }
                    i = end - 1
                }
            }
            i++
        }
        return out.toString()
    }
}
