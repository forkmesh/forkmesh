package com.forkmesh.nativeapp.nexus

import java.net.HttpURLConnection
import java.net.InetSocketAddress
import java.net.URI
import java.net.URL
import java.security.MessageDigest
import java.security.cert.X509Certificate
import java.security.interfaces.ECPublicKey
import java.security.interfaces.RSAPublicKey
import javax.net.ssl.SNIHostName
import javax.net.ssl.SSLException
import javax.net.ssl.SSLSocket
import javax.net.ssl.SSLSocketFactory

fun interface TlsProbing {
    fun inspectTls(host: String, port: Int, serverName: String?, timeoutMs: Int): TlsObservation
}

object SystemTlsProbe : TlsProbing {
    override fun inspectTls(host: String, port: Int, serverName: String?, timeoutMs: Int): TlsObservation {
        val factory = SSLSocketFactory.getDefault() as SSLSocketFactory
        val plain = java.net.Socket()
        try {
            NexusFailures.guard { plain.connect(InetSocketAddress(host, port), timeoutMs) }
            plain.soTimeout = timeoutMs
            val socket = factory.createSocket(plain, serverName ?: host, port, true) as SSLSocket
            socket.sslParameters = socket.sslParameters.apply {
                endpointIdentificationAlgorithm = "HTTPS"
                if (serverName != null && IPAddr.parse(serverName) == null) serverNames = listOf(SNIHostName(serverName))
            }
            val started = System.nanoTime()
            return try {
                socket.startHandshake()
                val session = socket.session
                val chain = session.peerCertificates.mapNotNull { it as? X509Certificate }.map(Certificates::summary)
                TlsObservation(host, port, serverName, session.protocol, session.cipherSuite, null, chain, TrustOutcome.Trusted, elapsedMs(started))
            } catch (rejected: SSLException) {
                TlsObservation(host, port, serverName, chain = emptyList(), trust = TrustOutcome.Untrusted(Certificates.reason(rejected)), handshakeMilliseconds = elapsedMs(started))
            } finally {
                runCatching { socket.close() }
            }
        } finally {
            runCatching { plain.close() }
        }
    }
}

object Certificates {
    fun summary(certificate: X509Certificate): CertificateSummary {
        val subject = certificate.subjectX500Principal.name
        val issuer = certificate.issuerX500Principal.name
        val key = certificate.publicKey
        val bits = when (key) {
            is RSAPublicKey -> key.modulus.bitLength()
            is ECPublicKey -> key.params.order.bitLength()
            else -> null
        }
        val names = runCatching { certificate.subjectAlternativeNames }.getOrNull().orEmpty()
            .filter { (it.getOrNull(0) as? Int) == 2 || (it.getOrNull(0) as? Int) == 7 }
            .mapNotNull { it.getOrNull(1) as? String }
        return CertificateSummary(
            subject = subject,
            subjectCommonName = commonName(subject),
            issuer = issuer,
            issuerCommonName = commonName(issuer),
            serialNumber = colonHex(certificate.serialNumber.toByteArray().dropWhile { it == 0.toByte() }.ifEmpty { listOf(0.toByte()) }.toByteArray()),
            version = certificate.version,
            notBefore = certificate.notBefore.time,
            notAfter = certificate.notAfter.time,
            subjectAlternativeNames = names,
            publicKeyAlgorithm = key.algorithm,
            publicKeyBits = bits,
            signatureAlgorithm = certificate.sigAlgName,
            isCertificateAuthority = certificate.basicConstraints >= 0,
            isSelfIssued = subject == issuer,
            sha256Fingerprint = colonHex(MessageDigest.getInstance("SHA-256").digest(certificate.encoded)),
        )
    }

    fun commonName(distinguishedName: String): String? =
        Regex("""(?:^|,)\s*CN=((?:\\.|[^,])*)""").find(distinguishedName)?.groupValues?.get(1)?.replace("\\", "")?.takeIf { it.isNotBlank() }

    fun colonHex(bytes: ByteArray): String = bytes.joinToString(":") { "%02X".format(it.toInt() and 0xFF) }

    fun reason(error: Throwable): String {
        var cause: Throwable? = error
        var message = error.message.orEmpty()
        while (cause != null) {
            val text = cause.message.orEmpty()
            when {
                "expired" in text.lowercase() -> return "the certificate has expired"
                "not yet valid" in text.lowercase() -> return "the certificate is not valid yet"
                "trust anchor" in text.lowercase() || "certification path" in text.lowercase() -> return "the certificate chain does not lead to a trusted authority"
                "hostname" in text.lowercase() || "subject alternative" in text.lowercase() || "no name matching" in text.lowercase() -> return "the certificate does not cover this host name"
            }
            if (text.isNotEmpty()) message = text
            cause = cause.cause
        }
        return message.ifEmpty { "the handshake failed" }
    }
}

