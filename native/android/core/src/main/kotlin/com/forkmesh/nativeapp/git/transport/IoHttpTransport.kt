package com.forkmesh.nativeapp.git.transport

import com.forkmesh.nativeapp.git.bytes.GitBytesBuilder
import com.forkmesh.nativeapp.git.bytes.encodeLowerHex
import com.forkmesh.nativeapp.git.crypto.ForkMeshSha1
import com.forkmesh.nativeapp.git.errors.GitErrorCode
import com.forkmesh.nativeapp.git.errors.GitException
import com.forkmesh.nativeapp.git.security.GitCancellationToken
import com.forkmesh.nativeapp.git.security.GitResourceLimits
import java.io.ByteArrayInputStream
import java.io.IOException
import java.net.HttpURLConnection
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.ProtocolException
import java.net.Socket
import java.net.SocketException
import java.net.SocketTimeoutException
import java.net.UnknownHostException
import java.security.KeyStore
import java.security.cert.CertificateException
import java.security.cert.CertificateFactory
import java.security.cert.X509Certificate
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.CopyOnWriteArrayList
import javax.net.ssl.HostnameVerifier
import javax.net.ssl.HttpsURLConnection
import javax.net.ssl.SSLContext
import javax.net.ssl.SSLEngine
import javax.net.ssl.SSLException
import javax.net.ssl.SSLHandshakeException
import javax.net.ssl.SSLSocket
import javax.net.ssl.SSLSocketFactory
import javax.net.ssl.TrustManagerFactory
import javax.net.ssl.X509ExtendedTrustManager
import javax.net.ssl.X509TrustManager

