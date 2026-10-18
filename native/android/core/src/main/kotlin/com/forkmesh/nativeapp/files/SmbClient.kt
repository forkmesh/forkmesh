package com.forkmesh.nativeapp.files

import com.forkmesh.nativeapp.openwrt.OpenWrtModel
import java.io.Closeable
import java.io.EOFException
import java.io.IOException
import java.io.InputStream
import java.io.OutputStream
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.Socket
import java.security.SecureRandom

interface SmbTransport : Closeable {
    fun send(message: ByteArray)

    fun receive(): ByteArray
}

class SocketSmbTransport(host: String, port: Int = 445, connectTimeoutMs: Int = 8_000, readTimeoutMs: Int = 30_000) : SmbTransport {
    private val socket = Socket()
    private val input: InputStream
    private val output: OutputStream

    init {
        val addresses = try {
            InetAddress.getAllByName(host).toList()
        } catch (_: IOException) {
            throw SmbException(SmbStatus.UNREACHABLE, SmbStatus.message(SmbStatus.UNREACHABLE, host))
        }
        if (addresses.isEmpty() || !addresses.all { OpenWrtModel.isLocalHost(it.hostAddress ?: "") }) {
            throw SmbException(SmbStatus.NOT_LOCAL, SmbStatus.message(SmbStatus.NOT_LOCAL))
        }
        try {
            socket.connect(InetSocketAddress(addresses.first(), port), connectTimeoutMs)
            socket.soTimeout = readTimeoutMs
            socket.tcpNoDelay = true
        } catch (_: IOException) {
            runCatching { socket.close() }
            throw SmbException(SmbStatus.UNREACHABLE, SmbStatus.message(SmbStatus.UNREACHABLE, host))
        }
        input = socket.getInputStream().buffered()
        output = socket.getOutputStream().buffered()
    }

    override fun send(message: ByteArray) {
        output.write(SmbFraming.frame(message))
        output.flush()
    }

    override fun receive(): ByteArray {
        val header = readFully(4)
        return readFully(SmbFraming.length(header))
    }

    private fun readFully(count: Int): ByteArray {
        val buffer = ByteArray(count)
        var read = 0
        while (read < count) {
            val n = input.read(buffer, read, count - read)
            if (n < 0) throw EOFException("connection closed")
            read += n
        }
        return buffer
    }

    override fun close() {
        runCatching { socket.close() }
    }
}

data class SmbEntry(
    val name: String,
    val isFolder: Boolean,
    val size: Long,
    val modified: Long,
    val attributes: Long,
) {
    val hidden: Boolean get() = (attributes and 0x2L) != 0L || name.startsWith(".")
}

class SmbResponse(val data: ByteArray) {
    val reader = ByteReader(data)
    val status: Long get() = reader.u32(8)
    val command: Int get() = reader.u16(12)
    val flags: Long get() = reader.u32(16)
    val messageId: Long get() = reader.u64(24)
    val treeId: Long get() = reader.u32(36)
    val sessionId: Long get() = reader.u64(40)
}

object Smb2 {
    const val NEGOTIATE = 0
    const val SESSION_SETUP = 1
    const val LOGOFF = 2
    const val TREE_CONNECT = 3
    const val TREE_DISCONNECT = 4
    const val CREATE = 5
    const val CLOSE = 6
    const val READ = 8
    const val WRITE = 9
    const val QUERY_DIRECTORY = 14
    const val SET_INFO = 17

    const val FLAG_RESPONSE = 0x1L
    const val FLAG_ASYNC = 0x2L
    const val FLAG_SIGNED = 0x8L

    const val DIALECT_202 = 0x0202
    const val DIALECT_210 = 0x0210

    const val CHUNK = 65_536
    const val HEADER = 64

    const val FILE_OPEN = 1
    const val FILE_CREATE = 2
    const val FILE_OVERWRITE_IF = 5
    const val DIRECTORY_FILE = 0x1
    const val NON_DIRECTORY_FILE = 0x40

    fun fromFileTime(value: Long): Long = if (value <= 0L) 0L else value / 10_000L - 11_644_473_600_000L

    fun wirePath(path: String): String = path.split('/').filter { it.isNotEmpty() }.joinToString("\\")
}

