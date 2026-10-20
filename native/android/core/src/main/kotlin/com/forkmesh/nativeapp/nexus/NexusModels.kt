package com.forkmesh.nativeapp.nexus

import java.util.UUID

enum class EvidenceLevel(val raw: String, val title: String) {
    Confirmed("confirmed", "Confirmed"),
    Inferred("inferred", "Inferred");

    companion object {
        fun of(raw: String): EvidenceLevel = entries.firstOrNull { it.raw == raw } ?: Inferred
    }
}

enum class PortState(val raw: String, val title: String, val explanation: String, val icon: String, val rank: Int) {
    Open("open", "Open", "A TCP connection was established.", "lock_open", 0),
    Closed("closed", "Closed", "The host answered and refused the connection.", "cancel", 4),
    TimedOut("timedOut", "No response", "No answer before the timeout: filtered by a firewall, or the host is offline.", "hourglass_empty", 2),
    Unreachable("unreachable", "Unreachable", "The network reported the host or network as unreachable.", "wifi_off", 3),
    Indeterminate("indeterminate", "Indeterminate", "The attempt failed in a way that does not show whether the port is open.", "help", 1);

    companion object {
        fun of(raw: String): PortState = entries.firstOrNull { it.raw == raw } ?: Indeterminate
    }
}

sealed class ConnectionFailure(val code: String, val title: String, val detail: String) : Exception(detail) {
    object Refused : ConnectionFailure("refused", "Connection refused", "The host answered and refused the connection (TCP reset).")
    object TimedOut : ConnectionFailure("timedOut", "Timed out", "No answer arrived before the timeout.")
    object HostUnreachable : ConnectionFailure("hostUnreachable", "Host unreachable", "The host could not be reached (no route or no ARP/NDP reply).")
    object NetworkUnreachable : ConnectionFailure("networkUnreachable", "Network unreachable", "There is no route to the network.")
    class NameResolution(why: String) : ConnectionFailure("nameResolution", "Name resolution failed", "The name could not be resolved: $why")
    object PermissionDenied : ConnectionFailure("permissionDenied", "Network access denied", "Android blocked the connection: check that ForkMesh may use the network and that no VPN or data saver blocks it.")
    object ResetByPeer : ConnectionFailure("resetByPeer", "Connection reset", "The host closed the connection abruptly.")
    class Tls(why: String) : ConnectionFailure("tls", "TLS failure", "The TLS handshake failed: $why")
    object Cancelled : ConnectionFailure("cancelled", "Cancelled", "The operation was cancelled.")
    class Unsupported(why: String) : ConnectionFailure("unsupported", "Not supported", why)
    class Denied(why: String) : ConnectionFailure("denied", "Not allowed", why)
    class AttestationRequired(why: String) : ConnectionFailure("attestationRequired", "Confirmation needed", why)
    class System(why: String) : ConnectionFailure("system", "System error", why)

    val portState: PortState
        get() = when (this) {
            Refused -> PortState.Closed
            TimedOut -> PortState.TimedOut
            HostUnreachable, NetworkUnreachable -> PortState.Unreachable
            else -> PortState.Indeterminate
        }

    override fun equals(other: Any?): Boolean = other is ConnectionFailure && other.code == code && other.detail == detail
    override fun hashCode(): Int = code.hashCode() * 31 + detail.hashCode()

    fun toJson(): Map<String, Any?> = linkedMapOf("code" to code, "detail" to detail)

    companion object {
        fun fromJson(json: Map<String, Any?>): ConnectionFailure? {
            val detail = json["detail"] as? String ?: ""
            return when (json["code"] as? String) {
                "refused" -> Refused
                "timedOut" -> TimedOut
                "hostUnreachable" -> HostUnreachable
                "networkUnreachable" -> NetworkUnreachable
                "nameResolution" -> NameResolution(detail.removePrefix("The name could not be resolved: "))
                "permissionDenied" -> PermissionDenied
                "resetByPeer" -> ResetByPeer
                "tls" -> Tls(detail.removePrefix("The TLS handshake failed: "))
                "cancelled" -> Cancelled
                "unsupported" -> Unsupported(detail)
                "denied" -> Denied(detail)
                "attestationRequired" -> AttestationRequired(detail)
                "system" -> System(detail)
                else -> null
            }
        }
    }
}