class GitIoHttpTransport(
    val headerProvider: GitHttpHeaderProvider? = null,
    val certificateChallenge: GitCertificateChallengeHandler? = null,
    trustedRootCertificatesPem: ByteArray? = null,
    userAgent: String? = null,
    val surfaceRedirects: Boolean = false,
) : GitHttpTransport {
    private val userAgent: String = userAgent ?: "forkmesh-git/0.0.0"

    @Volatile
    private var closed = false
    private val socketFactory: SSLSocketFactory
    private val hostnameVerifier: HostnameVerifier

    init {
        val primary = trustManagerFor(null)
        val extra = trustedRootCertificatesPem?.let { trustManagerFor(keyStoreFromPem(it)) }
        val context = SSLContext.getInstance("TLS")
        context.init(null, arrayOf(ChallengeTrustManager(primary, extra)), null)
        socketFactory = context.socketFactory
        val platformVerifier = HttpsURLConnection.getDefaultHostnameVerifier()
        hostnameVerifier = HostnameVerifier { host, session ->
            val verified = try {
                platformVerifier.verify(host, session)
            } catch (error: Exception) {
                false
            }
            verified || try {
                onBadCertificate(session.peerCertificates.firstOrNull() as? X509Certificate, host, session.peerPort)
            } catch (error: Exception) {
                false
            }
        }
    }

    private fun onBadCertificate(certificate: X509Certificate?, host: String?, port: Int): Boolean {
        val handler = certificateChallenge ?: return false
        if (certificate == null) return false
        val der = try {
            certificate.encoded
        } catch (error: Exception) {
            return false
        }
        if (der.size > maxCertificateBytes) return false
        return try {
            val digest = ForkMeshSha1()
            digest.add(der)
            val decision = handler.decide(
                GitCertificateChallenge(
                    host = host ?: "",
                    port = port,
                    certificateDer = der,
                    subject = certificate.subjectX500Principal.name,
                    issuer = certificate.issuerX500Principal.name,
                    sha1Fingerprint = encodeLowerHex(digest.close()),
                ),
            )
            decision == GitCertificateDecision.accept
        } catch (error: Throwable) {
            false
        }
    }

    override fun send(
        request: GitHttpRequest,
        cancellation: GitCancellationToken?,
        limits: GitResourceLimits?,
    ): GitHttpResponse {
        if (closed) {
            throw GitException(GitErrorCode.networkUnavailable, "transport is closed")
        }
        val effectiveLimits = limits ?: GitResourceLimits()
        cancellation?.throwIfCancelled()
        checkDeadline(request.deadline)
        val connectTimeoutMs = effectiveLimits.connectTimeout.inWholeMilliseconds
        val idleReadTimeoutMs = effectiveLimits.idleReadTimeout.inWholeMilliseconds
        val extraHeaders = providedHeaders(request)
        val connection = try {
            request.url.toURL().openConnection() as HttpURLConnection
        } catch (error: IOException) {
            throw GitException(GitErrorCode.networkUnavailable, "connection failed: ${redactOsError(error.message)}")
        } catch (error: IllegalArgumentException) {
            throw GitException(GitErrorCode.protocolUnsupported, "transport accepts only credential-free HTTPS URLs")
        }
        if (connection !is HttpsURLConnection) {
            throw GitException(GitErrorCode.protocolUnsupported, "transport accepts only credential-free HTTPS URLs")
        }
        val exchange = TransportExchange(cancellation, request.deadline)
        connection.sslSocketFactory = ExchangeSocketFactory(socketFactory, exchange)
        connection.hostnameVerifier = hostnameVerifier
        connection.instanceFollowRedirects = false
        connection.useCaches = false
        connection.allowUserInteraction = false
        connection.connectTimeout = connectTimeoutMs.toInt()
        connection.readTimeout = (connectTimeoutMs + idleReadTimeoutMs).toInt()
        connection.requestMethod = request.method
        for ((name, value) in extraHeaders) connection.setRequestProperty(name, value)
        for ((name, value) in request.headers) connection.setRequestProperty(name, value)
        connection.setRequestProperty("accept-encoding", "identity")
        connection.setRequestProperty("user-agent", userAgent)

        TransportWatchdog.register(exchange)
        var completed = false
        try {
            val status = try {
                exchange.arm(connectTimeoutMs, connectTimedOut())
                if (request.method == "POST") {
                    connection.doOutput = true
                    connection.setFixedLengthStreamingMode(request.body.size)
                    cancellation?.throwIfCancelled()
                    val output = connection.outputStream
                    var offset = 0
                    while (offset < request.body.size) {
                        exchange.arm(idleReadTimeoutMs, stalled())
                        val chunk = minOf(chunkBytes, request.body.size - offset)
                        output.write(request.body, offset, chunk)
                        offset += chunk
                        exchange.throwIfAborted()
                        cancellation?.throwIfCancelled()
                    }
                    output.close()
                } else {
                    connection.connect()
                }
                exchange.arm(remaining(request.deadline, connectTimeoutMs), connectTimedOut())
                val code = connection.responseCode
                exchange.throwIfAborted()
                code
            } catch (error: IOException) {
                throw exchange.abortReason ?: sendFailure(error)
            }
            exchange.disarm()
            val headers = boundedHeaders(connection, effectiveLimits)
            if (status in 300 until 400) {
                if (surfaceRedirects) return GitHttpResponse(statusCode = status, headers = headers, body = ByteArray(0))
                throw classifyHttpFailure(request, status, location = headers["location"])
            }
            if (status != 200) {
                throw classifyHttpFailure(request, status)
            }
            val declaredLength = connection.contentLengthLong
            if (declaredLength > request.maxResponseBytes) {
                throw GitException(GitErrorCode.resourceLimitExceeded, "response body exceeds the configured limit")
            }
            val contentType = mediaType(headers["content-type"])
            if (contentType != request.expectedContentType.lowercase()) {
                throw GitException(
                    GitErrorCode.protocolUnsupported,
                    "response content type is not the expected Git service type",
                )
            }
            val body = readBody(connection, exchange, request, idleReadTimeoutMs, declaredLength, cancellation)
            completed = true
            return GitHttpResponse(statusCode = status, headers = headers, body = body)
        } finally {
            TransportWatchdog.unregister(exchange)
            if (!completed) {
                exchange.closeSockets()
                try {
                    connection.disconnect()
                } catch (error: Exception) {
                }
            }
        }
    }

    private fun readBody(
        connection: HttpURLConnection,
        exchange: TransportExchange,
        request: GitHttpRequest,
        idleReadTimeoutMs: Long,
        declaredLength: Long,
        cancellation: GitCancellationToken?,
    ): ByteArray {
        val initial = if (declaredLength in 0..request.maxResponseBytes.toLong()) {
            minOf(declaredLength, 1L shl 24).toInt()
        } else {
            64 * 1024
        }
        val output = GitBytesBuilder(initial)
        val buffer = ByteArray(chunkBytes)
        try {
            exchange.arm(idleReadTimeoutMs, stalled())
            val input = connection.inputStream
            input.use { stream ->
                while (true) {
                    exchange.arm(idleReadTimeoutMs, stalled())
                    val read = stream.read(buffer)
                    exchange.throwIfAborted()
                    if (read == -1) break
                    if (output.length.toLong() + read > request.maxResponseBytes) {
                        throw GitException(
                            GitErrorCode.resourceLimitExceeded,
                            "response body exceeds the configured limit",
                        )
                    }
                    output.add(buffer, 0, read)
                    if (cancellation?.isCancelled == true) {
                        throw GitException(GitErrorCode.cancelled, "operation was cancelled")
                    }
                    val deadline = request.deadline
                    if (deadline != null && System.currentTimeMillis() > deadline) {
                        throw GitException(
                            GitErrorCode.networkUnavailable,
                            "operation deadline exceeded while streaming",
                        )
                    }
                }
            }
        } catch (error: IOException) {
            throw exchange.abortReason ?: if (error is SocketTimeoutException) {
                stalled()
            } else {
                GitException(GitErrorCode.networkUnavailable, "response stream failed: ${redactOsError(error.toString())}")
            }
        } finally {
            exchange.disarm()
        }
        return output.takeBytes()
    }

    private fun providedHeaders(request: GitHttpRequest): Map<String, String> {
        val provider = headerProvider ?: return emptyMap()
        val provided = try {
            provider.headers(request.method, request.url, request.body)
        } catch (error: GitException) {
            throw error
        } catch (error: Throwable) {
            throw GitException(GitErrorCode.authenticationFailed, "header provider failed")
        }
        val result = LinkedHashMap<String, String>()
        for ((key, value) in provided) {
            val name = key.lowercase()
            if (!headerName.matches(name) || !headerValue.matches(value) || name in reservedHeaders) {
                throw GitException(
                    GitErrorCode.authenticationFailed,
                    "header provider returned an invalid or reserved header",
                )
            }
            result[name] = value
        }
        return result
    }

    override fun close() {
        closed = true
    }

    private inner class ChallengeTrustManager(
        private val primary: X509TrustManager,
        private val extra: X509TrustManager?,
    ) : X509ExtendedTrustManager() {
        override fun getAcceptedIssuers(): Array<X509Certificate> =
            primary.acceptedIssuers + (extra?.acceptedIssuers ?: emptyArray())

        override fun checkClientTrusted(chain: Array<out X509Certificate>?, authType: String?) {
            throw CertificateException("client certificates are not accepted")
        }

        override fun checkClientTrusted(chain: Array<out X509Certificate>?, authType: String?, socket: Socket?) {
            throw CertificateException("client certificates are not accepted")
        }

        override fun checkClientTrusted(chain: Array<out X509Certificate>?, authType: String?, engine: SSLEngine?) {
            throw CertificateException("client certificates are not accepted")
        }

        override fun checkServerTrusted(chain: Array<out X509Certificate>?, authType: String?) {
            verify(chain, null, -1) { manager -> manager.checkServerTrusted(chain, authType) }
        }

        override fun checkServerTrusted(chain: Array<out X509Certificate>?, authType: String?, socket: Socket?) {
            val host = (socket as? SSLSocket)?.handshakeSession?.peerHost
                ?: (socket?.remoteSocketAddress as? InetSocketAddress)?.hostString
            verify(chain, host, socket?.port ?: -1) { manager ->
                if (manager is X509ExtendedTrustManager) {
                    manager.checkServerTrusted(chain, authType, socket)
                } else {
                    manager.checkServerTrusted(chain, authType)
                }
            }
        }

        override fun checkServerTrusted(chain: Array<out X509Certificate>?, authType: String?, engine: SSLEngine?) {
            verify(chain, engine?.peerHost, engine?.peerPort ?: -1) { manager ->
                if (manager is X509ExtendedTrustManager) {
                    manager.checkServerTrusted(chain, authType, engine)
                } else {
                    manager.checkServerTrusted(chain, authType)
                }
            }
        }

        private fun verify(
            chain: Array<out X509Certificate>?,
            host: String?,
            port: Int,
            check: (X509TrustManager) -> Unit,
        ) {
            val failure = try {
                check(primary)
                return
            } catch (error: CertificateException) {
                error
            }
            if (extra != null) {
                try {
                    check(extra)
                    return
                } catch (error: CertificateException) {
                }
            }
            if (!chain.isNullOrEmpty() && onBadCertificate(chain[0], host, port)) return
            throw failure
        }
    }

    private companion object {
        const val maxCertificateBytes = 64 * 1024
        const val chunkBytes = 64 * 1024

        val headerName = Regex("[a-z0-9!#${'$'}%&*+.^_`|~-]+")
        val headerValue = Regex("[\\x20-\\x7e]*")
        val reservedHeaders = setOf(
            "content-type",
            "accept",
            "git-protocol",
            "content-length",
            "transfer-encoding",
            "host",
            "cookie",
            "accept-encoding",
            "connection",
            "upgrade",
        )

        fun connectTimedOut() =
            GitException(GitErrorCode.networkUnavailable, "remote did not respond within the connect timeout")

        fun stalled() = GitException(GitErrorCode.networkUnavailable, "remote stalled beyond the idle read timeout")

        fun sendFailure(error: IOException): GitException = when {
            error is SSLHandshakeException -> GitException(
                GitErrorCode.tlsRejected,
                "TLS handshake was rejected: ${redactOsError(error.message)}",
            )
            error is SSLException -> GitException(GitErrorCode.tlsRejected, "TLS failure: ${redactOsError(error.message)}")
            error.message?.startsWith("HTTPS hostname wrong") == true -> GitException(
                GitErrorCode.tlsRejected,
                "TLS handshake was rejected: ${redactOsError(error.message)}",
            )
            error is SocketTimeoutException -> connectTimedOut()
            error is SocketException || error is UnknownHostException -> GitException(
                GitErrorCode.networkUnavailable,
                "connection failed: ${redactOsError(error.message)}",
            )
            error is ProtocolException -> GitException(
                GitErrorCode.networkUnavailable,
                "HTTP failure: ${redactOsError(error.message)}",
            )
            else -> GitException(GitErrorCode.networkUnavailable, "I/O failure while sending the request")
        }

        fun boundedHeaders(connection: HttpURLConnection, limits: GitResourceLimits): Map<String, String> {
            val headers = LinkedHashMap<String, String>()
            var total = 0L
            for ((name, values) in connection.headerFields ?: emptyMap()) {
                if (name == null) continue
                for (value in values) {
                    total += name.length + value.length + 4
                }
                headers[name.lowercase()] = values.joinToString(", ")
            }
            if (total > limits.maxResponseHeaderBytes) {
                throw GitException(GitErrorCode.resourceLimitExceeded, "response headers exceed the configured limit")
            }
            return headers
        }

        fun mediaType(contentType: String?): String {
            if (contentType == null) return ""
            val separator = contentType.indexOf(';')
            val type = if (separator == -1) contentType else contentType.substring(0, separator)
            return type.trim().lowercase()
        }

        fun checkDeadline(deadline: Long?) {
            if (deadline != null && System.currentTimeMillis() > deadline) {
                throw GitException(GitErrorCode.networkUnavailable, "operation deadline exceeded")
            }
        }

        fun remaining(deadline: Long?, fallbackMs: Long): Long {
            if (deadline == null) return fallbackMs
            val remaining = deadline - System.currentTimeMillis()
            if (remaining <= 0) {
                throw GitException(GitErrorCode.networkUnavailable, "operation deadline exceeded")
            }
            return if (remaining < fallbackMs) remaining else fallbackMs
        }

        fun redactOsError(message: String?): String {
            val buffer = StringBuilder()
            for (char in message ?: "") {
                if (buffer.length >= 160) break
                if (char.code in 0x20 until 0x7f) buffer.append(char)
            }
            return buffer.toString()
        }

        fun trustManagerFor(keyStore: KeyStore?): X509TrustManager {
            val factory = TrustManagerFactory.getInstance(TrustManagerFactory.getDefaultAlgorithm())
            factory.init(keyStore)
            return factory.trustManagers.filterIsInstance<X509TrustManager>().firstOrNull()
                ?: throw GitException(GitErrorCode.tlsRejected, "platform trust manager is unavailable")
        }

        fun keyStoreFromPem(pem: ByteArray): KeyStore {
            try {
                val certificates = CertificateFactory.getInstance("X.509")
                    .generateCertificates(ByteArrayInputStream(pem))
                if (certificates.isEmpty()) {
                    throw GitException(GitErrorCode.tlsRejected, "trusted root certificates are invalid")
                }
                val store = KeyStore.getInstance(KeyStore.getDefaultType())
                store.load(null, null)
                for ((index, certificate) in certificates.withIndex()) {
                    store.setCertificateEntry("forkmesh-trusted-$index", certificate)
                }
                return store
            } catch (error: GitException) {
                throw error
            } catch (error: Exception) {
                throw GitException(GitErrorCode.tlsRejected, "trusted root certificates are invalid")
            }
        }
    }
}

