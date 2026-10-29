package com.forkmesh.nativeapp.chat

import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.assertFails
import com.forkmesh.nativeapp.testing.expect
import java.net.InetAddress
import java.net.ServerSocket
import java.net.Socket
import java.net.URI
import javax.net.ssl.SSLSocketFactory
import kotlin.concurrent.thread

class ChatLinkAddressesTest {
    private fun v4(vararg octets: Int) = ByteArray(4) { octets[it].toByte() }

    private fun v6(text: String): ByteArray {
        val raw = InetAddress.getByName(text).address
        if (raw.size == 16) return raw
        return ByteArray(16).also {
            it[10] = 0xff.toByte()
            it[11] = 0xff.toByte()
            raw.copyInto(it, 12)
        }
    }

    fun testPrivateAndSpecialIPv4AddressesAreRefused() {
        expect(ChatLinkAddresses.isPublic(v4(93, 184, 216, 34)))
        expect(ChatLinkAddresses.isPublic(v4(8, 8, 8, 8)))
        for (address in listOf(
            v4(10, 0, 0, 1), v4(127, 0, 0, 1), v4(127, 255, 0, 9), v4(169, 254, 169, 254), v4(192, 168, 1, 1),
            v4(172, 16, 0, 1), v4(172, 31, 255, 255), v4(100, 64, 0, 1), v4(100, 127, 0, 1), v4(0, 0, 0, 0),
            v4(224, 0, 0, 1), v4(239, 255, 255, 250), v4(255, 255, 255, 255), v4(192, 0, 2, 1), v4(198, 18, 0, 1),
            v4(198, 51, 100, 7), v4(203, 0, 113, 9), v4(192, 0, 0, 8),
        )) {
            expect(!ChatLinkAddresses.isPublic(address), address.joinToString(".") { (it.toInt() and 0xff).toString() })
        }
        expect(ChatLinkAddresses.isPublic(v4(172, 32, 0, 1)))
        expect(ChatLinkAddresses.isPublic(v4(100, 128, 0, 1)))
    }

    fun testPrivateAndSpecialIPv6AddressesAreRefused() {
        expect(ChatLinkAddresses.isPublic(v6("2606:4700::1111")))
        for (text in listOf(
            "::1", "::", "fe80::1", "febf::1", "fd00::1", "fc00::1", "ff02::1", "fec0::1", "2001:db8::1", "::ffff:10.0.0.1",
            "::ffff:127.0.0.1", "::ffff:169.254.169.254", "::ffff:100.64.0.1", "64:ff9b::a00:1", "2002:0a00:0001::1", "2001::1",
            "::10.0.0.1", "4000::1",
        )) {
            expect(!ChatLinkAddresses.isPublic(v6(text)), text)
        }
        expect(ChatLinkAddresses.isPublic(v6("::ffff:93.184.216.34")), "an IPv4-mapped public address")
        expect(ChatLinkAddresses.isPublic(v6("64:ff9b::5db8:d822")), "NAT64 of a public address")
        expect(!ChatLinkAddresses.isPublic(byteArrayOf(1, 2, 3)))
    }

    fun testEveryResolvedAddressMustBePublic() {
        expect(!ChatLinkAddresses.allPublic(emptyList()), "a name that does not resolve is refused")
        expect(!ChatLinkAddresses.allPublic(listOf(v4(93, 184, 216, 34), v4(10, 0, 0, 1))), "one private answer is enough to refuse")
        expect(ChatLinkAddresses.allPublic(listOf(v4(93, 184, 216, 34), v6("2606:4700::1111"))))
    }

    fun testTheSystemResolverAnswersLiteralAddressesWithoutDns() {
        assertEq(listOf(127, 0, 0, 1), ChatLinkAddresses.system.resolve("127.0.0.1").single().map { it.toInt() and 0xff })
        expect(!ChatLinkAddresses.allPublic(ChatLinkAddresses.system.resolve("[::1]")))
        expect(!ChatLinkAddresses.allPublic(ChatLinkAddresses.system.resolve("")))
    }