data class HttpHeader(val name: String, val value: String)

data class ServiceIdentity(
    val name: String,
    val title: String,
    val evidence: EvidenceLevel,
    val basis: String,
    val product: String? = null,
    val version: String? = null,
    val banner: String? = null,
) {
    val productDescription: String?
        get() = when {
            product != null && version != null -> "$product $version"
            else -> product ?: version
        }

    fun toJson(): Map<String, Any?> = linkedMapOf(
        "name" to name, "title" to title, "evidence" to evidence.raw, "basis" to basis,
        "product" to product, "version" to version, "banner" to banner,
    )

    companion object {
        fun inferred(port: Int): ServiceIdentity? {
            val known = ServiceCatalog.service(port) ?: return null
            return ServiceIdentity(known.name, known.title, EvidenceLevel.Inferred, "Port $port is commonly used by ${known.title}; the service itself did not identify.")
        }

        fun fromJson(json: Map<String, Any?>): ServiceIdentity = ServiceIdentity(
            name = json.text("name"), title = json.text("title"), evidence = EvidenceLevel.of(json.text("evidence")), basis = json.text("basis"),
            product = json["product"] as? String, version = json["version"] as? String, banner = json["banner"] as? String,
        )
    }
}

data class PortResult(
    val host: String,
    val address: String?,
    val port: Int,
    val state: PortState,
    val latencyMilliseconds: Double? = null,
    val failure: ConnectionFailure? = null,
    val service: ServiceIdentity? = null,
    val observedAt: Long = System.currentTimeMillis(),
) {
    val id: String get() = "${address ?: host}#$port"
    val hostKey: String get() = address ?: host
    val serviceName: String get() = service?.title ?: ServiceCatalog.service(port)?.title ?: "Unknown"

    fun toJson(): Map<String, Any?> = linkedMapOf(
        "host" to host, "address" to address, "port" to port.toLong(), "state" to state.raw,
        "latencyMilliseconds" to latencyMilliseconds, "failure" to failure?.toJson(), "service" to service?.toJson(),
        "observedAt" to NexusTime.iso(observedAt),
    )

    companion object {
        fun fromJson(json: Map<String, Any?>): PortResult = PortResult(
            host = json.text("host"), address = json["address"] as? String, port = json.number("port").toInt(),
            state = PortState.of(json.text("state")), latencyMilliseconds = (json["latencyMilliseconds"] as? Number)?.toDouble(),
            failure = json.child("failure")?.let(ConnectionFailure::fromJson), service = json.child("service")?.let(ServiceIdentity::fromJson),
            observedAt = NexusTime.parse(json["observedAt"]),
        )
    }
}

data class CertificateSummary(
    val subject: String,
    val subjectCommonName: String?,
    val issuer: String,
    val issuerCommonName: String?,
    val serialNumber: String,
    val version: Int,
    val notBefore: Long,
    val notAfter: Long,
    val subjectAlternativeNames: List<String>,
    val publicKeyAlgorithm: String,
    val publicKeyBits: Int?,
    val signatureAlgorithm: String,
    val isCertificateAuthority: Boolean,
    val isSelfIssued: Boolean,
    val sha256Fingerprint: String,
) {
    val displayName: String get() = subjectCommonName ?: subjectAlternativeNames.firstOrNull() ?: subject

    fun daysUntilExpiry(now: Long = System.currentTimeMillis()): Int = Math.floorDiv(notAfter - now, DAY_MS).toInt()

    val validityDays: Int get() = Math.round((notAfter - notBefore).toDouble() / DAY_MS).toInt()

    companion object {
        const val DAY_MS = 86_400_000L
    }
}

sealed class TrustOutcome {
    object Trusted : TrustOutcome()
    data class Untrusted(val reason: String) : TrustOutcome()
    data class NotEvaluated(val reason: String) : TrustOutcome()

    val isTrusted: Boolean get() = this == Trusted

    val summary: String
        get() = when (this) {
            Trusted -> "Trusted by the system trust store"
            is Untrusted -> "Not trusted: $reason"
            is NotEvaluated -> "Not evaluated: $reason"
        }
}