class SmbSession(
    private val transport: SmbTransport,
    val host: String,
    private val random: SecureRandom = SecureRandom(),
    private val clock: () -> Long = System::currentTimeMillis,
) : Closeable {
    private var messageId = 0L
    var sessionId = 0L
        private set
    var dialect = 0
        private set
    private var signingRequired = false
    private var signing = false
    private var sessionKey = ByteArray(16)
    var maxRead = Smb2.CHUNK
        private set
    var maxWrite = Smb2.CHUNK
        private set
    var guest = false
        private set

    fun request(command: Int, body: ByteArray, treeId: Long = 0L): SmbResponse {
        val id = messageId++
        val header = ByteWriter()
            .bytes(byteArrayOf(0xFE.toByte(), 'S'.code.toByte(), 'M'.code.toByte(), 'B'.code.toByte()))
            .u16(64)
            .u16(if (dialect == Smb2.DIALECT_210 && command != Smb2.NEGOTIATE) 1 else 0)
            .u32(0)
            .u16(command)
            .u16(64)
            .u32(if (signing) Smb2.FLAG_SIGNED else 0L)
            .u32(0)
            .u64(id)
            .u32(0xFEFF)
            .u32(treeId)
            .u64(sessionId)
            .zeros(16)
            .toByteArray()
        val message = header + body
        if (signing) Ntlm.hmacSha256(sessionKey, message).copyInto(message, 48, 0, 16)
        try {
            transport.send(message)
            while (true) {
                val response = SmbResponse(transport.receive())
                if (response.data.size < Smb2.HEADER || response.reader.u32(0) != 0x424D53FEL) {
                    throw SmbException(SmbStatus.PROTOCOL_ERROR, SmbStatus.message(SmbStatus.PROTOCOL_ERROR))
                }
                if (response.messageId != id) continue
                if (response.status == SmbStatus.PENDING && (response.flags and Smb2.FLAG_ASYNC) != 0L) continue
                return response
            }
        } catch (problem: SmbException) {
            throw problem
        } catch (_: IOException) {
            throw SmbException(SmbStatus.UNREACHABLE, SmbStatus.message(SmbStatus.UNREACHABLE, host))
        }
    }

    fun fail(response: SmbResponse, share: String = ""): Nothing =
        throw SmbException(response.status, SmbStatus.message(response.status, host, share))

    fun negotiate() {
        val guid = ByteArray(16).also(random::nextBytes)
        val body = ByteWriter().u16(36).u16(2).u16(1).u16(0).u32(0).bytes(guid).u64(0)
            .u16(Smb2.DIALECT_202).u16(Smb2.DIALECT_210).toByteArray()
        val response = request(Smb2.NEGOTIATE, body)
        if (response.status != SmbStatus.SUCCESS) {
            if (response.status == SmbStatus.NOT_SUPPORTED) throw SmbException(SmbStatus.NO_DIALECT, SmbStatus.message(SmbStatus.NO_DIALECT, host))
            fail(response)
        }
        val r = response.reader
        val chosen = r.u16(68)
        if (chosen != Smb2.DIALECT_202 && chosen != Smb2.DIALECT_210) throw SmbException(SmbStatus.NO_DIALECT, SmbStatus.message(SmbStatus.NO_DIALECT, host))
        dialect = chosen
        signingRequired = (r.u16(66) and 0x2) != 0
        maxRead = minOf(r.u32(96).toInt().takeIf { it > 0 } ?: Smb2.CHUNK, Smb2.CHUNK)
        maxWrite = minOf(r.u32(100).toInt().takeIf { it > 0 } ?: Smb2.CHUNK, Smb2.CHUNK)
    }

    private fun sessionSetup(token: ByteArray): SmbResponse {
        val body = ByteWriter().u16(25).u8(0).u8(1).u32(0).u32(0).u16(Smb2.HEADER + 24).u16(token.size).u64(0)
            .bytes(token).toByteArray()
        return request(Smb2.SESSION_SETUP, body)
    }

    fun authenticate(credentials: SmbCredentials) {
        sessionId = 0L
        signing = false
        val first = sessionSetup(Spnego.initToken(Ntlm.negotiate(credentials.anonymous)))
        if (first.status != SmbStatus.MORE_PROCESSING_REQUIRED) fail(first)
        sessionId = first.sessionId
        val buffer = securityBuffer(first)
        val challenge = Ntlm.parseChallenge(buffer)
        val clientChallenge = ByteArray(8).also(random::nextBytes)
        val auth = Ntlm.authenticate(challenge, credentials, clientChallenge, clock())
        val second = sessionSetup(Spnego.responseToken(auth.message))
        if (second.status != SmbStatus.SUCCESS) fail(second)
        val sessionFlags = second.reader.u16(Smb2.HEADER + 2)
        guest = (sessionFlags and 0x3) != 0 || credentials.anonymous
        sessionKey = auth.sessionKey
        signing = signingRequired && !guest
    }

    private fun securityBuffer(response: SmbResponse): ByteArray {
        val r = response.reader
        val offset = r.u16(Smb2.HEADER + 4)
        val length = r.u16(Smb2.HEADER + 6)
        return r.bytes(offset, length)
    }

    fun treeConnect(share: String): Long {
        val path = utf16("\\\\$host\\$share")
        val body = ByteWriter().u16(9).u16(0).u16(Smb2.HEADER + 8).u16(path.size).bytes(path).toByteArray()
        val response = request(Smb2.TREE_CONNECT, body)
        if (response.status != SmbStatus.SUCCESS) fail(response, share)
        return response.treeId
    }

    fun treeDisconnect(treeId: Long) {
        runCatching { request(Smb2.TREE_DISCONNECT, ByteWriter().u16(4).u16(0).toByteArray(), treeId) }
    }

    override fun close() {
        if (sessionId != 0L) runCatching { request(Smb2.LOGOFF, ByteWriter().u16(4).u16(0).toByteArray()) }
        transport.close()
    }

    companion object {
        fun open(transport: SmbTransport, host: String, credentials: SmbCredentials, guestFallback: Boolean): SmbSession {
            val session = SmbSession(transport, host)
            try {
                session.negotiate()
                try {
                    session.authenticate(credentials)
                } catch (problem: SmbException) {
                    if (!guestFallback || problem.status != SmbStatus.LOGON_FAILURE && problem.status != SmbStatus.ACCESS_DENIED) throw problem
                    session.authenticate(SmbCredentials.anonymous)
                }
            } catch (problem: Exception) {
                transport.close()
                throw problem
            }
            return session
        }
    }
}