internal class TransportExchange(
    private val cancellation: GitCancellationToken?,
    private val deadline: Long?,
) {
    private val sockets = CopyOnWriteArrayList<Socket>()

    @Volatile
    var abortReason: GitException? = null
        private set

    @Volatile
    private var armed = false

    @Volatile
    private var phaseStartNanos = 0L

    @Volatile
    private var phaseTimeoutNanos = 0L

    @Volatile
    private var phaseError: GitException? = null

    fun <T : Socket> track(socket: T): T {
        sockets.add(socket)
        if (abortReason != null) closeQuietly(socket)
        return socket
    }

    fun arm(timeoutMs: Long, error: GitException) {
        phaseError = error
        phaseTimeoutNanos = timeoutMs * 1_000_000
        phaseStartNanos = System.nanoTime()
        armed = true
    }

    fun disarm() {
        armed = false
    }

    fun throwIfAborted() {
        abortReason?.let { throw it }
    }

    fun poll() {
        if (abortReason == null) {
            val reason = when {
                cancellation?.isCancelled == true -> GitException(GitErrorCode.cancelled, "operation was cancelled")
                deadline != null && System.currentTimeMillis() > deadline -> GitException(
                    GitErrorCode.networkUnavailable,
                    "operation deadline exceeded while streaming",
                )
                armed && System.nanoTime() - phaseStartNanos > phaseTimeoutNanos -> phaseError
                else -> null
            } ?: return
            abortReason = reason
        }
        closeSockets()
    }

    fun closeSockets() {
        for (socket in sockets) closeQuietly(socket)
    }

    private fun closeQuietly(socket: Socket) {
        try {
            socket.close()
        } catch (error: Exception) {
        }
    }
}