data class TlsObservation(
    val host: String,
    val port: Int,
    val serverName: String?,
    val protocolVersion: String? = null,
    val cipherSuite: String? = null,
    val negotiatedAlpn: String? = null,
    val chain: List<CertificateSummary>,
    val trust: TrustOutcome,
    val handshakeMilliseconds: Double? = null,
    val observedAt: Long = System.currentTimeMillis(),
) {
    val leaf: CertificateSummary? get() = chain.firstOrNull()
}

enum class DnsRecordType(val code: Int, val title: String) {
    A(1, "A"), NS(2, "NS"), CNAME(5, "CNAME"), SOA(6, "SOA"), PTR(12, "PTR"), MX(15, "MX"), TXT(16, "TXT"), AAAA(28, "AAAA");

    companion object {
        fun of(code: Int): DnsRecordType? = entries.firstOrNull { it.code == code }

        val lookupChoices = listOf(A, AAAA, CNAME, MX, TXT, NS, PTR)
    }
}

data class DnsRecord(val name: String, val type: DnsRecordType, val ttl: Long, val value: String) {
    val id: String get() = "$name|${type.code}|$value"
}

data class DnsLookup(
    val name: String,
    val type: DnsRecordType,
    val answers: List<DnsRecord> = emptyList(),
    val authority: List<DnsRecord> = emptyList(),
    val additional: List<DnsRecord> = emptyList(),
    val responseCode: Int = 0,
    val resolver: String,
    val latencyMilliseconds: Double,
    val error: String? = null,
)

enum class SecuritySeverity(val raw: String) { Informational("informational"), Attention("attention"), Warning("warning") }

data class SecurityObservation(
    val severity: SecuritySeverity,
    val title: String,
    val detail: String,
    val subject: String,
    val id: String = UUID.randomUUID().toString(),
    val observedAt: Long = System.currentTimeMillis(),
)

data class HttpTimings(val connectMilliseconds: Double?, val firstByteMilliseconds: Double?, val totalMilliseconds: Double)

data class HttpInspection(
    val requestedUrl: String,
    val finalUrl: String,
    val statusCode: Int,
    val reason: String,
    val headers: List<HttpHeader>,
    val redirects: List<String>,
    val elapsedMilliseconds: Double,
    val timings: HttpTimings,
    val securityObservations: List<SecurityObservation>,
    val tls: TlsObservation? = null,
)

data class Endpoint(val host: String, val port: Int) {
    override fun toString(): String = if (':' in host && !host.startsWith("[")) "[$host]:$port" else "$host:$port"
}

data class ConnectivityResult(
    val endpoint: Endpoint,
    val reachable: Boolean,
    val latencyMilliseconds: Double? = null,
    val resolvedAddresses: List<IPAddr> = emptyList(),
    val failure: ConnectionFailure? = null,
    val observedAt: Long = System.currentTimeMillis(),
)

sealed class SessionStatus(val raw: String, val title: String) {
    object Running : SessionStatus("running", "Running")
    object Completed : SessionStatus("completed", "Completed")
    object Cancelled : SessionStatus("cancelled", "Cancelled")
    data class Failed(val reason: String) : SessionStatus("failed", "Failed")

    val isFinished: Boolean get() = this != Running

    fun toJson(): Map<String, Any?> = linkedMapOf("state" to raw, "reason" to (this as? Failed)?.reason)

    companion object {
        fun fromJson(json: Map<String, Any?>?): SessionStatus = when (json?.get("state")) {
            "completed" -> Completed
            "cancelled" -> Cancelled
            "failed" -> Failed(json["reason"] as? String ?: "")
            "running" -> Running
            else -> Completed
        }
    }
}

data class ResolvedHost(val name: String, val addresses: List<IPAddr>, val error: String? = null) {
    fun toJson(): Map<String, Any?> = linkedMapOf("name" to name, "addresses" to addresses.map { it.toString() }, "error" to error)

    companion object {
        fun fromJson(json: Map<String, Any?>): ResolvedHost =
            ResolvedHost(json.text("name"), json.strings("addresses").mapNotNull(IPAddr::parse), json["error"] as? String)
    }
}