    fun testTheGuardAsksNothingForPrivateLiteralsOrPrivateNames() {
        val http = ChatLinkFakeHttp()
        http.on({ true }) { html("<head><title>Inside</title></head>") }
        var asked = 0
        val guarded = ChatLinkGuardedHttp(http) { host ->
            asked++
            if (host == "public.example.com") listOf(v4(93, 184, 216, 34)) else listOf(v4(10, 0, 0, 5))
        }
        for (raw in listOf("https://10.0.0.1/", "https://[fe80::1]/", "https://localhost/", "https://intranet.example.com/")) {
            assertEq(null, guarded.get(ChatLinkRequest(raw, emptyMap(), 1000, 1000)), raw)
        }
        assertEq(1, asked, "literal and local names are refused before any lookup")
        assertEq(0, http.requests.size)
        expect(guarded.get(ChatLinkRequest("https://public.example.com/", emptyMap(), 1000, 1000))?.ok == true)
    }

    fun testANameThatResolvesPrivatelyIsNeverFetchedNorRedirectedTo() {
        val http = ChatLinkFakeHttp()
        http.on({ true }) { request ->
            if (URI(request.url).host == "jump.example.com") {
                ChatLinkResponse(302, mapOf("location" to "https://rebind.example.com/admin"))
            } else {
                html("<head><meta property=og:title content=\"Inside\"></head>")
            }
        }
        val addresses = mapOf(
            "intranet.example.com" to listOf(v4(10, 0, 0, 5)),
            "rebind.example.com" to listOf(v4(127, 0, 0, 1)),
            "jump.example.com" to listOf(v4(93, 184, 216, 34)),
            "v6local.example.com" to listOf(v6("fe80::1")),
            "ula.example.com" to listOf(v6("fd12:3456::1")),
            "mapped.example.com" to listOf(v6("::ffff:192.168.0.1")),
            "cgnat.example.com" to listOf(v4(100, 100, 1, 1)),
            "mixed.example.com" to listOf(v4(93, 184, 216, 34), v6("::1")),
            "public.example.com" to listOf(v6("2606:4700::1111")),
        )
        val service = ChatLinkPreviewService(http, resolver = { host -> addresses[host].orEmpty() })
        val refused = listOf("intranet", "v6local", "ula", "mapped", "cgnat", "mixed", "unknown")
        for (host in refused) {
            val link = URI("https://$host.example.com/")
            service.load(link)
            assertEq(ChatLinkPreviewState.None, service.state(link), host)
        }
        val jump = URI("https://jump.example.com/go")
        service.load(jump)
        assertEq(ChatLinkPreviewState.None, service.state(jump))
        expect("jump.example.com" in http.hosts())
        val reached = http.hosts().toSet()
        for (host in refused + "rebind") expect("$host.example.com" !in reached, "$host in $reached")
        val fine = URI("https://public.example.com/")
        service.load(fine)
        assertEq("Inside", service.preview(fine)?.title)
        assertEq(null, service.imageData("https://intranet.example.com/a.png"))
    }

    fun testTheSocketFactoryRefusesAConnectionThatLandedOnLoopback() {
        ServerSocket(0, 4, InetAddress.getLoopbackAddress()).use { server ->
            val socket = Socket(InetAddress.getLoopbackAddress(), server.localPort)
            val factory = ChatLinkPublicSocketFactory(SSLSocketFactory.getDefault() as SSLSocketFactory)
            assertFails<ChatLinkRefused> { factory.createSocket(socket, "example.com", 443, true) }
            expect(socket.isClosed, "the refused socket is closed")
            assertFails<ChatLinkRefused> { factory.createSocket(InetAddress.getLoopbackAddress(), server.localPort) }
        }
    }

    fun testTheUrlConnectionClientNeverSpeaksToALoopbackServer() {
        ServerSocket(0, 4, InetAddress.getLoopbackAddress()).use { server ->
            server.soTimeout = 5_000
            var received = -2
            val listener = thread {
                runCatching { server.accept().use { received = it.getInputStream().read() } }
            }
            val response = ChatLinkUrlConnectionHttp().get(
                ChatLinkRequest("https://127.0.0.1:${server.localPort}/", emptyMap(), 3_000, 1024),
            )
            listener.join(6_000)
            assertEq(null, response)
            expect(received == -1 || received == -2, "the server got no TLS hello: $received")
        }
    }
}
