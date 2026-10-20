package com.forkmesh.nativeapp.nexus

import java.util.concurrent.ExecutorCompletionService
import java.util.concurrent.Executors
import java.util.concurrent.RejectedExecutionException
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicInteger

data class ScanProgress(val completed: Int, val total: Int, val result: PortResult? = null) {
    val fraction: Double get() = if (total == 0) 1.0 else completed.toDouble() / total
}

fun interface ServiceInspecting {
    fun inspect(host: String, port: Int, timeoutMs: Int): ServiceIdentity?
}

class ServiceInspector(private val connector: StreamConnector = SocketConnector) : ServiceInspecting {
    override fun inspect(host: String, port: Int, timeoutMs: Int): ServiceIdentity? = try {
        val stream = connector.connect(Endpoint(host, port), timeoutMs)
        try {
            if (port in HTTP_PORTS) stream.send("HEAD / HTTP/1.1\r\nHost: $host\r\nConnection: close\r\n\r\n".toByteArray(), timeoutMs)
            identify(stream.receiveUntil(4096, timeoutMs) { bytes -> bytes.contains(0x0A) || bytes.size >= 4096 }, port)
        } finally {
            stream.close()
        }
    } catch (_: Exception) {
        ServiceIdentity.inferred(port)
    }

    fun identify(data: ByteArray, port: Int): ServiceIdentity? {
        val text = String(data, Charsets.UTF_8).trim { it.isWhitespace() || it.isISOControl() }
        if (text.startsWith("HTTP/")) {
            val first = text.lineSequence().firstOrNull()?.trim().orEmpty().ifEmpty { "HTTP response" }
            return ServiceIdentity("http", "HTTP", EvidenceLevel.Confirmed, "The service returned $first.", banner = text.take(1024))
        }
        val upper = text.uppercase()
        val name = when {
            text.startsWith("SSH-") -> "ssh"
            upper.startsWith("220") && "FTP" in upper -> "ftp"
            upper.startsWith("220") && ("SMTP" in upper || "ESMTP" in upper) -> "smtp"
            else -> null
        } ?: return ServiceIdentity.inferred(port)
        val title = if (name == "ssh") "SSH" else name.uppercase()
        val product = if (name == "ssh") text.lineSequence().first().removePrefix("SSH-").substringAfter('-').substringBefore(' ').takeIf { it.isNotBlank() } else null
        return ServiceIdentity(name, title, EvidenceLevel.Confirmed, "The service returned its protocol greeting.", product = product, banner = text.take(1024))
    }

    companion object {
        val HTTP_PORTS = setOf(80, 81, 3000, 5000, 8000, 8080, 8081, 8088, 8888, 9000, 9090)
    }
}

class PortScanner(
    private val connector: StreamConnector = SocketConnector,
    private val inspector: ServiceInspecting = ServiceInspector(connector),
    private val resolver: HostResolving = SystemResolver,
) {
    fun run(
        target: TargetSpecification,
        ports: PortList,
        options: ScanOptions = ScanOptions(),
        authorization: ScanAuthorizationPolicy = ScanAuthorizationPolicy(),
        attestation: AuthorizationAttestation? = null,
        cancel: NexusCancel = NexusCancel(),
        progress: (ScanProgress) -> Unit = {},
    ): ScanSession {
        val settings = options.clamped
        val hosts = resolve(target, settings.timeoutMs, cancel)
        val addresses = hosts.flatMap { it.addresses }
        when (val decision = authorization.evaluate(addresses, ports.count, target.containsRange, attestation)) {
            AuthorizationDecision.Allowed -> Unit
            is AuthorizationDecision.RequiresAttestation -> throw ConnectionFailure.AttestationRequired(decision.message)
            is AuthorizationDecision.Denied -> throw ConnectionFailure.Denied(decision.message)
        }
        var session = ScanSession(target = target.raw, ports = ports, resolvedHosts = hosts, options = settings, attestation = attestation)
        val jobs = hosts.flatMap { host -> host.addresses.flatMap { address -> ports.ports.map { Triple(host.name, address, it) } } }
        val pool = Executors.newFixedThreadPool(settings.maximumConcurrency.coerceAtMost(jobs.size.coerceAtLeast(1))) { task ->
            Thread(task, "nexus-scan").apply { isDaemon = true }
        }
        cancel.onCancel { pool.shutdownNow() }
        val completion = ExecutorCompletionService<PortResult>(pool)
        val completed = AtomicInteger(0)
        val results = ArrayList<PortResult>(jobs.size)
        var counts = PortStateCounts()
        try {
            var submitted = 0
            fun submitNext() {
                if (submitted >= jobs.size || cancel.isCancelled) return
                val (host, address, port) = jobs[submitted]
                try {
                    completion.submit { probe(host, address, port, settings, cancel) }
                    submitted++
                } catch (_: RejectedExecutionException) {
                }
            }
            repeat(settings.maximumConcurrency) { submitNext() }
            var received = 0
            while (received < submitted && !cancel.isCancelled) {
                val result = try {
                    completion.poll(100, TimeUnit.MILLISECONDS)?.get() ?: continue
                } catch (_: Exception) {
                    break
                }
                received++
                if (result.state != PortState.Closed || settings.recordClosedPorts) results += result
                counts = counts.adding(result.state)
                progress(ScanProgress(completed.incrementAndGet(), jobs.size, result))
                submitNext()
            }
        } finally {
            pool.shutdownNow()
        }
        session = session.copy(
            results = results,
            counts = counts,
            status = if (cancel.isCancelled) SessionStatus.Cancelled else SessionStatus.Completed,
            finishedAt = System.currentTimeMillis(),
        )
        return session
    }

    private fun resolve(target: TargetSpecification, timeoutMs: Int, cancel: NexusCancel): List<ResolvedHost> = target.hosts().map { host ->
        cancel.check()
        when (host) {
            is TargetHost.Address -> ResolvedHost(host.address.toString(), listOf(host.address))
            is TargetHost.Name -> resolver.resolve(host.name, timeoutMs)
        }
    }

    private fun probe(host: String, address: IPAddr, port: Int, options: ScanOptions, cancel: NexusCancel): PortResult {
        if (cancel.isCancelled) return PortResult(host, address.toString(), port, PortState.Indeterminate, failure = ConnectionFailure.Cancelled)
        val started = System.nanoTime()
        return try {
            connector.connect(Endpoint(address.toString(), port), options.timeoutMs).close()
            val latency = elapsedMs(started)
            val service = if (options.inspectServices && !cancel.isCancelled) inspector.inspect(host, port, options.inspectionTimeoutMs) else ServiceIdentity.inferred(port)
            PortResult(host, address.toString(), port, PortState.Open, latency, service = service)
        } catch (error: Exception) {
            val failure = NexusFailures.map(error)
            if (failure == ConnectionFailure.TimedOut && options.retryTimeouts && !cancel.isCancelled) {
                return probe(host, address, port, options.copy(timeoutSeconds = (options.timeoutSeconds * 2).coerceAtMost(30.0), retryTimeouts = false), cancel)
            }
            PortResult(host, address.toString(), port, failure.portState, failure = failure)
        }
    }
}