data class ScanOptions(
    val timeoutSeconds: Double = 1.5,
    val maximumConcurrency: Int = 48,
    val inspectServices: Boolean = true,
    val inspectionTimeoutSeconds: Double = 3.0,
    val retryTimeouts: Boolean = false,
    val recordClosedPorts: Boolean = false,
    val preferIPv4: Boolean = true,
) {
    val clamped: ScanOptions
        get() = copy(
            timeoutSeconds = timeoutSeconds.coerceIn(0.2, 30.0),
            maximumConcurrency = maximumConcurrency.coerceIn(1, 128),
            inspectionTimeoutSeconds = inspectionTimeoutSeconds.coerceIn(0.5, 30.0),
        )

    val timeoutMs: Int get() = (timeoutSeconds * 1000).toInt()
    val inspectionTimeoutMs: Int get() = (inspectionTimeoutSeconds * 1000).toInt()

    fun toJson(): Map<String, Any?> = linkedMapOf(
        "timeoutSeconds" to timeoutSeconds, "maximumConcurrency" to maximumConcurrency.toLong(), "inspectServices" to inspectServices,
        "inspectionTimeoutSeconds" to inspectionTimeoutSeconds, "retryTimeouts" to retryTimeouts, "recordClosedPorts" to recordClosedPorts,
        "preferIPv4" to preferIPv4,
    )

    companion object {
        fun fromJson(json: Map<String, Any?>?): ScanOptions {
            val defaults = ScanOptions()
            if (json == null) return defaults
            return ScanOptions(
                timeoutSeconds = (json["timeoutSeconds"] as? Number)?.toDouble() ?: defaults.timeoutSeconds,
                maximumConcurrency = (json["maximumConcurrency"] as? Number)?.toInt() ?: defaults.maximumConcurrency,
                inspectServices = json["inspectServices"] as? Boolean ?: defaults.inspectServices,
                inspectionTimeoutSeconds = (json["inspectionTimeoutSeconds"] as? Number)?.toDouble() ?: defaults.inspectionTimeoutSeconds,
                retryTimeouts = json["retryTimeouts"] as? Boolean ?: defaults.retryTimeouts,
                recordClosedPorts = json["recordClosedPorts"] as? Boolean ?: defaults.recordClosedPorts,
                preferIPv4 = json["preferIPv4"] as? Boolean ?: defaults.preferIPv4,
            )
        }
    }
}

sealed class ScanEngineKind {
    object Native : ScanEngineKind()
    data class Remote(val agent: String, val profile: String) : ScanEngineKind()

    val title: String
        get() = when (this) {
            Native -> "On-device TCP connect"
            is Remote -> "Remote agent · $profile"
        }

    fun toJson(): Map<String, Any?> = when (this) {
        Native -> linkedMapOf("kind" to "native")
        is Remote -> linkedMapOf("kind" to "remote", "agent" to agent, "profile" to profile)
    }

    companion object {
        fun fromJson(json: Map<String, Any?>?): ScanEngineKind =
            if (json?.get("kind") == "remote") Remote(json.text("agent"), json.text("profile")) else Native
    }
}

data class PortStateCounts(val open: Int = 0, val closed: Int = 0, val timedOut: Int = 0, val unreachable: Int = 0, val indeterminate: Int = 0) {
    operator fun get(state: PortState): Int = when (state) {
        PortState.Open -> open
        PortState.Closed -> closed
        PortState.TimedOut -> timedOut
        PortState.Unreachable -> unreachable
        PortState.Indeterminate -> indeterminate
    }

    fun adding(state: PortState): PortStateCounts = when (state) {
        PortState.Open -> copy(open = open + 1)
        PortState.Closed -> copy(closed = closed + 1)
        PortState.TimedOut -> copy(timedOut = timedOut + 1)
        PortState.Unreachable -> copy(unreachable = unreachable + 1)
        PortState.Indeterminate -> copy(indeterminate = indeterminate + 1)
    }

    val total: Int get() = open + closed + timedOut + unreachable + indeterminate

    fun toJson(): Map<String, Any?> = PortState.entries.associate { it.raw to this[it].toLong() }

    companion object {
        fun fromJson(json: Map<String, Any?>?): PortStateCounts = PortStateCounts(
            json.number("open").toInt(), json.number("closed").toInt(), json.number("timedOut").toInt(),
            json.number("unreachable").toInt(), json.number("indeterminate").toInt(),
        )
    }
}