private class ExchangeSocketFactory(
    private val delegate: SSLSocketFactory,
    private val exchange: TransportExchange,
) : SSLSocketFactory() {
    override fun getDefaultCipherSuites(): Array<String> = delegate.defaultCipherSuites

    override fun getSupportedCipherSuites(): Array<String> = delegate.supportedCipherSuites

    override fun createSocket(): Socket = exchange.track(delegate.createSocket())

    override fun createSocket(socket: Socket, host: String, port: Int, autoClose: Boolean): Socket {
        exchange.track(socket)
        return exchange.track(delegate.createSocket(socket, host, port, autoClose))
    }

    override fun createSocket(host: String, port: Int): Socket = exchange.track(delegate.createSocket(host, port))

    override fun createSocket(host: String, port: Int, localHost: InetAddress, localPort: Int): Socket =
        exchange.track(delegate.createSocket(host, port, localHost, localPort))

    override fun createSocket(host: InetAddress, port: Int): Socket = exchange.track(delegate.createSocket(host, port))

    override fun createSocket(address: InetAddress, port: Int, localAddress: InetAddress, localPort: Int): Socket =
        exchange.track(delegate.createSocket(address, port, localAddress, localPort))
}

internal object TransportWatchdog {
    private const val intervalMs = 50L
    private val exchanges: MutableSet<TransportExchange> = ConcurrentHashMap.newKeySet()
    private val lock = Any()
    private var thread: Thread? = null

    fun register(exchange: TransportExchange) {
        exchanges.add(exchange)
        synchronized(lock) {
            if (thread == null) {
                thread = Thread(::run, "forkmesh-git-http-watchdog").also {
                    it.isDaemon = true
                    it.start()
                }
            }
        }
    }

    fun unregister(exchange: TransportExchange) {
        exchanges.remove(exchange)
    }

    private fun run() {
        while (true) {
            try {
                Thread.sleep(intervalMs)
            } catch (error: InterruptedException) {
            }
            for (exchange in exchanges) exchange.poll()
            synchronized(lock) {
                if (exchanges.isEmpty()) {
                    thread = null
                    return
                }
            }
        }
    }
}
