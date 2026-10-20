package com.forkmesh.nativeapp.nexus

import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.assertFails
import com.forkmesh.nativeapp.testing.expect
import java.io.ByteArrayOutputStream
import java.net.InetAddress
import java.net.ServerSocket
import kotlin.concurrent.thread

private class FakeStream(private val reply: ByteArray) : NexusStream {
    private var sent = false
    override val remoteAddress: String? = null
    override fun send(data: ByteArray, timeoutMs: Int) {}
    override fun receive(maximumLength: Int, timeoutMs: Int): ByteArray = if (sent) ByteArray(0) else reply.also { sent = true }
    override fun close() {}
}

private class FakeConnector(private val outcomes: Map<Int, ConnectionFailure?>, private val banner: ByteArray = ByteArray(0)) : StreamConnector {
    val attempts = java.util.concurrent.atomic.AtomicInteger()
    override fun connect(endpoint: Endpoint, timeoutMs: Int): NexusStream {
        attempts.incrementAndGet()
        outcomes[endpoint.port]?.let { throw it }
        if (endpoint.port !in outcomes) throw ConnectionFailure.Refused
        return FakeStream(banner)
    }
}

private class FakeDatagram(private val respond: (ByteArray) -> ByteArray) : NexusDatagram {
    private var reply = ByteArray(0)
    override fun send(data: ByteArray) {
        reply = respond(data)
    }
    override fun receive(timeoutMs: Int): ByteArray = reply
    override fun close() {}
}

private fun dnsAnswer(query: ByteArray, records: (ByteArrayOutputStream) -> Int): ByteArray {
    val out = ByteArrayOutputStream()
    out.write(query, 0, 2)
    out.write(0x81)
    out.write(0x80)
    out.write(0)
    out.write(1)
    val body = ByteArrayOutputStream()
    val count = records(body)
    out.write(0)
    out.write(count)
    repeat(4) { out.write(0) }
    out.write(query, 12, query.size - 12)
    out.write(body.toByteArray())
    return out.toByteArray()
}

private fun ByteArrayOutputStream.record(type: Int, rdata: ByteArray) {
    write(0xC0)
    write(12)
    write(0)
    write(type)
    write(0)
    write(1)
    write(byteArrayOf(0, 0, 0x0E, 0x10))
    write(rdata.size shr 8)
    write(rdata.size and 0xFF)
    write(rdata)
}

class NexusTest {
    fun testIPv4Parsing() {
        assertEq("192.168.1.10", IPv4Addr.parse("192.168.1.10").toString())
        expect(IPv4Addr.parse("192.168.01.1") == null, "leading zero")
        expect(IPv4Addr.parse("256.1.1.1") == null, "octet range")
        expect(IPv4Addr.parse("1.2.3") == null, "three parts")
        expect(IPv4Addr.parse("10.0.0.1")!!.isPrivate)
        expect(IPv4Addr.parse("100.64.3.1")!!.isSharedAddressSpace)
        expect(!IPv4Addr.parse("8.8.8.8")!!.isLocalScope)
        assertEq("4.3.2.1.in-addr.arpa", IPv4Addr.parse("1.2.3.4")!!.reversePointerName)
    }

    fun testIPv6Formatting() {
        assertEq("2001:db8::1", IPv6Addr.parse("2001:db8::1").toString())
        assertEq("2001:db8::1", IPv6Addr.parse("2001:0DB8:0:0:0:0:0:1").toString())
        assertEq("::1", IPv6Addr.parse("::1").toString())
        assertEq("::ffff:10.0.0.1", IPv6Addr.parse("::ffff:10.0.0.1").toString())
        assertEq("fe80::1%wlan0", IPv6Addr.parse("fe80::1%wlan0").toString())
        expect(IPv6Addr.parse("::ffff:10.0.0.1")!!.isLocalScope)
        expect(IPv6Addr.parse("fd00::5")!!.isUniqueLocal)
        expect(IPv6Addr.parse("1::2::3") == null)
        expect(IPv6Addr.parse("+1::") == null)
        expect(IPv6Addr.parse("[::1]")!!.isLoopback)
    }