data class AuthorizationAttestation(
    val statement: String = DEFAULT_STATEMENT,
    val confirmedAt: Long = System.currentTimeMillis(),
    val reference: String? = null,
) {
    fun toJson(): Map<String, Any?> = linkedMapOf("statement" to statement, "confirmedAt" to NexusTime.iso(confirmedAt), "reference" to reference)

    companion object {
        const val DEFAULT_STATEMENT = "I own these targets or have written permission to test them."

        fun fromJson(json: Map<String, Any?>): AuthorizationAttestation =
            AuthorizationAttestation(json.text("statement"), NexusTime.parse(json["confirmedAt"]), json["reference"] as? String)
    }
}

data class ScanSession(
    val target: String,
    val ports: PortList,
    val id: String = UUID.randomUUID().toString(),
    val resolvedHosts: List<ResolvedHost> = emptyList(),
    val options: ScanOptions = ScanOptions(),
    val engine: ScanEngineKind = ScanEngineKind.Native,
    val startedAt: Long = System.currentTimeMillis(),
    val finishedAt: Long? = null,
    val status: SessionStatus = SessionStatus.Running,
    val results: List<PortResult> = emptyList(),
    val counts: PortStateCounts = PortStateCounts(),
    val attestation: AuthorizationAttestation? = null,
    val notes: List<String> = emptyList(),
) {
    val openResults: List<PortResult>
        get() = results.filter { it.state == PortState.Open }.sortedWith(compareBy({ it.hostKey }, { it.port }))

    val hostKeys: List<String> get() = results.map { it.hostKey }.toSortedSet().toList()

    val durationSeconds: Double? get() = finishedAt?.let { (it - startedAt) / 1000.0 }

    val scannedAddresses: List<IPAddr> get() = resolvedHosts.flatMap { it.addresses }

    fun toJson(): Map<String, Any?> = linkedMapOf(
        "id" to id, "target" to target, "resolvedHosts" to resolvedHosts.map { it.toJson() }, "ports" to ports.toString(),
        "options" to options.toJson(), "engine" to engine.toJson(), "startedAt" to NexusTime.iso(startedAt),
        "finishedAt" to finishedAt?.let(NexusTime::iso), "status" to status.toJson(), "results" to results.map { it.toJson() },
        "counts" to counts.toJson(), "attestation" to attestation?.toJson(), "notes" to notes,
    )

    companion object {
        fun fromJson(json: Map<String, Any?>): ScanSession = ScanSession(
            target = json.text("target"), ports = PortList.decode(json.text("ports")), id = json.text("id").ifEmpty { UUID.randomUUID().toString() },
            resolvedHosts = json.maps("resolvedHosts").map(ResolvedHost::fromJson), options = ScanOptions.fromJson(json.child("options")),
            engine = ScanEngineKind.fromJson(json.child("engine")), startedAt = NexusTime.parse(json["startedAt"]),
            finishedAt = json["finishedAt"]?.let(NexusTime::parse), status = SessionStatus.fromJson(json.child("status")),
            results = json.maps("results").map(PortResult::fromJson), counts = PortStateCounts.fromJson(json.child("counts")),
            attestation = json.child("attestation")?.let(AuthorizationAttestation::fromJson), notes = json.strings("notes"),
        )
    }
}

enum class DiscoverySource(val raw: String, val title: String, val icon: String) {
    ThisDevice("thisDevice", "This device", "smartphone"),
    Mdns("bonjour", "Network services (mDNS)", "cell_tower"),
    TcpSweep("tcpSweep", "TCP sweep", "radar"),
    PortScan("portScan", "Port scan", "my_location"),
    ReverseDns("reverseDNS", "Reverse DNS", "menu_book"),
    RemoteAgent("remoteAgent", "Remote agent", "dns"),
    Manual("manual", "Added manually", "touch_app");

    companion object {
        fun of(raw: String): DiscoverySource? = entries.firstOrNull { it.raw == raw }
    }
}

