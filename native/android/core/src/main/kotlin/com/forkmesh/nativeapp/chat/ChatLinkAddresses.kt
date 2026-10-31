package com.forkmesh.nativeapp.chat

import java.io.ByteArrayOutputStream
import java.io.IOException
import java.io.InputStream
import java.net.HttpURLConnection
import java.net.InetAddress
import java.net.Proxy
import java.net.Socket
import java.net.URL
import javax.net.ssl.HttpsURLConnection
import javax.net.ssl.SSLSocketFactory

fun interface ChatHostResolver {
    fun resolve(host: String): List<ByteArray>
}

data class ChatLinkRequest(
    val url: String,
    val headers: Map<String, String>,
    val timeoutMs: Int,
    val maxBytes: Int,
)

class ChatLinkResponse(
    val status: Int,
    headers: Map<String, String> = emptyMap(),
    val body: ByteArray = ByteArray(0),
    val overflow: Boolean = false,
    val interrupted: Boolean = false,
) {
    val headers: Map<String, String> = headers.mapKeys { it.key.lowercase() }

    fun header(name: String): String? = headers[name.lowercase()]

    val ok: Boolean get() = status in 200..299

    fun with(name: String, value: String, body: ByteArray = this.body): ChatLinkResponse =
        ChatLinkResponse(status, headers + (name.lowercase() to value), body)
}

fun interface ChatLinkHttp {
    fun get(request: ChatLinkRequest): ChatLinkResponse?
}

class ChatLinkRefused(message: String) : IOException(message)

object ChatLinkAddresses {
    fun isPublic(raw: ByteArray): Boolean = when (raw.size) {
        4 -> ChatLinkPreviewParsing.isPublicIPv4(IntArray(4) { raw[it].toInt() and 0xff })
        16 -> isPublicIPv6(raw)
        else -> false
    }

    private fun u8(raw: ByteArray, index: Int): Int = raw[index].toInt() and 0xff

    private fun embedded(raw: ByteArray, from: Int): IntArray = IntArray(4) { u8(raw, from + it) }

    private fun isPublicIPv6(b: ByteArray): Boolean {
        val head = (u8(b, 0) shl 8) or u8(b, 1)
        val second = (u8(b, 2) shl 8) or u8(b, 3)
        if ((0 until 10).all { b[it].toInt() == 0 }) {
            if (u8(b, 10) == 0xff && u8(b, 11) == 0xff) return ChatLinkPreviewParsing.isPublicIPv4(embedded(b, 12))
            return false
        }
        if (head == 0x0064 && second == 0xff9b) {
            if ((4 until 12).all { b[it].toInt() == 0 }) return ChatLinkPreviewParsing.isPublicIPv4(embedded(b, 12))
            return false
        }
        if (head == 0x2002) return ChatLinkPreviewParsing.isPublicIPv4(embedded(b, 2))
        if (head == 0x2001 && (second == 0x0000 || second == 0x0db8 || (second and 0xfff0) == 0x0010 || (second and 0xfff0) == 0x0020)) {
            return false
        }
        if (head and 0xfe00 == 0xfc00 || head and 0xffc0 == 0xfe80 || head and 0xffc0 == 0xfec0 || head and 0xff00 == 0xff00) {
            return false
        }
        return head and 0xe000 == 0x2000
    }

    fun allPublic(addresses: List<ByteArray>): Boolean = addresses.isNotEmpty() && addresses.all(::isPublic)

    fun bare(host: String): String = if (host.startsWith("[") && host.endsWith("]")) host.substring(1, host.length - 1) else host

    val system = ChatHostResolver { host ->
        val name = bare(host)
        if (name.isEmpty()) {
            emptyList()
        } else {
            try {
                InetAddress.getAllByName(name).map { it.address }
            } catch (_: Exception) {
                emptyList()
            }
        }
    }

    fun checkConnected(socket: Socket) {
        val address = socket.inetAddress
        if (address == null || !isPublic(address.address)) throw ChatLinkRefused("refused a link preview to a private address")
    }
}

