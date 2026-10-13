package com.forkmesh.nativeapp.net

import java.io.BufferedInputStream
import java.io.ByteArrayOutputStream
import java.io.Closeable
import java.io.EOFException
import java.io.IOException
import java.io.InputStream
import java.io.OutputStream
import java.net.InetSocketAddress
import java.net.Socket
import java.net.SocketTimeoutException
import java.net.URI
import java.net.URISyntaxException
import java.nio.ByteBuffer
import java.nio.charset.CharacterCodingException
import java.nio.charset.CodingErrorAction
import java.security.MessageDigest
import java.security.SecureRandom
import java.util.Base64
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.locks.ReentrantLock
import javax.net.ssl.HostnameVerifier
import javax.net.ssl.HttpsURLConnection
import javax.net.ssl.SNIHostName
import javax.net.ssl.SSLPeerUnverifiedException
import javax.net.ssl.SSLSocket
import javax.net.ssl.SSLSocketFactory

class WebSocketException(message: String, val status: Int = 0, cause: Throwable? = null) : IOException(message, cause)

class WebSocketClient private constructor(
    val url: String,
    private val socket: Socket,
    private val input: InputStream,
    private val output: OutputStream,
    private val listener: Listener,
    private val options: Options,
    val protocol: String,
    val responseHeaders: Map<String, List<String>>,
) {
    interface Listener {
        fun onText(socket: WebSocketClient, text: String) {}

        fun onBinary(socket: WebSocketClient, data: ByteArray) {}

        fun onPing(socket: WebSocketClient, payload: ByteArray) {}

        fun onPong(socket: WebSocketClient, payload: ByteArray) {}

        fun onClosed(socket: WebSocketClient, code: Int, reason: String, error: Throwable?) {}
    }

    class Options {
        val headers = LinkedHashMap<String, String>()

        var protocols: List<String> = emptyList()

        var connectTimeoutMs: Int = 15_000

        var handshakeTimeoutMs: Int = 15_000

        var readTimeoutMs: Int = 0

        var maxMessageBytes: Int = 16 * 1024 * 1024

        var fragmentSize: Int = 0

        var closeTimeoutMs: Int = 5_000

        var sslSocketFactory: SSLSocketFactory? = null

        var hostnameVerifier: HostnameVerifier? = null

        fun header(name: String, value: String): Options = apply { headers[name] = value }
    }

    private val writeLock = ReentrantLock()
    private val closeSent = AtomicBoolean(false)
    private val closedNotified = AtomicBoolean(false)
    private val closedLatch = CountDownLatch(1)

    @Volatile
    private var state = STATE_OPEN

    @Volatile
    private var abortReason: Throwable? = null

    private lateinit var reader: Thread

    val isOpen: Boolean get() = state == STATE_OPEN

    fun sendText(text: String): Boolean = sendMessage(OP_TEXT, text.toByteArray(Charsets.UTF_8))

    fun sendBinary(data: ByteArray): Boolean = sendMessage(OP_BINARY, data)

    fun ping(payload: ByteArray = ByteArray(0)): Boolean {
        if (payload.size > MAX_CONTROL_PAYLOAD) return false
        return writeControl(OP_PING, payload)
    }

    fun close(code: Int = NORMAL_CLOSURE, reason: String = "") {
        require(code == NO_STATUS || isValidCloseCode(code)) { "invalid close code $code" }
        sendClose(code, reason)
        if (Thread.currentThread() === reader) return
        val timeout = options.closeTimeoutMs.toLong().coerceAtLeast(0)
        if (!await(timeout)) {
            abort(WebSocketException("WebSocket close handshake timed out"))
            await(timeout.coerceAtLeast(1_000))
        }
    }

    fun cancel() {
        state = STATE_CLOSED
        abort(WebSocketException("WebSocket connection cancelled"))
    }

    fun awaitClosed(timeoutMs: Long): Boolean {
        if (Thread.currentThread() === reader) return closedLatch.count == 0L
        return await(timeoutMs.coerceAtLeast(0))
    }

    private fun await(timeoutMs: Long): Boolean = try {
        closedLatch.await(timeoutMs, TimeUnit.MILLISECONDS)
    } catch (_: InterruptedException) {
        Thread.currentThread().interrupt()
        closedLatch.count == 0L
    }

    private fun start() {
        reader = Thread({ readLoop() }, "WebSocketClient-reader").apply { isDaemon = true }
        reader.start()
    }

    private fun sendMessage(opcode: Int, data: ByteArray): Boolean {
        writeLock.lock()
        try {
            if (state != STATE_OPEN || closeSent.get()) return false
            val step = options.fragmentSize
            if (step <= 0 || data.size <= step) return writeFrameLocked(true, opcode, data, 0, data.size)
            var offset = 0
            var frameOpcode = opcode
            while (offset < data.size) {
                val length = minOf(step, data.size - offset)
                val last = offset + length == data.size
                if (!writeFrameLocked(last, frameOpcode, data, offset, length)) return false
                frameOpcode = OP_CONTINUATION
                offset += length
            }
            return true
        } finally {
            writeLock.unlock()
        }
    }

    private fun writeControl(opcode: Int, payload: ByteArray): Boolean {
        writeLock.lock()
        try {
            if (state != STATE_OPEN || closeSent.get()) return false
            return writeFrameLocked(true, opcode, payload, 0, payload.size)
        } finally {
            writeLock.unlock()
        }
    }

    private fun sendClose(code: Int, reason: String): Boolean {
        if (state == STATE_CLOSED) return false
        if (!closeSent.compareAndSet(false, true)) return false
        if (state == STATE_OPEN) state = STATE_CLOSING
        val payload = closePayload(code, reason)
        try {
            socket.soTimeout = options.closeTimeoutMs.coerceAtLeast(1)
        } catch (_: IOException) {
        }
        val locked = try {
            writeLock.tryLock(options.closeTimeoutMs.toLong().coerceAtLeast(1), TimeUnit.MILLISECONDS)
        } catch (_: InterruptedException) {
            Thread.currentThread().interrupt()
            false
        }
        if (!locked) {
            abort(WebSocketException("timed out writing the WebSocket close frame"))
            return false
        }
        try {
            return writeFrameLocked(true, OP_CLOSE, payload, 0, payload.size)
        } finally {
            writeLock.unlock()
        }
    }

    private fun writeFrameLocked(fin: Boolean, opcode: Int, data: ByteArray, offset: Int, length: Int): Boolean =
        try {
            output.write(encodeFrame(fin, opcode, data, offset, length))
            output.flush()
            true
        } catch (error: IOException) {
            abort(error)
            false
        }

    private fun encodeFrame(fin: Boolean, opcode: Int, data: ByteArray, offset: Int, length: Int): ByteArray {
        val lengthBytes = when {
            length <= 125 -> 0
            length <= 0xFFFF -> 2
            else -> 8
        }
        val frame = ByteArray(2 + lengthBytes + 4 + length)
        frame[0] = ((if (fin) 0x80 else 0) or opcode).toByte()
        var pos = 2
        when (lengthBytes) {
            0 -> frame[1] = (0x80 or length).toByte()
            2 -> {
                frame[1] = (0x80 or 126).toByte()
                frame[2] = (length shr 8).toByte()
                frame[3] = length.toByte()
                pos = 4
            }
            else -> {
                frame[1] = (0x80 or 127).toByte()
                for (i in 0 until 8) frame[2 + i] = (length.toLong() shr (56 - 8 * i)).toByte()
                pos = 10
            }
        }
        val mask = ByteArray(4).also { RANDOM.nextBytes(it) }
        System.arraycopy(mask, 0, frame, pos, 4)
        pos += 4
        for (i in 0 until length) {
            frame[pos + i] = (data[offset + i].toInt() xor mask[i and 3].toInt()).toByte()
        }
        return frame
    }

    private fun closePayload(code: Int, reason: String): ByteArray {
        if (code == NO_STATUS) return ByteArray(0)
        val reasonBytes = truncateUtf8(reason, MAX_CONTROL_PAYLOAD - 2)
        val payload = ByteArray(2 + reasonBytes.size)
        payload[0] = (code shr 8).toByte()
        payload[1] = code.toByte()
        reasonBytes.copyInto(payload, 2)
        return payload
    }

    private fun abort(reason: Throwable) {
        if (abortReason == null) abortReason = reason
        closeQuietly(socket)
    }

    private class Violation(val code: Int, message: String, cause: Throwable? = null) : Exception(message, cause)

    private fun readLoop() {
        var code = ABNORMAL
        var reason = ""
        var error: Throwable? = null
        val message = ByteArrayOutputStream()
        var messageOpcode = -1
        try {
            while (true) {
                val b0 = readByte()
                val b1 = readByte()
                val fin = (b0 and 0x80) != 0
                if ((b0 and 0x70) != 0) throw Violation(PROTOCOL_ERROR, "reserved bits set without a negotiated extension")
                val opcode = b0 and 0x0f
                if ((b1 and 0x80) != 0) throw Violation(PROTOCOL_ERROR, "server frames must not be masked")
                var length = (b1 and 0x7f).toLong()
                if (length == 126L) {
                    length = ((readByte() shl 8) or readByte()).toLong()
                } else if (length == 127L) {
                    length = 0
                    repeat(8) { length = (length shl 8) or readByte().toLong() }
                    if (length < 0) throw Violation(PROTOCOL_ERROR, "frame length has its most significant bit set")
                }
                when (opcode) {
                    OP_CLOSE, OP_PING, OP_PONG -> {
                        if (!fin) throw Violation(PROTOCOL_ERROR, "control frames must not be fragmented")
                        if (length > MAX_CONTROL_PAYLOAD) throw Violation(PROTOCOL_ERROR, "control frame payload longer than 125 bytes")
                        val payload = readBytes(length.toInt())
                        when (opcode) {
                            OP_PING -> {
                                writeControl(OP_PONG, payload)
                                callback { listener.onPing(this, payload) }
                            }
                            OP_PONG -> callback { listener.onPong(this, payload) }
                            else -> {
                                if (payload.size == 1) throw Violation(PROTOCOL_ERROR, "close frame with a one-byte payload")
                                var peerCode = NO_STATUS
                                var peerReason = ""
                                if (payload.size >= 2) {
                                    peerCode = ((payload[0].toInt() and 0xff) shl 8) or (payload[1].toInt() and 0xff)
                                    if (!isValidCloseCode(peerCode)) throw Violation(PROTOCOL_ERROR, "invalid close code $peerCode")
                                    peerReason = decodeUtf8(payload, 2, payload.size - 2)
                                        ?: throw Violation(INVALID_PAYLOAD, "close reason is not valid UTF-8")
                                }
                                sendClose(peerCode, "")
                                code = peerCode
                                reason = peerReason
                                return
                            }
                        }
                    }
                    OP_TEXT, OP_BINARY -> {
                        if (messageOpcode != -1) {
                            throw Violation(PROTOCOL_ERROR, "new data frame before the fragmented message finished")
                        }
                        checkSize(length, 0)
                        val payload = readBytes(length.toInt())
                        if (fin) {
                            deliver(opcode, payload)
                        } else {
                            messageOpcode = opcode
                            message.reset()
                            message.write(payload)
                        }
                    }
                    OP_CONTINUATION -> {
                        if (messageOpcode == -1) throw Violation(PROTOCOL_ERROR, "continuation frame without a message to continue")
                        checkSize(length, message.size())
                        message.write(readBytes(length.toInt()))
                        if (fin) {
                            val finished = messageOpcode
                            messageOpcode = -1
                            val bytes = message.toByteArray()
                            message.reset()
                            deliver(finished, bytes)
                        }
                    }
                    else -> throw Violation(PROTOCOL_ERROR, "unknown opcode $opcode")
                }
            }
        } catch (violation: Violation) {
            code = violation.code
            reason = violation.message ?: ""
            error = violation.cause ?: WebSocketException(reason)
            sendClose(violation.code, if (violation.cause == null) reason else "")
        } catch (timeout: SocketTimeoutException) {
            code = ABNORMAL
            error = abortReason ?: if (closeSent.get()) {
                WebSocketException("WebSocket close handshake timed out", 0, timeout)
            } else {
                timeout
            }
        } catch (failure: Exception) {
            code = ABNORMAL
            error = abortReason ?: failure
        } finally {
            state = STATE_CLOSED
            closeQuietly(socket)
            if (code == ABNORMAL && error == null) {
                error = abortReason ?: WebSocketException("WebSocket connection closed abnormally")
            }
            try {
                if (closedNotified.compareAndSet(false, true)) listener.onClosed(this, code, reason, error)
            } finally {
                closedLatch.countDown()
            }
        }
    }

    private inline fun callback(block: () -> Unit) {
        try {
            block()
        } catch (failure: RuntimeException) {
            throw Violation(INTERNAL_ERROR, "listener failed: $failure", failure)
        }
    }

    private fun deliver(opcode: Int, payload: ByteArray) {
        if (opcode == OP_TEXT) {
            val text = decodeUtf8(payload, 0, payload.size)
                ?: throw Violation(INVALID_PAYLOAD, "text message is not valid UTF-8")
            callback { listener.onText(this, text) }
        } else {
            callback { listener.onBinary(this, payload) }
        }
    }

    private fun checkSize(frameLength: Long, assembled: Int) {
        if (frameLength > options.maxMessageBytes.toLong() - assembled) {
            throw Violation(MESSAGE_TOO_BIG, "message larger than ${options.maxMessageBytes} bytes")
        }
    }

    private fun readByte(): Int {
        val value = input.read()
        if (value < 0) throw EOFException("WebSocket connection closed by the server")
        return value
    }

    private fun readBytes(count: Int): ByteArray {
        val bytes = ByteArray(count)
        var read = 0
        while (read < count) {
            val n = input.read(bytes, read, count - read)
            if (n < 0) throw EOFException("WebSocket connection closed by the server")
            read += n
        }
        return bytes
    }

    companion object {
        const val NORMAL_CLOSURE = 1000
        const val GOING_AWAY = 1001
        const val PROTOCOL_ERROR = 1002
        const val UNSUPPORTED_DATA = 1003
        const val NO_STATUS = 1005
        const val ABNORMAL = 1006
        const val INVALID_PAYLOAD = 1007
        const val POLICY_VIOLATION = 1008
        const val MESSAGE_TOO_BIG = 1009
        const val INTERNAL_ERROR = 1011

        private const val OP_CONTINUATION = 0x0
        private const val OP_TEXT = 0x1
        private const val OP_BINARY = 0x2
        private const val OP_CLOSE = 0x8
        private const val OP_PING = 0x9
        private const val OP_PONG = 0xA

        private const val STATE_OPEN = 0
        private const val STATE_CLOSING = 1
        private const val STATE_CLOSED = 2

        private const val MAX_CONTROL_PAYLOAD = 125
        private const val MAX_HEAD_BYTES = 64 * 1024
        private const val ACCEPT_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
        private val RESERVED_HEADERS = setOf(
            "host", "upgrade", "connection", "sec-websocket-key", "sec-websocket-version", "sec-websocket-protocol",
        )
        private val RANDOM = SecureRandom()

        fun connect(url: String, listener: Listener, options: Options = Options()): WebSocketClient {
            val target = WebSocketTarget.parse(url)
            for ((name, value) in options.headers) {
                if (name.isEmpty() || name.any { it == '\r' || it == '\n' || it == ':' } || value.any { it == '\r' || it == '\n' }) {
                    throw WebSocketException("invalid handshake header \"$name\"")
                }
            }
            val socket = openSocket(target, options)
            try {
                socket.soTimeout = options.handshakeTimeoutMs
                socket.tcpNoDelay = true
                val input = BufferedInputStream(socket.getInputStream(), 16 * 1024)
                val output = socket.getOutputStream()
                val key = Base64.getEncoder().encodeToString(ByteArray(16).also { RANDOM.nextBytes(it) })
                val request = StringBuilder()
                    .append("GET ").append(target.resource).append(" HTTP/1.1\r\n")
                    .append("Host: ").append(target.hostHeader).append("\r\n")
                    .append("Upgrade: websocket\r\n")
                    .append("Connection: Upgrade\r\n")
                    .append("Sec-WebSocket-Key: ").append(key).append("\r\n")
                    .append("Sec-WebSocket-Version: 13\r\n")
                if (options.protocols.isNotEmpty()) {
                    request.append("Sec-WebSocket-Protocol: ").append(options.protocols.joinToString(", ")).append("\r\n")
                }
                for ((name, value) in options.headers) {
                    if (name.lowercase() in RESERVED_HEADERS) continue
                    request.append(name).append(": ").append(value).append("\r\n")
                }
                request.append("\r\n")
                output.write(request.toString().toByteArray(Charsets.UTF_8))
                output.flush()

                val lines = readHead(input).split("\r\n")
                val statusLine = lines.first()
                val parts = statusLine.split(' ', limit = 3)
                val status = parts.getOrNull(1)?.toIntOrNull()
                if (!statusLine.startsWith("HTTP/") || status == null) {
                    throw WebSocketException("malformed WebSocket handshake response: $statusLine")
                }
                val headers = LinkedHashMap<String, MutableList<String>>()
                for (line in lines.drop(1)) {
                    val colon = line.indexOf(':')
                    if (colon <= 0) continue
                    val name = line.substring(0, colon).trim()
                    val existing = headers.keys.firstOrNull { it.equals(name, ignoreCase = true) } ?: name
                    headers.getOrPut(existing) { mutableListOf() }.add(line.substring(colon + 1).trim())
                }
                fun header(name: String): String? =
                    headers.entries.firstOrNull { it.key.equals(name, ignoreCase = true) }?.value?.firstOrNull()

                if (status != 101) {
                    val text = parts.getOrNull(2)?.let { " $it" } ?: ""
                    throw WebSocketException("WebSocket handshake failed: HTTP $status$text", status)
                }
                if (header("Upgrade")?.equals("websocket", ignoreCase = true) != true) {
                    throw WebSocketException("WebSocket handshake response is missing Upgrade: websocket")
                }
                val connectionTokens = headers.entries.filter { it.key.equals("Connection", ignoreCase = true) }
                    .flatMap { it.value }.flatMap { it.split(',') }.map { it.trim() }
                if (connectionTokens.none { it.equals("upgrade", ignoreCase = true) }) {
                    throw WebSocketException("WebSocket handshake response is missing Connection: Upgrade")
                }
                if (header("Sec-WebSocket-Accept") != acceptFor(key)) {
                    throw WebSocketException("WebSocket handshake response has a wrong Sec-WebSocket-Accept")
                }
                val selected = header("Sec-WebSocket-Protocol").orEmpty()
                if (selected.isNotEmpty() && selected !in options.protocols) {
                    throw WebSocketException("server selected a subprotocol that was not offered: $selected")
                }
                socket.soTimeout = options.readTimeoutMs
                val client = WebSocketClient(url, socket, input, output, listener, options, selected, headers)
                client.start()
                return client
            } catch (failure: Throwable) {
                closeQuietly(socket)
                throw failure
            }
        }

        private fun openSocket(target: WebSocketTarget, options: Options): Socket {
            val plain = Socket()
            try {
                plain.connect(InetSocketAddress(target.host, target.port), options.connectTimeoutMs)
                if (!target.secure) return plain
                val factory = options.sslSocketFactory ?: (SSLSocketFactory.getDefault() as SSLSocketFactory)
                val ssl = factory.createSocket(plain, target.host, target.port, true) as SSLSocket
                try {
                    val params = ssl.sslParameters
                    if (!target.isIpLiteral) {
                        try {
                            params.serverNames = listOf(SNIHostName(target.host))
                        } catch (_: IllegalArgumentException) {
                        }
                    }
                    params.endpointIdentificationAlgorithm = "HTTPS"
                    ssl.sslParameters = params
                    ssl.soTimeout = options.handshakeTimeoutMs
                    ssl.startHandshake()
                    val verifier = options.hostnameVerifier
                        ?: if (isAndroidRuntime()) HttpsURLConnection.getDefaultHostnameVerifier() else null
                    if (verifier != null && !verifier.verify(target.host, ssl.session)) {
                        throw SSLPeerUnverifiedException("hostname ${target.host} does not match the server certificate")
                    }
                    return ssl
                } catch (failure: Throwable) {
                    closeQuietly(ssl)
                    throw failure
                }
            } catch (failure: Throwable) {
                closeQuietly(plain)
                throw failure
            }
        }

        private fun readHead(input: InputStream): String {
            val out = ByteArrayOutputStream()
            var matched = 0
            while (matched < 4) {
                val b = input.read()
                if (b < 0) throw EOFException("connection closed during the WebSocket handshake")
                out.write(b)
                matched = when {
                    b == '\r'.code && (matched == 0 || matched == 2) -> matched + 1
                    b == '\n'.code && (matched == 1 || matched == 3) -> matched + 1
                    b == '\r'.code -> 1
                    else -> 0
                }
                if (out.size() > MAX_HEAD_BYTES) throw WebSocketException("WebSocket handshake response head too large")
            }
            return String(out.toByteArray(), Charsets.ISO_8859_1).removeSuffix("\r\n\r\n")
        }

        internal fun acceptFor(key: String): String =
            Base64.getEncoder().encodeToString(
                MessageDigest.getInstance("SHA-1").digest((key + ACCEPT_GUID).toByteArray(Charsets.ISO_8859_1)),
            )

        private fun isValidCloseCode(code: Int): Boolean =
            code in 1000..1003 || code in 1007..1014 || code in 3000..4999

        private fun isAndroidRuntime(): Boolean = System.getProperty("java.vm.name") == "Dalvik"

        private fun decodeUtf8(bytes: ByteArray, offset: Int, length: Int): String? = try {
            Charsets.UTF_8.newDecoder()
                .onMalformedInput(CodingErrorAction.REPORT)
                .onUnmappableCharacter(CodingErrorAction.REPORT)
                .decode(ByteBuffer.wrap(bytes, offset, length))
                .toString()
        } catch (_: CharacterCodingException) {
            null
        }

        private fun truncateUtf8(text: String, maxBytes: Int): ByteArray {
            val bytes = text.toByteArray(Charsets.UTF_8)
            if (bytes.size <= maxBytes) return bytes
            var end = maxBytes
            while (end > 0 && (bytes[end].toInt() and 0xC0) == 0x80) end--
            return bytes.copyOf(end)
        }

        private fun closeQuietly(closeable: Closeable) {
            try {
                closeable.close()
            } catch (_: IOException) {
            }
        }
    }
}