data class NetService(
    val name: String,
    val type: String,
    val domain: String = "local.",
    val port: Int? = null,
    val hostName: String? = null,
    val addresses: List<IPAddr> = emptyList(),
    val txt: Map<String, String> = emptyMap(),
    val interfaceName: String? = null,
    val discoveredAt: Long = System.currentTimeMillis(),
) {
    val id: String get() = "$name.$type.$domain"

    fun toJson(): Map<String, Any?> = linkedMapOf(
        "name" to name, "type" to type, "domain" to domain, "port" to port?.toLong(), "hostName" to hostName,
        "addresses" to addresses.map { it.toString() }, "txt" to txt, "interfaceName" to interfaceName, "discoveredAt" to NexusTime.iso(discoveredAt),
    )

    companion object {
        fun fromJson(json: Map<String, Any?>): NetService = NetService(
            name = json.text("name"), type = json.text("type"), domain = json.text("domain").ifEmpty { "local." },
            port = (json["port"] as? Number)?.toInt(), hostName = json["hostName"] as? String,
            addresses = json.strings("addresses").mapNotNull(IPAddr::parse),
            txt = json.child("txt").orEmpty().mapValues { it.value?.toString() ?: "" }, interfaceName = json["interfaceName"] as? String,
            discoveredAt = NexusTime.parse(json["discoveredAt"]),
        )
    }
}

enum class DeviceKind(val raw: String, val title: String, val icon: String) {
    Router("router", "Router or gateway", "router"),
    Computer("computer", "Computer", "computer"),
    Phone("phone", "Phone", "smartphone"),
    Tablet("tablet", "Tablet", "tablet"),
    Printer("printer", "Printer or scanner", "print"),
    Tv("tv", "TV or streaming device", "tv"),
    Speaker("speaker", "Speaker", "speaker"),
    Camera("camera", "Camera", "videocam"),
    Storage("storage", "Network storage", "storage"),
    SmartHome("smartHome", "Smart home device", "home"),
    GameConsole("gameConsole", "Game console", "sports_esports"),
    Server("server", "Server", "dns"),
    Unknown("unknown", "Unknown device", "devices_other");

    companion object {
        fun of(raw: String): DeviceKind = entries.firstOrNull { it.raw == raw } ?: Unknown
    }
}

data class DeviceIdentification(val kind: DeviceKind, val evidence: EvidenceLevel, val basis: String, val model: String? = null, val manufacturer: String? = null)

data class Device(
    val id: String,
    val displayName: String,
    val addresses: List<IPAddr> = emptyList(),
    val hostnames: List<String> = emptyList(),
    val services: List<NetService> = emptyList(),
    val openPorts: List<PortResult> = emptyList(),
    val identification: DeviceIdentification? = null,
    val sources: List<DiscoverySource> = emptyList(),
    val firstSeen: Long = System.currentTimeMillis(),
    val lastSeen: Long = System.currentTimeMillis(),
    val isCurrentDevice: Boolean = false,
    val userLabel: String? = null,
) {
    val title: String get() = userLabel ?: displayName
    val primaryAddress: IPAddr? get() = addresses.firstOrNull { it.family == IPAddr.Family.IPv4 } ?: addresses.firstOrNull()
    val kind: DeviceKind get() = identification?.kind ?: DeviceKind.Unknown
    val serviceTypes: List<String> get() = services.map { it.type }.toSortedSet().toList()

    val searchText: String
        get() = (listOf(title, displayName) + addresses.map { it.toString() } + hostnames + services.map { it.name } + services.map { it.type } +
            openPorts.map { "${it.port} ${it.serviceName}" } + listOfNotNull(identification?.model, identification?.manufacturer, kind.title))
            .joinToString(" ").lowercase()

    fun toJson(): Map<String, Any?> = linkedMapOf(
        "id" to id, "displayName" to displayName, "addresses" to addresses.map { it.toString() }, "hostnames" to hostnames,
        "services" to services.map { it.toJson() }, "openPorts" to openPorts.map { it.toJson() },
        "identification" to identification?.let { linkedMapOf("kind" to it.kind.raw, "evidence" to it.evidence.raw, "basis" to it.basis, "model" to it.model, "manufacturer" to it.manufacturer) },
        "sources" to sources.map { it.raw }, "firstSeen" to NexusTime.iso(firstSeen), "lastSeen" to NexusTime.iso(lastSeen),
        "isCurrentDevice" to isCurrentDevice, "userLabel" to userLabel,
    )

    companion object {
        fun key(address: IPAddr): String = "ip:$address"

        fun fromJson(json: Map<String, Any?>): Device = Device(
            id = json.text("id"), displayName = json.text("displayName"), addresses = json.strings("addresses").mapNotNull(IPAddr::parse),
            hostnames = json.strings("hostnames"), services = json.maps("services").map(NetService::fromJson),
            openPorts = json.maps("openPorts").map(PortResult::fromJson),
            identification = json.child("identification")?.let { DeviceIdentification(DeviceKind.of(it.text("kind")), EvidenceLevel.of(it.text("evidence")), it.text("basis"), it["model"] as? String, it["manufacturer"] as? String) },
            sources = json.strings("sources").mapNotNull(DiscoverySource::of), firstSeen = NexusTime.parse(json["firstSeen"]),
            lastSeen = NexusTime.parse(json["lastSeen"]), isCurrentDevice = json["isCurrentDevice"] == true, userLabel = json["userLabel"] as? String,
        )
    }
}