data class SmbHandle(val persistent: Long, val volatile: Long, val attributes: Long, val size: Long, val modified: Long) {
    val isFolder: Boolean get() = (attributes and 0x10L) != 0L

    fun write(writer: ByteWriter) = writer.u64(persistent).u64(volatile)
}

class SmbShare(private val session: SmbSession, val share: String, private val treeId: Long) {
    val host: String get() = session.host

    private fun check(response: SmbResponse): SmbResponse {
        if (response.status != SmbStatus.SUCCESS) session.fail(response, share)
        return response
    }

    fun create(path: String, access: Long, disposition: Int, options: Int, attributes: Long = 0L, shareAccess: Int = 7): SmbHandle {
        val name = utf16(Smb2.wirePath(path))
        val writer = ByteWriter().u16(57).u8(0).u8(0).u32(2).u64(0).u64(0)
            .u32(access).u32(attributes).u32(shareAccess).u32(disposition).u32(options)
            .u16(Smb2.HEADER + 56).u16(name.size).u32(0).u32(0)
        writer.bytes(if (name.isEmpty()) ByteArray(1) else name)
        val response = check(session.request(Smb2.CREATE, writer.toByteArray(), treeId))
        val r = response.reader
        val b = Smb2.HEADER
        return SmbHandle(r.u64(b + 64), r.u64(b + 72), r.u32(b + 56), r.u64(b + 48), Smb2.fromFileTime(r.u64(b + 24)))
    }

    fun close(handle: SmbHandle) {
        val body = ByteWriter().u16(24).u16(0).u32(0)
        handle.write(body)
        runCatching { session.request(Smb2.CLOSE, body.toByteArray(), treeId) }
    }

    private inline fun <T> using(handle: SmbHandle, block: (SmbHandle) -> T): T = try {
        block(handle)
    } finally {
        close(handle)
    }

    fun stat(path: String): SmbEntry = using(create(path, 0x100080L, Smb2.FILE_OPEN, 0)) { handle ->
        SmbEntry(path.substringAfterLast('/'), handle.isFolder, if (handle.isFolder) 0L else handle.size, handle.modified, handle.attributes)
    }