object SecurityInsights {
    fun http(url: String, status: Int, headers: List<HttpHeader>): List<SecurityObservation> {
        val table = headers.groupBy { it.name.lowercase() }.mapValues { it.value.first().value }
        val subject = runCatching { java.net.URI(url).host }.getOrNull() ?: url
        val findings = mutableListOf<SecurityObservation>()
        fun add(severity: SecuritySeverity, title: String, detail: String) {
            findings += SecurityObservation(severity, title, detail, subject)
        }
        val scheme = url.substringBefore("://").lowercase()
        if (scheme == "http") add(SecuritySeverity.Attention, "Unencrypted HTTP", "This endpoint was inspected over HTTP, so request and response content can be observed or altered on the network.")
        if (scheme == "https" && table["strict-transport-security"] == null) add(SecuritySeverity.Informational, "No HSTS header observed", "The response did not include Strict-Transport-Security.")
        if (table["content-security-policy"] == null) add(SecuritySeverity.Informational, "No Content-Security-Policy observed", "The response did not include a Content-Security-Policy header.")
        if (table["x-content-type-options"]?.lowercase() != "nosniff") add(SecuritySeverity.Informational, "MIME sniffing protection not observed", "The response did not include X-Content-Type-Options: nosniff.")
        if (status >= 500) add(SecuritySeverity.Attention, "Server error response", "The endpoint returned HTTP $status.")
        return findings
    }

    fun tls(observation: TlsObservation, now: Long = System.currentTimeMillis()): List<SecurityObservation> {
        val findings = mutableListOf<SecurityObservation>()
        val leaf = observation.leaf
        if (!observation.trust.isTrusted) findings += SecurityObservation(SecuritySeverity.Warning, "Certificate not trusted", observation.trust.summary, observation.host)
        if (leaf != null) {
            val days = leaf.daysUntilExpiry(now)
            if (days < 0) {
                findings += SecurityObservation(SecuritySeverity.Warning, "Certificate expired", "The leaf certificate expired ${-days} days ago.", observation.host)
            } else if (days < 14) {
                findings += SecurityObservation(SecuritySeverity.Attention, "Certificate expires soon", "The leaf certificate expires in $days days.", observation.host)
            }
        }
        val protocol = observation.protocolVersion
        if (protocol != null && (protocol == "TLSv1" || protocol == "TLSv1.1" || protocol.startsWith("SSL"))) {
            findings += SecurityObservation(SecuritySeverity.Attention, "Old TLS version", "The connection negotiated $protocol.", observation.host)
        }
        return findings
    }

    fun portChanges(current: ScanSession, previous: ScanSession?): List<SecurityObservation> {
        if (previous == null) return emptyList()
        val oldOpen = previous.openResults.map { it.id }.toSet()
        return current.openResults.map { it.id }.filter { it !in oldOpen }.sorted().map {
            SecurityObservation(SecuritySeverity.Attention, "New exposed TCP port", "$it was not open in the previous scan.", current.target)
        }
    }
}