data class DiscoverySession(
    val id: String = UUID.randomUUID().toString(),
    val startedAt: Long = System.currentTimeMillis(),
    val finishedAt: Long? = null,
    val status: SessionStatus = SessionStatus.Running,
    val network: String? = null,
    val methods: List<DiscoverySource> = emptyList(),
    val serviceTypes: List<String> = emptyList(),
    val devices: List<Device> = emptyList(),
    val notes: List<String> = emptyList(),
) {
    fun toJson(): Map<String, Any?> = linkedMapOf(
        "id" to id, "startedAt" to NexusTime.iso(startedAt), "finishedAt" to finishedAt?.let(NexusTime::iso), "status" to status.toJson(),
        "network" to network, "methods" to methods.map { it.raw }, "serviceTypes" to serviceTypes, "devices" to devices.map { it.toJson() }, "notes" to notes,
    )

    companion object {
        fun fromJson(json: Map<String, Any?>): DiscoverySession = DiscoverySession(
            id = json.text("id").ifEmpty { UUID.randomUUID().toString() }, startedAt = NexusTime.parse(json["startedAt"]),
            finishedAt = json["finishedAt"]?.let(NexusTime::parse), status = SessionStatus.fromJson(json.child("status")),
            network = json["network"] as? String, methods = json.strings("methods").mapNotNull(DiscoverySource::of),
            serviceTypes = json.strings("serviceTypes"), devices = json.maps("devices").map(Device::fromJson), notes = json.strings("notes"),
        )
    }
}

enum class ChangeKind(val title: String, val icon: String) {
    DeviceAppeared("New device", "add_circle"),
    DeviceDisappeared("Device not seen", "remove_circle"),
    ServiceAdded("Service advertised", "cell_tower"),
    ServiceRemoved("Service no longer advertised", "portable_wifi_off"),
    PortOpened("Port opened", "lock_open"),
    PortClosed("Port no longer open", "lock"),
}

data class ObservedChange(
    val kind: ChangeKind,
    val deviceId: String?,
    val subject: String,
    val before: String? = null,
    val after: String? = null,
    val id: String = UUID.randomUUID().toString(),
    val detectedAt: Long = System.currentTimeMillis(),
) {
    val summary: String
        get() = when {
            before != null && after != null -> "$subject: $before → $after"
            after != null -> "$subject: $after"
            before != null -> "$subject: was $before"
            else -> subject
        }
}

object NexusTime {
    fun iso(millis: Long): String = java.time.Instant.ofEpochMilli(millis).toString()

    fun parse(value: Any?): Long = when (value) {
        is Number -> value.toLong()
        is String -> runCatching { java.time.Instant.parse(value).toEpochMilli() }.getOrDefault(0L)
        else -> 0L
    }
}

internal fun Map<String, Any?>.text(key: String): String = this[key] as? String ?: ""

internal fun Map<String, Any?>?.number(key: String): Long = (this?.get(key) as? Number)?.toLong() ?: 0L

@Suppress("UNCHECKED_CAST")
internal fun Map<String, Any?>.child(key: String): Map<String, Any?>? = this[key] as? Map<String, Any?>

@Suppress("UNCHECKED_CAST")
internal fun Map<String, Any?>.maps(key: String): List<Map<String, Any?>> = (this[key] as? List<Any?>).orEmpty().mapNotNull { it as? Map<String, Any?> }

internal fun Map<String, Any?>.strings(key: String): List<String> = (this[key] as? List<Any?>).orEmpty().mapNotNull { it as? String }