    fun list(path: String): List<SmbEntry> = using(create(path, 0x100081L, Smb2.FILE_OPEN, Smb2.DIRECTORY_FILE)) { handle ->
        val entries = mutableListOf<SmbEntry>()
        val pattern = utf16("*")
        var first = true
        while (true) {
            val body = ByteWriter().u16(33).u8(0x01).u8(if (first) 0x01 else 0x00).u32(0)
            handle.write(body)
            body.u16(Smb2.HEADER + 32).u16(pattern.size).u32(Smb2.CHUNK).bytes(pattern)
            first = false
            val response = session.request(Smb2.QUERY_DIRECTORY, body.toByteArray(), treeId)
            if (response.status == SmbStatus.NO_MORE_FILES || response.status == SmbStatus.NO_SUCH_FILE) break
            check(response)
            val r = response.reader
            val offset = r.u16(Smb2.HEADER + 2)
            val length = r.u32(Smb2.HEADER + 4).toInt()
            if (length == 0) break
            var at = offset
            while (true) {
                val next = r.u32(at).toInt()
                val nameLength = r.u32(at + 60).toInt()
                val name = fromUtf16(r.bytes(at + 64, nameLength))
                val attributes = r.u32(at + 56)
                if (name != "." && name != "..") {
                    val folder = (attributes and 0x10L) != 0L
                    entries += SmbEntry(name, folder, if (folder) 0L else r.u64(at + 40), Smb2.fromFileTime(r.u64(at + 24)), attributes)
                }
                if (next == 0) break
                at += next
            }
        }
        entries
    }

    fun readTo(path: String, out: OutputStream, limit: Long = Long.MAX_VALUE, progress: (Long, Long) -> Boolean = { _, _ -> true }): Long =
        using(create(path, 0x100081L, Smb2.FILE_OPEN, Smb2.NON_DIRECTORY_FILE)) { handle ->
            if (handle.size > limit) throw SmbException(SmbStatus.TOO_LARGE, SmbStatus.message(SmbStatus.TOO_LARGE))
            var offset = 0L
            while (offset < handle.size || handle.size == 0L) {
                val body = ByteWriter().u16(49).u8(0x50).u8(0).u32(session.maxRead).u64(offset)
                handle.write(body)
                body.u32(0).u32(0).u32(0).u16(0).u16(0).u8(0)
                val response = session.request(Smb2.READ, body.toByteArray(), treeId)
                if (response.status == SmbStatus.END_OF_FILE) break
                check(response)
                val r = response.reader
                val dataOffset = r.u8(Smb2.HEADER + 2)
                val dataLength = r.u32(Smb2.HEADER + 4).toInt()
                if (dataLength == 0) break
                out.write(r.data, dataOffset, dataLength)
                offset += dataLength
                if (offset > limit) throw SmbException(SmbStatus.TOO_LARGE, SmbStatus.message(SmbStatus.TOO_LARGE))
                if (!progress(offset, handle.size)) throw SmbException(SmbStatus.CANCELLED, SmbStatus.message(SmbStatus.CANCELLED))
            }
            offset
        }

    fun writeFrom(path: String, input: InputStream, length: Long, progress: (Long, Long) -> Boolean = { _, _ -> true }): Long =
        using(create(path, 0x100183L, Smb2.FILE_OVERWRITE_IF, Smb2.NON_DIRECTORY_FILE, 0x80L)) { handle ->
            val buffer = ByteArray(session.maxWrite)
            var offset = 0L
            while (true) {
                var filled = 0
                while (filled < buffer.size) {
                    val n = input.read(buffer, filled, buffer.size - filled)
                    if (n < 0) break
                    filled += n
                }
                if (filled == 0) break
                val body = ByteWriter().u16(49).u16(Smb2.HEADER + 48).u32(filled).u64(offset)
                handle.write(body)
                body.u32(0).u32(0).u16(0).u16(0).u32(0).bytes(buffer.copyOf(filled))
                val response = check(session.request(Smb2.WRITE, body.toByteArray(), treeId))
                val written = response.reader.u32(Smb2.HEADER + 4)
                offset += written
                if (!progress(offset, length)) throw SmbException(SmbStatus.CANCELLED, SmbStatus.message(SmbStatus.CANCELLED))
                if (filled < buffer.size) break
            }
            offset
        }