    fun testNetworks() {
        val net = IPv4Network.parse("192.168.1.12/24")!!
        assertEq("192.168.1.0/24", net.toString())
        expect(net.contains(IPv4Addr.parse("192.168.1.99")!!))
        assertEq(254L, net.hostCount)
        assertEq("192.168.1.255", net.broadcast.toString())
        assertEq(26, IPv4Network.of(IPv4Addr.parse("10.1.2.200")!!, IPv4Addr.parse("255.255.255.192")!!)!!.prefixLength)
        expect(IPv4Network.of(IPv4Addr.parse("10.1.2.200")!!, IPv4Addr.parse("255.0.255.0")!!) == null)
        assertEq(4, IPv6Network.parse("2001:db8::/126")!!.addresses(10).size)
        expect(IPNetwork.parse("10.0.0.0/8")!!.contains(IPAddr.parse("::ffff:10.2.3.4")!!))
    }

    fun testTargetParsing() {
        assertEq(3, TargetSpecification.parse("192.168.1.2-4").hosts().size)
        assertEq(5, TargetSpecification.parse("10.0.0.1-10.0.0.5").hosts().size)
        assertEq(254, TargetSpecification.parse("192.168.7.0/24").hosts().size)
        assertEq(2, TargetSpecification.parse("printer.local, 192.168.1.1 192.168.1.1").components.size)
        expect(TargetSpecification.parse("10.0.0.9/32").components.single() is TargetSpecification.Component.Address)
        assertEq("The targets cover 65534 hosts; the limit is 1024.", assertFails<TargetParseException> { TargetSpecification.parse("10.0.0.0/16") }.message)
        assertFails<TargetParseException> { TargetSpecification.parse("2001:db8::/64") }
        assertFails<TargetParseException> { TargetSpecification.parse("10.0.0.9-3") }
        assertFails<TargetParseException> { TargetSpecification.parse("   ") }
        assertFails<TargetParseException> { TargetSpecification.parse("bad_host!") }
        expect(TargetSpecification.parse("10.0.0.0/30").containsRange)
        expect(Hostname.isLocal("nas.lan"))
        expect(!Hostname.isLocal("example.com"))
    }

    fun testPortParsing() {
        assertEq("22,80-82", PortList.parse("22,80-82").toString())
        assertEq("1,3,5-7", PortList.parse("7 6 5 3 1 1").toString())
        assertEq(20, PortPreset.Quick.ports.count)
        assertEq(100, PortPreset.Common.ports.count)
        assertEq(65535, PortPreset.All.ports.count)
        assertEq("“0” is outside 1–65535.", assertFails<PortParseException> { PortList.parse("0") }.message)
        assertFails<PortParseException> { PortList.parse("90-80") }
        assertFails<PortParseException> { PortList.parse("http") }
        assertEq(PortList.parse("22,443"), PortList.decode("22,443"))
        assertEq("SSH", ServiceCatalog.service(22)?.title)
    }

    fun testAuthorizationPolicy() {
        val local = listOf(IPAddr.parse("192.168.1.5")!!)
        val public = listOf(IPAddr.parse("8.8.8.8")!!)
        assertEq(AuthorizationDecision.Allowed, ScanAuthorizationPolicy().evaluate(local, 20, false, null))
        expect(ScanAuthorizationPolicy().evaluate(public, 1, false, null) is AuthorizationDecision.Denied)
        assertEq(
            AuthorizationDecision.RequiresAttestation("Confirm you own or are authorized to test the internet hosts in this scan."),
            ScanAuthorizationPolicy(allowPublicTargets = true).evaluate(public, 1, false, null),
        )
        assertEq(AuthorizationDecision.Allowed, ScanAuthorizationPolicy(allowPublicTargets = true).evaluate(public, 1, false, AuthorizationAttestation()))
        expect(ScanAuthorizationPolicy(allowPublicTargets = true).evaluate(public, 1, true, AuthorizationAttestation()) is AuthorizationDecision.Denied)
        assertEq(AuthorizationDecision.Allowed, ScanAuthorizationPolicy(authorizedNetworks = listOf(IPNetwork.parse("8.8.8.0/24")!!)).evaluate(public, 1, true, null))
        expect(ScanAuthorizationPolicy().evaluate(listOf(IPAddr.parse("224.0.0.251")!!), 1, false, null) is AuthorizationDecision.Denied)
        expect(ScanAuthorizationPolicy().evaluate(listOf(IPAddr.parse("0.0.0.0")!!), 1, false, null) is AuthorizationDecision.Denied)
        expect(ScanAuthorizationPolicy().evaluate(local, 70_001, false, null) is AuthorizationDecision.Denied)
        val five = (1..5).map { IPAddr.parse("1.1.1.$it")!! }
        expect(ScanAuthorizationPolicy(allowPublicTargets = true).evaluate(five, 1, false, AuthorizationAttestation()) is AuthorizationDecision.Denied)
    }