class HttpInspector(private val tls: TlsProbing = SystemTlsProbe) {
    fun inspect(urlText: String, timeoutMs: Int = 15_000): HttpInspection {
        val url = runCatching { URI(urlText.trim()).toURL() }.getOrNull()
        val scheme = url?.protocol?.lowercase()
        if (url == null || (scheme != "http" && scheme != "https") || url.host.isNullOrEmpty()) throw ConnectionFailure.Unsupported("Use an http or https URL.")
        val started = System.nanoTime()
        var current: URL = url
        val redirects = mutableListOf<String>()
        var connectMs: Double? = null
        var firstByteMs: Double? = null
        while (true) {
            val hop = System.nanoTime()
            val response = try {
                request(current, "HEAD", timeoutMs).let { if (it.status == 405 || it.status == 501) request(current, "GET", timeoutMs) else it }
            } catch (error: Exception) {
                val failure = NexusFailures.map(error)
                if (failure is ConnectionFailure.Tls && current.protocol == "https") {
                    val observed = runCatching { tls.inspectTls(current.host, port(current), current.host, timeoutMs) }.getOrNull()
                    throw ConnectionFailure.Tls(observed?.trust?.summary ?: failure.detail.removePrefix("The TLS handshake failed: "))
                }
                if ("cleartext" in error.message.orEmpty().lowercase()) throw ConnectionFailure.Unsupported("Android does not let this app read plain HTTP from ${current.host}. Inspect the https:// address instead.")
                throw failure
            }
            if (connectMs == null) connectMs = response.connectMs
            if (firstByteMs == null) firstByteMs = elapsedMs(hop)
            val location = response.headers.firstOrNull { it.name.equals("location", true) }?.value
            if (response.status in 300..399 && location != null && redirects.size < 10) {
                val next = runCatching { URL(current, location) }.getOrNull()
                if (next != null && (next.protocol == "http" || next.protocol == "https")) {
                    redirects += next.toString()
                    current = next
                    continue
                }
            }
            val total = elapsedMs(started)
            val observation = if (current.protocol == "https") runCatching { tls.inspectTls(current.host, port(current), current.host, timeoutMs) }.getOrNull() else null
            val findings = SecurityInsights.http(current.toString(), response.status, response.headers) + (observation?.let(SecurityInsights::tls) ?: emptyList())
            return HttpInspection(
                requestedUrl = url.toString(), finalUrl = current.toString(), statusCode = response.status, reason = response.reason,
                headers = response.headers, redirects = redirects, elapsedMilliseconds = total,
                timings = HttpTimings(connectMs, firstByteMs, total), securityObservations = findings, tls = observation,
            )
        }
    }

    private class Response(val status: Int, val reason: String, val headers: List<HttpHeader>, val connectMs: Double)

    private fun request(url: URL, method: String, timeoutMs: Int): Response {
        val connection = url.openConnection() as HttpURLConnection
        try {
            connection.instanceFollowRedirects = false
            connection.requestMethod = method
            connection.connectTimeout = timeoutMs
            connection.readTimeout = timeoutMs
            connection.useCaches = false
            connection.setRequestProperty("User-Agent", "NEXUS/1.0")
            val started = System.nanoTime()
            connection.connect()
            val connectMs = elapsedMs(started)
            val status = connection.responseCode
            val headers = connection.headerFields.orEmpty()
                .filterKeys { it != null }
                .flatMap { (name, values) -> values.map { HttpHeader(name!!, it) } }
                .sortedBy { it.name.lowercase() }
            return Response(status, connection.responseMessage.orEmpty(), headers, connectMs)
        } finally {
            connection.disconnect()
        }
    }

    private fun port(url: URL): Int = if (url.port > 0) url.port else url.defaultPort
}

class ConnectivityToolkit(
    private val connector: StreamConnector = SocketConnector,
    private val resolver: HostResolving = SystemResolver,
) {
    fun reachability(host: String, port: Int, timeoutMs: Int = 3_000): ConnectivityResult {
        val endpoint = Endpoint(host, port)
        val started = System.nanoTime()
        return try {
            val resolved = resolver.resolve(host, timeoutMs)
            connector.connect(Endpoint(resolved.addresses.first().toString(), port), timeoutMs).close()
            ConnectivityResult(endpoint, true, elapsedMs(started), resolved.addresses)
        } catch (error: Exception) {
            ConnectivityResult(endpoint, false, failure = NexusFailures.map(error))
        }
    }

    fun tracerouteAvailability(): String =
        "Traceroute and ICMP ping need raw packet access that Android does not give apps. Use an approved remote scanner for hop-by-hop traces."
}
