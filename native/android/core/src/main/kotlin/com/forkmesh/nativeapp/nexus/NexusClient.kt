package com.forkmesh.nativeapp.nexus

import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.net.HttpClient

data class RemoteScannerConfiguration(val baseUrl: String, val token: String, val agentName: String, val allowedProfiles: List<String>)

data class RemoteScanRequest(val target: String, val ports: PortList, val profile: String, val attestation: AuthorizationAttestation) {
    fun toJson(): Map<String, Any?> = linkedMapOf("target" to target, "ports" to ports.toString(), "profile" to profile, "attestation" to attestation.toJson())
}

fun interface RemoteScanning {
    fun submit(request: RemoteScanRequest): ScanSession
}

class RemoteScannerClient(private val configuration: RemoteScannerConfiguration) : RemoteScanning {
    override fun submit(request: RemoteScanRequest): ScanSession {
        if (request.profile !in configuration.allowedProfiles) throw ConnectionFailure.Unsupported("The remote profile ${request.profile} is not approved by this integration.")
        val response = HttpClient.Request.post(configuration.baseUrl.trimEnd('/') + "/v1/scans").bearer(configuration.token).json(request.toJson()).send()
        if (response.status !in 200..299) throw ConnectionFailure.System("Remote scanner rejected the request.")
        return ScanSession.fromJson(response.jsonMap()).copy(engine = ScanEngineKind.Remote(configuration.agentName, request.profile))
    }
}

data class NexusConfiguration(
    val authorization: ScanAuthorizationPolicy = ScanAuthorizationPolicy(),
    val defaultScanOptions: ScanOptions = ScanOptions(),
    val remoteScanner: RemoteScannerConfiguration? = null,
)

class NexusClient(
    val configuration: NexusConfiguration = NexusConfiguration(),
    browser: ServiceBrowsing = ServiceBrowsing { _, _, _ -> throw ConnectionFailure.Unsupported("Service discovery needs the Android network service browser.") },
    streamConnector: StreamConnector = SocketConnector,
    datagramConnector: DatagramConnector = UdpConnector,
    hostResolver: HostResolving = SystemResolver,
    tls: TlsProbing = SystemTlsProbe,
) {
    val dns = DnsResolver(connector = datagramConnector)
    val discovery = DiscoveryEngine(browser)
    val scanner = PortScanner(streamConnector, ServiceInspector(streamConnector), hostResolver)
    val web = HttpInspector(tls)
    val connectivity = ConnectivityToolkit(streamConnector, hostResolver)
    val remote: RemoteScanning? = configuration.remoteScanner?.let(::RemoteScannerClient)

    fun discover(options: DiscoveryOptions = DiscoveryOptions(), cancel: NexusCancel = NexusCancel(), progress: (DiscoveryProgress) -> Unit = {}): DiscoverySession =
        discovery.discover(options, cancel, progress)

    fun scan(
        target: String,
        ports: PortList = PortPreset.Quick.ports,
        options: ScanOptions? = null,
        attestation: AuthorizationAttestation? = null,
        cancel: NexusCancel = NexusCancel(),
        progress: (ScanProgress) -> Unit = {},
    ): ScanSession {
        val specification = TargetSpecification.parse(target, configuration.authorization.maximumHostsPerScan)
        if (ports.isEmpty) throw PortParseException("Enter at least one port.")
        return scanner.run(specification, ports, options ?: configuration.defaultScanOptions, configuration.authorization, attestation, cancel, progress)
    }

    fun dnsLookup(name: String, type: DnsRecordType): DnsLookup {
        val trimmed = name.trim()
        val address = IPAddr.parse(trimmed)
        return if (address != null && type == DnsRecordType.PTR) dns.reverse(address) else dns.lookup(trimmed, type)
    }

    fun inspectWeb(url: String): HttpInspection = web.inspect(url)

    fun checkConnectivity(host: String, port: Int): ConnectivityResult = connectivity.reachability(host, port)
}

object NexusReports {
    fun json(value: Any?): String = pretty(value, 0)

    fun scanJson(session: ScanSession): String = json(session.toJson())

    fun csv(session: ScanSession): String {
        val header = "host,address,port,state,latency_ms,service,evidence\n"
        val rows = session.results.sortedWith(compareBy({ it.hostKey }, { it.port })).map { result ->
            listOf(
                result.host, result.address ?: "", result.port.toString(), result.state.raw,
                result.latencyMilliseconds?.let { String.format(java.util.Locale.ROOT, "%.2f", it) } ?: "",
                result.service?.name ?: "", result.service?.evidence?.raw ?: "",
            ).joinToString(",") { "\"" + it.replace("\"", "\"\"") + "\"" }
        }
        return header + rows.joinToString("\n") + "\n"
    }

    fun fileName(session: ScanSession, extension: String): String {
        val stamp = NexusTime.iso(session.startedAt).take(19).replace(":", "").replace("-", "")
        val target = session.target.replace(Regex("[^A-Za-z0-9.]+"), "_").take(40).trim('_').ifEmpty { "scan" }
        return "nexus-$target-$stamp.$extension"
    }

    private fun pretty(value: Any?, depth: Int): String {
        val pad = "  ".repeat(depth + 1)
        val close = "  ".repeat(depth)
        return when (value) {
            is Map<*, *> -> if (value.isEmpty()) "{}" else value.entries.sortedBy { it.key.toString() }.joinToString(",\n", "{\n", "\n$close}") {
                pad + Json.stringify(it.key.toString()) + ": " + pretty(it.value, depth + 1)
            }
            is Iterable<*> -> if (!value.iterator().hasNext()) "[]" else value.joinToString(",\n", "[\n", "\n$close]") { pad + pretty(it, depth + 1) }
            else -> Json.stringify(value)
        }
    }
}

object NexusHistory {
    const val MAX_SCANS = 50
    const val MAX_DISCOVERIES = 20

    data class Snapshot(val scans: List<ScanSession> = emptyList(), val discoveries: List<DiscoverySession> = emptyList(), val devices: List<Device> = emptyList())

    fun encode(snapshot: Snapshot): String = Json.stringify(
        linkedMapOf(
            "version" to 1L,
            "scans" to snapshot.scans.take(MAX_SCANS).map { it.toJson() },
            "discoveries" to snapshot.discoveries.take(MAX_DISCOVERIES).map { it.toJson() },
            "devices" to snapshot.devices.map { it.toJson() },
        ),
    )

    fun decode(text: String): Snapshot {
        val json = Json.parseObject(text)
        return Snapshot(
            scans = json.maps("scans").mapNotNull { runCatching { ScanSession.fromJson(it) }.getOrNull() }.sortedByDescending { it.startedAt },
            discoveries = json.maps("discoveries").mapNotNull { runCatching { DiscoverySession.fromJson(it) }.getOrNull() }.sortedByDescending { it.startedAt },
            devices = json.maps("devices").mapNotNull { runCatching { Device.fromJson(it) }.getOrNull() },
        )
    }
}