    fun testLocalNetworkSuggestion() {
        fun entry(name: String, address: String, mask: String) = InterfaceAddress(name, IPv4Addr.parse(address)!!, IPv4Addr.parse(mask)!!)
        assertEq("192.168.7.0/24", LocalNetwork.suggested(listOf(entry("rmnet_data0", "10.40.2.9", "255.255.255.255"), entry("wlan0", "192.168.7.33", "255.255.0.0"))).toString())
        assertEq("10.1.2.192/26", LocalNetwork.suggested(listOf(entry("eth0", "10.1.2.200", "255.255.255.192"))).toString())
        expect(LocalNetwork.suggested(listOf(entry("wlan0", "8.8.8.8", "255.255.255.0"), entry("wlan1", "169.254.3.4", "255.255.0.0"))) == null)
        expect(LocalNetwork.suggested(listOf(entry("tun0", "100.64.0.7", "255.192.0.0"))) == null)
    }

    fun testDnsQueryAndParse() {
        val query = DnsWire.query(0x1234, "example.com", DnsRecordType.A)
        assertEq(0x12, query[0].toInt())
        assertEq(29, query.size)
        val reply = dnsAnswer(query) { body ->
            body.record(1, byteArrayOf(93.toByte(), 184.toByte(), 216.toByte(), 34))
            body.record(5, byteArrayOf(3, 'w'.code.toByte(), 'w'.code.toByte(), 'w'.code.toByte(), 0xC0.toByte(), 12))
            body.record(15, byteArrayOf(0, 10, 0xC0.toByte(), 12))
            body.record(16, byteArrayOf(5) + "hello".toByteArray() + byteArrayOf(2) + "hi".toByteArray())
            4
        }
        val message = DnsWire.parse(reply, 0x1234)
        assertEq(listOf("93.184.216.34", "www.example.com", "10 example.com", "hello hi"), message.answers.map { it.value })
        assertEq(3600L, message.answers.first().ttl)
        assertFails<ConnectionFailure> { DnsWire.parse(reply, 0x9999) }
        assertFails<ConnectionFailure> { DnsWire.parse(reply.copyOf(20), 0x1234) }
        val looping = byteArrayOf(0xC0.toByte(), 0)
        assertFails<ConnectionFailure> { DnsWire.readName(looping, intArrayOf(0)) }
        assertEq("Name does not exist", DnsWire.responseError(3))
    }

    fun testDnsResolverOverFakeSocket() {
        val resolver = DnsResolver(connector = { _, _ ->
            FakeDatagram { query ->
                val ipv6 = query[query.size - 3].toInt() == 28
                dnsAnswer(query) { body ->
                    if (ipv6) body.record(28, ByteArray(16).also { it[15] = 1; it[0] = 0x20; it[1] = 0x01; it[2] = 0x0D; it[3] = 0xB8.toByte() }) else body.record(1, byteArrayOf(10, 0, 0, 7))
                    1
                }
            }
        })
        val lookup = resolver.lookup("nas.example.", DnsRecordType.A)
        assertEq("nas.example", lookup.name)
        assertEq("10.0.0.7", lookup.answers.single().value)
        assertEq(listOf("10.0.0.7", "2001:db8::1"), resolver.resolve("nas.example", 1000).addresses.map { it.toString() })
        assertEq("1.2.0.192.in-addr.arpa", IPAddr.parse("192.0.2.1")!!.reversePointerName)
    }

