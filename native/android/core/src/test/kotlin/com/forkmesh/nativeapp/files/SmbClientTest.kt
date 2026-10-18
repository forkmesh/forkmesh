package com.forkmesh.nativeapp.files

import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.assertFails
import com.forkmesh.nativeapp.testing.assertHex
import com.forkmesh.nativeapp.testing.bytesOfHex
import com.forkmesh.nativeapp.testing.expect
import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import java.io.DataInputStream
import java.net.InetAddress
import java.net.ServerSocket
import kotlin.concurrent.thread

class SmbClientTest {
    private fun client(server: FakeSmbServer, user: String = "alice", password: String = "secret", guest: Boolean = false, share: String = "media"): SmbClient {
        val credentials = if (guest) SmbCredentials.guest else SmbCredentials.parse(user, password)
        return SmbClient("192.168.1.1", share, credentials, guest) { server }
    }

    fun testMd4Vectors() {
        assertHex("31d6cfe0d16ae931b73c59d7e0c089c0", Md4.digest(ByteArray(0)))
        assertHex("a448017aaf21d8525fc10ae87aa6729d", Md4.digest("abc".toByteArray()))
        assertHex("d9130a8164549fe818874806e1c7014b", Md4.digest("message digest".toByteArray()))
        assertHex("e33b4ddc9c38f2199c3e7b164fcc0536", Md4.digest("12345678901234567890123456789012345678901234567890123456789012345678901234567890".toByteArray()))
    }

    fun testNtlmV2MatchesMsNlmpVectors() {
        assertHex("a4f49c406510bdcab6824ee7c30fd852", Ntlm.ntowfV1("Password"))
        val key = Ntlm.ntowfV2("Password", "User", "Domain")
        assertHex("0c868a403bfd7a93a3001ef22ef02e3f", key)
        val info = bytesOfHex("02000c0044006f006d00610069006e0001000c0053006500720076006500720000000000")
        val (response, sessionKey) = Ntlm.ntlmV2Response(key, bytesOfHex("0123456789abcdef"), bytesOfHex("aaaaaaaaaaaaaaaa"), ByteArray(8), info)
        assertHex("68cd0ab851e51c96aabc927bebef6a1c", response.copyOf(16))
        assertHex("8de40ccadbc14a82f15cb0ad0de95ca3", sessionKey)
    }

    fun testSpnegoWrapsNtlmAndChallengeIsFoundBySignature() {
        val negotiate = Ntlm.negotiate(false)
        val token = Spnego.initToken(negotiate)
        assertEq(0x60, token[0].toInt() and 0xff)
        assertEq(Ntlm.findMessage(token) + negotiate.size, token.size)
        val long = Spnego.responseToken(ByteArray(300))
        assertEq(0xa1, long[0].toInt() and 0xff)
        assertEq(0x82, long[1].toInt() and 0xff)
    }

    fun testCredentialsSplitDomain() {
        assertEq(SmbCredentials("bob", "pw", "WORK"), SmbCredentials.parse("WORK\\bob", "pw"))
        assertEq(SmbCredentials("bob", "pw"), SmbCredentials.parse(" bob ", "pw"))
    }

    fun testListReadWriteRenameDelete() {
        val server = FakeSmbServer().file("Movies/a.txt", "hello").file("notes.md", "# hi").folder("Empty")
        val share = client(server)
        val root = share.run { it.list("") }
        assertEq(listOf("Empty", "Movies", "notes.md"), root.map { it.name }.sorted())
        expect(root.first { it.name == "Movies" }.isFolder)
        assertEq(4L, root.first { it.name == "notes.md" }.size)
        expect(root.first { it.name == "notes.md" }.modified > 1_500_000_000_000L)

        val big = ByteArray(10_000) { (it % 251).toByte() }
        val written = share.run { it.writeFrom("Movies/big.bin", ByteArrayInputStream(big), big.size.toLong()) }
        assertEq(big.size.toLong(), written)
        expect(big.contentEquals(server.files["Movies/big.bin"]))
        val out = ByteArrayOutputStream()
        share.run { it.readTo("Movies/big.bin", out) }
        expect(big.contentEquals(out.toByteArray()))

        share.run { it.mkdir("Movies/New") }
        expect("Movies/New" in server.folders)
        share.run { it.rename("Movies/a.txt", "Movies/New/b.txt") }
        assertEq("hello", String(server.files["Movies/New/b.txt"]!!))
        expect("Movies/a.txt" !in server.files)

        share.run { it.deleteOne("notes.md", false) }
        expect("notes.md" !in server.files)
        val notEmpty = assertFails<SmbException> { share.run { it.deleteOne("Movies", true) } }
        assertEq(SmbStatus.DIRECTORY_NOT_EMPTY, notEmpty.status)
        assertEq("The folder is not empty.", notEmpty.message)
        share.run { it.delete("Movies") }
        expect("Movies" !in server.folders)
        expect(server.files.keys.none { it.startsWith("Movies") })
    }