    fun pipeRead(handle: SmbHandle): ByteArray {
        val body = ByteWriter().u16(49).u8(0x50).u8(0).u32(Smb2.CHUNK).u64(0)
        handle.write(body)
        body.u32(0).u32(0).u32(0).u16(0).u16(0).u8(0)
        val response = session.request(Smb2.READ, body.toByteArray(), treeId)
        if (response.status != SmbStatus.BUFFER_OVERFLOW) check(response)
        val r = response.reader
        return r.bytes(r.u8(Smb2.HEADER + 2), r.u32(Smb2.HEADER + 4).toInt())
    }

    fun pipeCall(handle: SmbHandle, request: ByteArray): ByteArray {
        val body = ByteWriter().u16(49).u16(Smb2.HEADER + 48).u32(request.size).u64(0)
        handle.write(body)
        body.u32(0).u32(0).u16(0).u16(0).u32(0).bytes(request)
        check(session.request(Smb2.WRITE, body.toByteArray(), treeId))
        return pipeRead(handle)
    }

    fun mkdir(path: String) {
        close(create(path, 0x100080L, Smb2.FILE_CREATE, Smb2.DIRECTORY_FILE, 0x10L))
    }

    private fun setInfo(handle: SmbHandle, infoClass: Int, buffer: ByteArray) {
        val body = ByteWriter().u16(33).u8(1).u8(infoClass).u32(buffer.size).u16(Smb2.HEADER + 32).u16(0).u32(0)
        handle.write(body)
        body.bytes(buffer)
        check(session.request(Smb2.SET_INFO, body.toByteArray(), treeId))
    }

    fun deleteOne(path: String, folder: Boolean) {
        val handle = create(path, 0x110080L, Smb2.FILE_OPEN, if (folder) Smb2.DIRECTORY_FILE else Smb2.NON_DIRECTORY_FILE)
        using(handle) { setInfo(it, 13, byteArrayOf(1)) }
    }

    fun delete(path: String) {
        val entry = stat(path)
        if (entry.isFolder) {
            for (child in list(path)) {
                val childPath = if (path.isEmpty()) child.name else "$path/${child.name}"
                if (child.isFolder) delete(childPath) else deleteOne(childPath, false)
            }
        }
        deleteOne(path, entry.isFolder)
    }

    fun rename(from: String, to: String) {
        val target = utf16(Smb2.wirePath(to))
        val buffer = ByteWriter().u8(0).zeros(7).u64(0).u32(target.size).bytes(target).toByteArray()
        using(create(from, 0x110080L, Smb2.FILE_OPEN, 0)) { setInfo(it, 10, buffer) }
    }

    fun disconnect() = session.treeDisconnect(treeId)
}

class SmbClient(
    val host: String,
    val share: String,
    private val credentials: SmbCredentials,
    private val guestFallback: Boolean = false,
    private val connector: () -> SmbTransport,
) : Closeable {
    private var session: SmbSession? = null
    private var tree: SmbShare? = null

    val guest: Boolean get() = session?.guest ?: false

    @Synchronized
    fun connect(): SmbShare {
        tree?.let { return it }
        val opened = SmbSession.open(connector(), host, credentials, guestFallback)
        try {
            val share = SmbShare(opened, share, opened.treeConnect(share))
            session = opened
            tree = share
            return share
        } catch (problem: Exception) {
            opened.close()
            throw problem
        }
    }

    @Synchronized
    fun <T> run(block: (SmbShare) -> T): T {
        val first = connect()
        return try {
            block(first)
        } catch (problem: SmbException) {
            if (problem.status != SmbStatus.UNREACHABLE && problem.status != SmbStatus.USER_SESSION_DELETED && problem.status != SmbStatus.NETWORK_SESSION_EXPIRED) throw problem
            drop()
            block(connect())
        }
    }

    @Synchronized
    private fun drop() {
        runCatching { session?.close() }
        session = null
        tree = null
    }

    @Synchronized
    override fun close() {
        tree?.disconnect()
        drop()
    }

    companion object {
        fun listShares(host: String, credentials: SmbCredentials, guestFallback: Boolean, transport: SmbTransport): List<String> {
            val session = SmbSession.open(transport, host, credentials, guestFallback)
            try {
                val treeId = session.treeConnect("IPC$")
                val ipc = SmbShare(session, "IPC$", treeId)
                return SrvSvc.shareEnum(ipc, host)
            } finally {
                session.close()
            }
        }
    }
}