    fun testServiceIdentification() {
        val inspector = ServiceInspector()
        val ssh = inspector.identify("SSH-2.0-OpenSSH_9.0\r\n".toByteArray(), 22)!!
        assertEq(EvidenceLevel.Confirmed, ssh.evidence)
        assertEq("ssh", ssh.name)
        assertEq("OpenSSH_9.0", ssh.product)
        val http = inspector.identify("HTTP/1.1 200 OK\r\nServer: x\r\n".toByteArray(), 8080)!!
        assertEq("The service returned HTTP/1.1 200 OK.", http.basis)
        assertEq("ftp", inspector.identify("220 ProFTPD FTP server ready\r\n".toByteArray(), 21)?.name)
        assertEq("smtp", inspector.identify("220 mail ESMTP Postfix\r\n".toByteArray(), 25)?.name)
        assertEq(EvidenceLevel.Inferred, inspector.identify(ByteArray(0), 3306)?.evidence)
        expect(inspector.identify(ByteArray(0), 40123) == null)
    }

    fun testScannerWithFakes() {
        val connector = FakeConnector(mapOf(22 to null, 80 to ConnectionFailure.TimedOut, 443 to ConnectionFailure.HostUnreachable), "SSH-2.0-dropbear\r\n".toByteArray())
        val updates = mutableListOf<ScanProgress>()
        val session = PortScanner(connector).run(TargetSpecification.parse("192.168.1.2-3"), PortList(listOf(22, 23, 80, 443)), progress = { updates += it })
        assertEq(8, updates.size)
        assertEq(8, updates.last().completed)
        assertEq(SessionStatus.Completed, session.status)
        assertEq(2, session.counts.open)
        assertEq(2, session.counts.closed)
        assertEq(2, session.counts.timedOut)
        assertEq(2, session.counts.unreachable)
        assertEq(6, session.results.size)
        assertEq(listOf("192.168.1.2#22", "192.168.1.3#22"), session.openResults.map { it.id })
        assertEq(EvidenceLevel.Confirmed, session.openResults.first().service?.evidence)
        val all = PortScanner(connector).run(TargetSpecification.parse("10.0.0.1"), PortList(listOf(23)), ScanOptions(recordClosedPorts = true))
        assertEq(PortState.Closed, all.results.single().state)
    }

    fun testScannerRefusesBeforeConnecting() {
        val connector = FakeConnector(mapOf(22 to null))
        val denied = assertFails<ConnectionFailure.Denied> { PortScanner(connector).run(TargetSpecification.parse("8.8.8.8"), PortList(listOf(22))) }
        expect("Allow internet targets" in denied.detail)
        assertFails<ConnectionFailure.AttestationRequired> {
            PortScanner(connector).run(TargetSpecification.parse("8.8.8.8"), PortList(listOf(22)), authorization = ScanAuthorizationPolicy(allowPublicTargets = true))
        }
        assertEq(0, connector.attempts.get())
        val allowed = PortScanner(connector).run(TargetSpecification.parse("8.8.8.8"), PortList(listOf(22)), ScanOptions(inspectServices = false), ScanAuthorizationPolicy(allowPublicTargets = true), AuthorizationAttestation())
        assertEq(1, allowed.counts.open)
    }

    fun testScannerCancels() {
        val cancel = NexusCancel()
        val slow = StreamConnector { _, _ ->
            Thread.sleep(20)
            throw ConnectionFailure.Refused
        }
        var seen = 0
        val session = PortScanner(slow).run(TargetSpecification.parse("127.0.0.1"), PortPreset.WellKnown.ports, ScanOptions(maximumConcurrency = 4), cancel = cancel, progress = {
            seen++
            if (seen == 10) cancel.cancel()
        })
        assertEq(SessionStatus.Cancelled, session.status)
        expect(session.counts.total < 1024, "stopped early")
    }