class ChatLinkGuardedHttp(
    private val base: ChatLinkHttp,
    private val resolver: ChatHostResolver = ChatLinkAddresses.system,
) : ChatLinkHttp {
    override fun get(request: ChatLinkRequest): ChatLinkResponse? {
        val url = ChatLinkPreviewParsing.previewable(request.url) ?: return null
        val host = url.host ?: return null
        if (!ChatLinkAddresses.allPublic(resolver.resolve(host))) return null
        return base.get(request)
    }
}

class ChatLinkPublicSocketFactory(private val base: SSLSocketFactory) : SSLSocketFactory() {
    private fun checked(socket: Socket): Socket {
        try {
            ChatLinkAddresses.checkConnected(socket)
        } catch (refused: ChatLinkRefused) {
            runCatching { socket.close() }
            throw refused
        }
        return socket
    }

    override fun createSocket(s: Socket, host: String?, port: Int, autoClose: Boolean): Socket {
        try {
            ChatLinkAddresses.checkConnected(s)
        } catch (refused: ChatLinkRefused) {
            if (autoClose) runCatching { s.close() }
            throw refused
        }
        return base.createSocket(s, host, port, autoClose)
    }

    override fun createSocket(host: String?, port: Int): Socket = checked(base.createSocket(host, port))

    override fun createSocket(host: String?, port: Int, localHost: InetAddress?, localPort: Int): Socket =
        checked(base.createSocket(host, port, localHost, localPort))

    override fun createSocket(host: InetAddress?, port: Int): Socket = checked(base.createSocket(host, port))

    override fun createSocket(address: InetAddress?, port: Int, localAddress: InetAddress?, localPort: Int): Socket =
        checked(base.createSocket(address, port, localAddress, localPort))

    override fun getDefaultCipherSuites(): Array<String> = base.defaultCipherSuites

    override fun getSupportedCipherSuites(): Array<String> = base.supportedCipherSuites
}

class ChatLinkUrlConnectionHttp(
    private val sockets: SSLSocketFactory = ChatLinkPublicSocketFactory(HttpsURLConnection.getDefaultSSLSocketFactory()),
    private val now: () -> Long = System::currentTimeMillis,
) : ChatLinkHttp {
    override fun get(request: ChatLinkRequest): ChatLinkResponse? {
        val target = if (request.url.startsWith("http://", ignoreCase = true)) "https://" + request.url.substring(7) else request.url
        val connection = try {
            URL(target).openConnection(Proxy.NO_PROXY) as? HttpsURLConnection
        } catch (_: Exception) {
            null
        } ?: return null
        val began = now()
        try {
            connection.sslSocketFactory = sockets
            connection.instanceFollowRedirects = false
            connection.useCaches = false
            connection.connectTimeout = request.timeoutMs
            connection.readTimeout = request.timeoutMs
            for ((name, value) in request.headers) connection.setRequestProperty(name, value)
            val status = connection.responseCode
            val headers = HashMap<String, String>()
            for ((name, values) in connection.headerFields) {
                if (name != null && values.isNotEmpty()) headers[name.lowercase()] = values.last()
            }
            if (status in 300..399 || status >= 400) return ChatLinkResponse(status, headers)
            val stream = connection.inputStream ?: return ChatLinkResponse(status, headers)
            return read(stream, status, headers, request, began)
        } catch (_: Exception) {
            return null
        } finally {
            connection.disconnect()
        }
    }

    private fun read(stream: InputStream, status: Int, headers: Map<String, String>, request: ChatLinkRequest, began: Long): ChatLinkResponse =
        stream.use { input ->
            val out = ByteArrayOutputStream()
            val buffer = ByteArray(16 * 1024)
            val deadline = began + request.timeoutMs * 2L
            try {
                while (true) {
                    val count = input.read(buffer)
                    if (count < 0) break
                    out.write(buffer, 0, count)
                    if (out.size() >= request.maxBytes) {
                        return@use ChatLinkResponse(status, headers, out.toByteArray().copyOf(request.maxBytes), overflow = true)
                    }
                    if (now() > deadline) return@use ChatLinkResponse(status, headers, out.toByteArray(), interrupted = true)
                }
            } catch (_: IOException) {
                return@use ChatLinkResponse(status, headers, out.toByteArray(), interrupted = true)
            }
            ChatLinkResponse(status, headers, out.toByteArray())
        }
}