internal class WebSocketTarget(val secure: Boolean, val host: String, val port: Int, val resource: String) {
    val isIpLiteral: Boolean get() = host.contains(':') || IPV4.matches(host)

    val hostHeader: String
        get() {
            val name = if (host.contains(':')) "[$host]" else host
            return if (port == (if (secure) 443 else 80)) name else "$name:$port"
        }

    companion object {
        private val IPV4 = Regex("^\\d{1,3}(\\.\\d{1,3}){3}$")

        fun parse(url: String): WebSocketTarget {
            val uri = try {
                URI(url)
            } catch (error: URISyntaxException) {
                throw WebSocketException("invalid WebSocket URL: $url", 0, error)
            }
            val secure = when (uri.scheme?.lowercase()) {
                "ws", "http" -> false
                "wss", "https" -> true
                else -> throw WebSocketException("unsupported WebSocket URL scheme: $url")
            }
            var host: String? = uri.host
            var port = uri.port
            if (host == null) {
                val authority = uri.rawAuthority?.substringAfterLast('@').orEmpty()
                if (authority.startsWith("[")) {
                    host = authority.substringBefore(']') + "]"
                    port = authority.substringAfter("]:", "").toIntOrNull() ?: -1
                } else {
                    val colon = authority.lastIndexOf(':')
                    if (colon >= 0) {
                        host = authority.substring(0, colon)
                        port = authority.substring(colon + 1).toIntOrNull() ?: -1
                    } else {
                        host = authority
                    }
                }
            }
            val cleanHost = host.removePrefix("[").removeSuffix("]")
            if (cleanHost.isEmpty()) throw WebSocketException("WebSocket URL has no host: $url")
            if (port == -1) port = if (secure) 443 else 80
            val path = uri.rawPath.orEmpty().ifEmpty { "/" }
            val resource = if (uri.rawQuery != null) "$path?${uri.rawQuery}" else path
            return WebSocketTarget(secure, cleanHost, port, resource)
        }
    }
}