    fun testScannerAgainstLoopbackSocket() {
        val server = ServerSocket(0, 10, InetAddress.getByName("127.0.0.1"))
        val listener = thread(isDaemon = true) {
            while (!server.isClosed) {
                val socket = runCatching { server.accept() }.getOrNull() ?: break
                runCatching { socket.use { it.getOutputStream().write("SSH-2.0-NexusTest\r\n".toByteArray()) } }
            }
        }
        try {
            val session = PortScanner().run(TargetSpecification.parse("127.0.0.1"), PortList(listOf(server.localPort)), ScanOptions(timeoutSeconds = 2.0))
            val open = session.openResults.single()
            assertEq(server.localPort, open.port)
            assertEq("ssh", open.service?.name)
            expect((open.latencyMilliseconds ?: -1.0) >= 0.0)
            val reach = ConnectivityToolkit().reachability("127.0.0.1", server.localPort)
            expect(reach.reachable)
        } finally {
            server.close()
            listener.join(1000)
        }
    }

    fun testFailureMapping() {
        assertEq(ConnectionFailure.Refused, NexusFailures.map(java.net.ConnectException("failed to connect to /10.0.0.1 (port 81): connect failed: ECONNREFUSED (Connection refused)")))
        assertEq(ConnectionFailure.HostUnreachable, NexusFailures.map(java.net.NoRouteToHostException("No route to host")))
        assertEq(ConnectionFailure.NetworkUnreachable, NexusFailures.map(java.net.ConnectException("connect failed: ENETUNREACH (Network is unreachable)")))
        assertEq(ConnectionFailure.TimedOut, NexusFailures.map(java.net.SocketTimeoutException("connect timed out")))
        assertEq(ConnectionFailure.PermissionDenied, NexusFailures.map(java.net.SocketException("socket failed: EPERM (Operation not permitted)")))
        assertEq(PortState.Closed, ConnectionFailure.Refused.portState)
        assertEq(PortState.Indeterminate, ConnectionFailure.ResetByPeer.portState)
    }

    fun testInventoryAndChanges() {
        val inventory = DeviceInventory()
        val printer = NetService("Printer", "_ipp._tcp", hostName = "printer.local", addresses = listOf(IPAddr.parse("192.168.1.20")!!))
        inventory.merge(printer)
        inventory.merge(NetService("Printer web", "_http._tcp", hostName = "printer.local", addresses = listOf(IPAddr.parse("192.168.1.20")!!)))
        inventory.merge(NetService("Living Room", "_googlecast._tcp", addresses = listOf(IPAddr.parse("192.168.1.30")!!)))
        assertEq(2, inventory.devices.size)
        assertEq(2, inventory.devices.first().services.size)
        assertEq(DeviceKind.Printer, inventory.devices.first().kind)
        assertEq(DeviceKind.Tv, inventory.devices.last().kind)
        assertEq("ip:192.168.1.20", inventory.devices.first().id)
        val before = inventory.devices
        val next = DeviceInventory()
        next.merge(printer)
        next.merge(NetService("NAS", "_smb._tcp", addresses = listOf(IPAddr.parse("192.168.1.40")!!)))
        val changes = next.changes(before)
        assertEq(setOf(ChangeKind.DeviceAppeared, ChangeKind.DeviceDisappeared, ChangeKind.ServiceRemoved), changes.map { it.kind }.toSet())
        expect(next.devices.first().searchText.contains("printer.local"))
    }

    fun testDiscoveryEngine() {
        val engine = DiscoveryEngine { type, _, _ -> if (type == "_ssh._tcp") listOf(NetService("mac", type, addresses = listOf(IPAddr.parse("10.0.0.2")!!))) else emptyList() }
        val steps = mutableListOf<Int>()
        val session = engine.discover(DiscoveryOptions(listOf("_http._tcp", "_ssh._tcp")), progress = { steps += it.completedServiceTypes })
        assertEq(listOf(1, 2), steps)
        assertEq(SessionStatus.Completed, session.status)
        assertEq(DeviceKind.Computer, session.devices.single().kind)
        val cancel = NexusCancel().also { it.cancel() }
        assertEq(SessionStatus.Cancelled, engine.discover(cancel = cancel).status)
    }