    fun testReadLimitAndCancel() {
        val server = FakeSmbServer().file("big.bin", ByteArray(20_000))
        val share = client(server)
        val tooLarge = assertFails<SmbException> { share.run { it.readTo("big.bin", ByteArrayOutputStream(), 1000) } }
        assertEq("The file is too large to open here. Download it instead.", tooLarge.message)
        val cancelled = assertFails<SmbException> { share.run { it.readTo("big.bin", ByteArrayOutputStream()) { done, _ -> done < 4096 } } }
        assertEq(SmbStatus.CANCELLED, cancelled.status)
    }

    fun testWrongPasswordAndMissingShare() {
        val wrong = assertFails<SmbException> { client(FakeSmbServer(), password = "nope").connect() }
        assertEq("The user name or password is wrong.", wrong.message)
        val missing = assertFails<SmbException> { client(FakeSmbServer(), share = "films").connect() }
        assertEq("There is no share named films on 192.168.1.1.", missing.message)
        val gone = assertFails<SmbException> { client(FakeSmbServer()).run { it.stat("nothing.txt") } }
        assertEq("That file or folder no longer exists.", gone.message)
        val clash = assertFails<SmbException> { client(FakeSmbServer().folder("A")).run { it.mkdir("A") } }
        assertEq("Something with that name is already there.", clash.message)
    }

    fun testDialectRefused() {
        val problem = assertFails<SmbException> { client(FakeSmbServer(dialect = 0x0311)).connect() }
        assertEq("192.168.1.1 does not speak SMB 2.1.", problem.message)
    }

    fun testGuestThenAnonymousFallback() {
        val guestServer = FakeSmbServer(allowGuest = true).file("a.txt", "x")
        val guest = client(guestServer, guest = true)
        assertEq(1, guest.run { it.list("") }.size)
        expect(guest.guest)
        assertEq("guest", guestServer.lastUser)

        val anonymousServer = FakeSmbServer(allowAnonymous = true).file("a.txt", "x")
        val anonymous = client(anonymousServer, guest = true)
        assertEq(1, anonymous.run { it.list("") }.size)
        assertEq("", anonymousServer.lastUser)
    }

    fun testSigningWhenServerRequiresIt() {
        val server = FakeSmbServer(signingRequired = true).file("a.txt", "signed")
        val share = client(server)
        val out = ByteArrayOutputStream()
        share.run { it.readTo("a.txt", out) }
        assertEq("signed", out.toString())
        expect(server.badSignatures.isEmpty())
    }

    fun testReconnectsOnceAfterDroppedConnection() {
        val servers = mutableListOf<FakeSmbServer>()
        val share = SmbClient("192.168.1.1", "media", SmbCredentials("alice", "secret")) {
            FakeSmbServer().file("a.txt", "x").also { servers += it }
        }
        share.run { it.list("") }
        servers.last().dropNext = true
        assertEq(1, share.run { it.list("") }.size)
        assertEq(2, servers.size)
        expect(servers.first().closed)
    }

    fun testShareEnumerationListsDiskShares() {
        val server = FakeSmbServer()
        assertEq(listOf("backup", "media"), SmbClient.listShares("192.168.1.1", SmbCredentials("alice", "secret"), false, server))
    }

    fun testSrvSvcUuidByteOrder() {
        assertHex("c84f324b7016d30112785a47bf6ee188", SrvSvc.uuid("4b324fc8-1670-01d3-1278-5a47bf6ee188"))
    }

    fun testStatusMessages() {
        assertEq("Access denied.", SmbStatus.message(SmbStatus.ACCESS_DENIED))
        assertEq("The file is open somewhere else.", SmbStatus.message(SmbStatus.SHARING_VIOLATION))
        assertEq("The disk is full.", SmbStatus.message(SmbStatus.DISK_FULL))
        assertEq("The share is read only.", SmbStatus.message(SmbStatus.MEDIA_WRITE_PROTECTED))
        assertEq("Could not reach nas. Is file sharing on?", SmbStatus.message(SmbStatus.UNREACHABLE, "nas"))
        assertEq("The file server answered 0xC0000099.", SmbStatus.message(0xC0000099L))
    }

    fun testSocketTransportRefusesInternetHosts() {
        val problem = assertFails<SmbException> { SocketSmbTransport("8.8.8.8") }
        assertEq("Network shares must be on your local network.", problem.message)
    }

    fun testSocketTransportFramesMessages() {
        val server = ServerSocket(0, 1, InetAddress.getByName("127.0.0.1"))
        val fake = FakeSmbServer().file("a.txt", "over tcp")
        val worker = thread {
            server.accept().use { socket ->
                val input = DataInputStream(socket.getInputStream())
                val output = socket.getOutputStream()
                try {
                    while (true) {
                        val header = ByteArray(4)
                        input.readFully(header)
                        val message = ByteArray(SmbFraming.length(header))
                        input.readFully(message)
                        fake.send(message)
                        output.write(SmbFraming.frame(fake.receive()))
                        output.flush()
                    }
                } catch (_: Exception) {
                }
            }
        }
        val share = SmbClient("127.0.0.1", "media", SmbCredentials("alice", "secret")) { SocketSmbTransport("127.0.0.1", server.localPort) }
        val out = ByteArrayOutputStream()
        share.run { it.readTo("a.txt", out) }
        assertEq("over tcp", out.toString())
        share.close()
        server.close()
        worker.join(2_000)
    }
}