    fun testSecurityInsights() {
        val https = SecurityInsights.http("https://example.com", 200, emptyList())
        expect(https.any { it.title == "No HSTS header observed" })
        expect(SecurityInsights.http("http://example.com", 503, emptyList()).count { it.severity == SecuritySeverity.Attention } == 2)
        val hardened = SecurityInsights.http("https://example.com", 200, listOf(HttpHeader("Strict-Transport-Security", "max-age=1"), HttpHeader("Content-Security-Policy", "x"), HttpHeader("X-Content-Type-Options", "nosniff")))
        assertEq(0, hardened.size)
        val previous = ScanSession("10.0.0.1", PortList(listOf(22, 80)), results = listOf(PortResult("10.0.0.1", "10.0.0.1", 22, PortState.Open)))
        val current = previous.copy(results = previous.results + PortResult("10.0.0.1", "10.0.0.1", 80, PortState.Open))
        assertEq(listOf("10.0.0.1#80 was not open in the previous scan."), SecurityInsights.portChanges(current, previous).map { it.detail })
        val now = 1_000L * CertificateSummary.DAY_MS
        val leaf = CertificateSummary("CN=a", "a", "CN=b", "b", "01", 3, 0, now + 5 * CertificateSummary.DAY_MS, emptyList(), "EC", 256, "SHA256withECDSA", false, false, "AA")
        val tls = SecurityInsights.tls(TlsObservation("a", 443, "a", "TLSv1.1", chain = listOf(leaf), trust = TrustOutcome.Trusted), now)
        assertEq(listOf("Certificate expires soon", "Old TLS version"), tls.map { it.title })
    }

    fun testCertificateNames() {
        assertEq("example.com", Certificates.commonName("CN=example.com,O=Example\\, Inc.,C=US"))
        assertEq("R3", Certificates.commonName("C=US,O=Let's Encrypt,CN=R3"))
        expect(Certificates.commonName("O=Nobody") == null)
        assertEq("0A:FF", Certificates.colonHex(byteArrayOf(10, -1)))
        assertEq("the certificate has expired", Certificates.reason(javax.net.ssl.SSLHandshakeException("x").apply { initCause(java.security.cert.CertificateExpiredException("NotAfter: expired")) }))
    }

    fun testHttpInspectorRejectsOtherSchemes() {
        assertFails<ConnectionFailure.Unsupported> { HttpInspector().inspect("ftp://example.com") }
        assertFails<ConnectionFailure.Unsupported> { HttpInspector().inspect("not a url") }
    }

    fun testHttpInspectorFollowsRedirects() {
        val server = ServerSocket(0, 10, InetAddress.getByName("127.0.0.1"))
        val port = server.localPort
        val listener = thread(isDaemon = true) {
            while (!server.isClosed) {
                val accepted = runCatching { server.accept() }.getOrNull() ?: break
                runCatching {
                    accepted.use { socket ->
                        val request = socket.getInputStream().bufferedReader().readLine().orEmpty()
                        val reply = if (" /start " in request) {
                            "HTTP/1.1 301 Moved\r\nLocation: http://127.0.0.1:$port/end\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"
                        } else {
                            "HTTP/1.1 200 OK\r\nX-Content-Type-Options: nosniff\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"
                        }
                        socket.getOutputStream().write(reply.toByteArray())
                    }
                }
            }
        }
        try {
            val result = HttpInspector().inspect("http://127.0.0.1:$port/start", 5_000)
            assertEq(200, result.statusCode)
            assertEq(listOf("http://127.0.0.1:$port/end"), result.redirects)
            assertEq("http://127.0.0.1:$port/end", result.finalUrl)
            expect(result.headers.any { it.name == "X-Content-Type-Options" })
            expect(result.securityObservations.any { it.title == "Unencrypted HTTP" })
            expect(result.tls == null)
        } finally {
            server.close()
            listener.join(1000)
        }
    }

    fun testHistoryAndReports() {
        val result = PortResult("nas", "10.0.0.2", 22, PortState.Open, 1.5, service = ServiceIdentity("ssh", "SSH", EvidenceLevel.Confirmed, "greeting", product = "Open\"SSH"))
        val failed = PortResult("nas", "10.0.0.2", 80, PortState.TimedOut, failure = ConnectionFailure.TimedOut)
        val scan = ScanSession(
            "nas", PortList(listOf(22, 80)), resolvedHosts = listOf(ResolvedHost("nas", listOf(IPAddr.parse("10.0.0.2")!!))),
            status = SessionStatus.Failed("boom"), results = listOf(result, failed), counts = PortStateCounts(open = 1, timedOut = 1),
            attestation = AuthorizationAttestation(confirmedAt = 1_700_000_000_000L), finishedAt = 1_700_000_001_000L, startedAt = 1_700_000_000_000L,
        )
        val discovery = DiscoverySession(devices = listOf(Device("ip:10.0.0.2", "nas", listOf(IPAddr.parse("10.0.0.2")!!), services = listOf(NetService("nas", "_smb._tcp", port = 445)))), status = SessionStatus.Completed)
        val restored = NexusHistory.decode(NexusHistory.encode(NexusHistory.Snapshot(listOf(scan), listOf(discovery), discovery.devices)))
        assertEq(scan.toJson().minus("results"), restored.scans.single().toJson().minus("results"))
        assertEq(ConnectionFailure.TimedOut, restored.scans.single().results.last().failure)
        assertEq("Open\"SSH", restored.scans.single().results.first().service?.product)
        assertEq(445, restored.discoveries.single().devices.single().services.single().port)
        assertEq(1.0, restored.scans.single().durationSeconds)
        val csv = NexusReports.csv(scan).lines()
        assertEq("host,address,port,state,latency_ms,service,evidence", csv[0])
        assertEq("\"nas\",\"10.0.0.2\",\"22\",\"open\",\"1.50\",\"ssh\",\"confirmed\"", csv[1])
        val json = Json.parseObject(NexusReports.scanJson(scan))
        assertEq("nas", json["target"])
        assertEq("22,80", json["ports"])
        assertEq("nexus-nas-20231114T221320.csv", NexusReports.fileName(scan, "csv"))
        expect(NexusHistory.decode("not json").scans.isEmpty())
    }

    fun testClientFacade() {
        val client = NexusClient(streamConnector = FakeConnector(mapOf(443 to null)), datagramConnector = { _, _ -> FakeDatagram { dnsAnswer(it) { body -> body.record(12, byteArrayOf(3) + "nas".toByteArray() + byteArrayOf(0)); 1 } } })
        expect(!client.configuration.authorization.allowPublicTargets)
        assertEq(1, client.scan("10.0.0.5", PortList(listOf(443)), ScanOptions(inspectServices = false)).counts.open)
        assertFails<PortParseException> { client.scan("10.0.0.5", PortList(emptyList())) }
        assertEq("nas", client.dnsLookup("10.0.0.5", DnsRecordType.PTR).answers.single().value)
        assertEq("5.0.0.10.in-addr.arpa", client.dnsLookup("10.0.0.5", DnsRecordType.PTR).name)
        expect(client.discover(DiscoveryOptions(durationMs = 1)).status is SessionStatus.Failed)
        expect("Android" in client.connectivity.tracerouteAvailability())
    }

    fun testRemoteProfilesMustBeApproved() {
        val client = RemoteScannerClient(RemoteScannerConfiguration("https://scanner.example", "t", "lab", listOf("safe")))
        assertFails<ConnectionFailure.Unsupported> { client.submit(RemoteScanRequest("10.0.0.1", PortList(listOf(22)), "syn", AuthorizationAttestation())) }
    }
}
